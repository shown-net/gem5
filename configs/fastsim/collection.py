"""Retired collection policy; native SPR timing objects are read, never tuned."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import re


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Native SPR retired-stream FS capture")
    p.add_argument("--fastsim-root", type=Path, required=True)
    p.add_argument("--workload-binary", required=True)
    p.add_argument("--workload-args", default="")
    p.add_argument("--participant-cores", "--omp-threads", dest="omp_threads", type=int, required=True)
    p.add_argument("--extra-cores", type=int, default=0)
    p.add_argument("--functional-user-target", type=int, default=10000000)
    p.add_argument("--functional-sync", action="store_true")
    p.add_argument("--thread-state", action="store_true")
    p.add_argument("--task-clock", action="store_true", help="observe independent native BaseCPU task cycles; requires --thread-state and a --task-clock scheduler profile")
    p.add_argument("--scheduler-profile", type=Path)
    p.add_argument("--sync-initial-context", choices=("empty", "unknown"), default="unknown")
    kernel = p.add_mutually_exclusive_group()
    kernel.add_argument("--kernel", type=Path, help="local gem5-compatible kernel ELF")
    kernel.add_argument("--kernel-resource", default="x86-linux-kernel-5.4.49")
    disk = p.add_mutually_exclusive_group()
    disk.add_argument("--disk", type=Path, help="local raw guest disk image")
    disk.add_argument("--disk-resource", default="x86-ubuntu-18.04-img")
    p.add_argument("--disk-root-partition", default="1")
    p.add_argument("--input-file", action="append", default=[])
    p.add_argument("--input-archive", action="append", default=[])
    p.add_argument("--staging-size", choices=("512m", "1g", "2g"), default="512m")
    p.add_argument("--prepare-workload-args", default="")
    p.add_argument("--rate-copies", type=int, default=0)
    p.add_argument("--native-workers", type=int, default=0)
    p.add_argument("--native-omp-num-threads", action="store_true")
    p.add_argument("--native-warmup-after-identities", action="store_true")
    p.add_argument("--rate-launcher", type=Path)
    p.add_argument("--rate-private-cwd", action="store_true")
    p.add_argument("--rate-serial-init", action="store_true")
    p.add_argument("--housekeeping-cpu", type=int)
    p.add_argument("--warmup-insts", type=int, default=0)
    p.add_argument("--warmup-steps", type=int)
    p.add_argument("--omp-proc-bind", default="")
    p.add_argument("--omp-places", default="")
    p.add_argument("--omp-wait-policy", choices=("", "ACTIVE", "PASSIVE"), default="")
    p.add_argument("--skip-workload-validation", action="store_true")
    p.add_argument("--trace-subdir", default="tao_trace")
    p.add_argument("--max-ticks", type=int, default=0)
    p.add_argument("--kvm-quantum-us", type=float, default=1000)
    p.add_argument("--mem-size", default="2GiB", help="guest RAM capacity; native DDR5 timings and channels stay fixed")
    p.add_argument("--x86-large-memory", action="store_true", help="explicit split-range RAM above the x86 PCI hole")
    # Donor workload helpers consume these protocol options. Reusing old staged
    # checkpoints with a different target would require a new identity contract.
    p.set_defaults(checkpoint_phase="staged", save_boot_checkpoint=None,
                   restore_boot_checkpoint=None, checkpoint_only=False,
                   trace_format="fst", functional_include_kernel=True,
                   functional_syscall_microops=True, functional_frontend=True,
                   functional_retirement=True, functional_asid_static_map=True, clk="2.6GHz",
                   mem_channels=16)
    return p.parse_args(argv)


def validate_options(args):
    if args.omp_threads <= 0 or args.extra_cores < 0:
        raise ValueError("participant count must be positive and spare count nonnegative")
    if args.functional_user_target <= 0 or args.warmup_insts < 0 or args.max_ticks < 0:
        raise ValueError("common-end target must be positive; warmup/tick limits nonnegative")
    if args.kvm_quantum_us <= 0:
        raise ValueError("KVM quantum must be positive")
    size = memory_size_bytes(args.mem_size)
    if size > 3 * 2**30 and not args.x86_large_memory:
        raise ValueError("RAM above 3GiB requires --x86-large-memory")
    if args.native_workers:
        if not args.task_clock or not args.thread_state:
            raise ValueError("native business tasks require --thread-state --task-clock")
        if not args.native_warmup_after_identities or args.warmup_insts <= 0:
            raise ValueError("native business tasks require --native-warmup-after-identities and positive warmup")
    if args.task_clock and not args.thread_state:
        raise ValueError("--task-clock requires --thread-state")
    if args.thread_state and not args.scheduler_profile:
        raise ValueError("--thread-state requires --scheduler-profile with an exact audited kernel")
    if args.scheduler_profile and not args.thread_state:
        raise ValueError("--scheduler-profile requires --thread-state")
    trace = Path(args.trace_subdir)
    if trace.is_absolute() or ".." in trace.parts or not trace.parts:
        raise ValueError("trace subdirectory must stay inside the output directory")


def memory_size_bytes(value):
    match = re.fullmatch(r"([0-9]+)(GiB|MiB|KiB|B)", value)
    if not match or int(match[1]) <= 0:
        raise ValueError("memory capacity must be a positive integer with GiB/MiB/KiB/B units")
    return int(match[1]) * {"GiB": 2**30, "MiB": 2**20, "KiB": 2**10, "B": 1}[match[2]]


def guest_memory_boot_script(args, original):
    size = memory_size_bytes(args.mem_size)
    if size <= 3 * 2**30:
        return original
    if not args.x86_large_memory:
        raise ValueError("guest high-memory visibility requires --x86-large-memory")
    first, separator, body = original.partition("\n")
    if first != "#!/bin/sh" or not separator:
        raise ValueError("guest boot script must begin with #!/bin/sh")
    minimum_kib = (size - 2**30) // 1024
    report = f'''set -e
swapoff -a
echo '[fastsim-spr-memory] begin visibility-only'
cat /proc/meminfo
cat /proc/iomem
dmesg | grep -E 'e820|BIOS-provided physical RAM map|Memory:' || true
awk '/^MemTotal:/ {{ if ($2 >= {minimum_kib}) good=1 }} END {{ exit !good }}' /proc/meminfo
awk '/System RAM/ {{ split($1, range, "-"); sub(/^0+/, "", range[1]); if (length(range[1]) > 8) good=1 }} END {{ exit !good }}' /proc/iomem
echo '[fastsim-spr-memory] visibility-passed minimum-kib={minimum_kib}'
echo '[fastsim-spr-memory] end'
'''
    return first + "\n" + report + body


def participant_ids(args):
    return list(range(args.omp_threads))


def load_donor(root):
    root = Path(root).resolve()
    path = root / "scripts/run_gem5_sync_probe.py"
    if not path.is_file() or not (root / "tools/generate_guest_scheduler_profile.py").is_file():
        raise ValueError("--fastsim-root does not contain the maintained collection helpers")
    sys.path.insert(0, str(root / "tools"))
    spec = importlib.util.spec_from_file_location("fastsim_legacy_collection_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def trace_options(args, trace_dir, profile_path, identity, thread_params=None):
    return dict(
        output_dir=str(trace_dir), uarch_profile_path=str(profile_path),
        emit_micro=True, emit_macro=False, emit_mem_events=False,
        require_roi=False, measure_cpl=True, trace_format="fst",
        functional_include_kernel=True, functional_syscall_microops=True,
        functional_frontend=True, functional_retirement=True, functional_asid_static_map=True,
        functional_task_clock=args.task_clock,
        functional_warmup=True, functional_user_target=args.functional_user_target,
        functional_sync_marker_pc=identity.get("marker_pc", 0),
        functional_sync_initial_context=args.sync_initial_context,
        **(thread_params or {}),
    )


def attach_traces(cpus, trace_class, options):
    for cpu in cpus:
        cpu.tao_trace = trace_class(manager=cpu, **options)


def native_profile(cpu, cache, memory, num_cores):
    """Read incorporated native objects, including real bank and fill geometry.

    The legacy observer profile is a diagnostic shadow; it does not represent
    Ruby/SLICC native cache truth or provide target timing inputs.
    """
    def cache_row(obj, banks=1):
        policy = {"LRURP": "lru", "TreePLRURP": "tree_plru"}.get(obj.replacement_policy.type)
        if policy is None:
            raise ValueError("unsupported observer replacement policy: " + obj.replacement_policy.type)
        return dict(size_b=int(obj.size) * banks, assoc=int(obj.assoc), line_b=64,
                    num_banks=banks, bank_select_low_bit=6, policy=policy)

    def tlb_row(obj, ways_name=None):
        size = int(obj.size)
        ways = int(getattr(obj, ways_name)) if ways_name else 0
        return dict(entries=size, assoc=ways or size)

    l1 = cache._l1_controllers[0]
    l2 = cache._l2_controllers[0]
    l3 = cache._l3_controllers[0]
    banks = len(cache._l3_controllers)
    dram = memory.get_memory_controllers()[0].dram
    profile = dict(
        schema_version=2,
        source="native SPR live SimObjects; legacy observer shadow, not native cache truth",
        core=dict(isa="X86", num_cores=num_cores, freq_ghz=2.6,
                  sq_entries=int(cpu.SQEntries)),
        cache=dict(l1d=cache_row(l1.Dcache), l1i=cache_row(l1.Icache),
                   l2=cache_row(l2.cache), l3=cache_row(l3.L2cache, banks)),
        tlb=dict(dtlb=tlb_row(cpu.mmu.dtb, "dtlb_4k_ways"),
                 itlb=tlb_row(cpu.mmu.itb, "itlb_4k_ways"), stlb=tlb_row(cpu.mmu.stlb)),
        page_walker=dict(levels=4, page_size_bits=12, walk_attaches_to="sequencer", pwc_entries=0),
        mshr=dict(l1d_entries=int(l1.Dcache.fill_capacity),
                  l2_entries=int(l2.cache.fill_capacity), l3_entries=int(l3.number_of_TBEs)),
        coherence=dict(protocol="MESI_Three_Level"),
        dram=dict(model=dram.type, size_b=int(memory.get_size()),
                  num_channels=len(memory.get_memory_controllers()),
                  banks_per_channel=int(dram.banks_per_rank) * int(dram.ranks_per_channel),
                  row_size_b=int(dram.device_rowbuffer_size) * int(dram.devices_per_rank),
                  burst_b=64, interleaving_size_b=64,
                  queue_window=int(dram.read_buffer_size)),
    )
    profile["observer_limitations"] = {
        "cache_fields": "legacy observer shadow; native truth requires conserved Ruby/SLICC summaries",
        "mshr_fields": "L1/L2 native data-fill credits; LLC native TBE budget",
        "page_walker": "x86 four-level 4KiB shadow, not complete native walk/cache identity",
    }
    profile["native_cpu"] = {
        name: int(getattr(cpu, name)) for name in (
            "fetchWidth", "decodeWidth", "renameWidth", "dispatchWidth", "issueWidth",
            "wbWidth", "commitWidth", "numROBEntries", "LQEntries", "SQEntries")}
    profile["native_cpu"]["iq_entries"] = [int(iq.numEntries) for iq in cpu.instQueues]
    return profile


def write_profile(path, cpu, cache, memory, num_cores):
    Path(path).write_text(json.dumps(native_profile(cpu, cache, memory, num_cores), indent=2) + "\n")
