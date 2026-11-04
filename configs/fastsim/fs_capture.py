"""Native SPR FS capture using legacy workload markers and a shared common end."""
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from collection import (
    attach_traces, load_donor, parse_args, participant_ids, sha256,
    trace_options, validate_options, write_profile,
)


def main():
    args = parse_args()
    validate_options(args)
    # Imports are delayed so policy tests and --help work without gem5.
    import m5
    from m5.objects import TaoTrace
    from gem5.components.boards.x86_board import X86Board
    from gem5.components.cachehierarchies.ruby.mesi_three_level_cache_hierarchy import SPRCacheHierarchy
    from gem5.components.memory.dram_interfaces.ddr5 import DDR5_4400_4x8
    from gem5.components.memory.memory import ChanneledMemory
    from gem5.components.processors.cpu_types import CPUTypes
    from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor
    from gem5.isas import ISA
    from gem5.resources.resource import DiskImageResource, KernelResource, obtain_resource
    from gem5.simulate.exit_event import ExitEvent
    from gem5.simulate.simulator import Simulator

    donor = load_donor(args.fastsim_root)
    donor.validate_rate_options(args)
    identity = dict(binary=str(Path(args.workload_binary).resolve()),
                    binary_sha256=sha256(args.workload_binary))
    if args.functional_sync:
        identity.update(donor.sync_binary_identity(args.workload_binary))
    kernel = (KernelResource(local_path=str(args.kernel.resolve())) if args.kernel
              else obtain_resource(args.kernel_resource))
    disk = (DiskImageResource(local_path=str(args.disk.resolve()),
                              root_partition=args.disk_root_partition) if args.disk
            else obtain_resource(args.disk_resource))
    thread_params, scheduler_identity = {}, None
    if args.thread_state:
        wait = donor.static_marker_identity(args.workload_binary, "fastsim_wait_marker",
                                            "fastsim-wait-marker-v1", weak_only=True)
        identity["thread_state_marker_pc"] = wait["marker_pc"]
        identity["thread_state_marker_abi"] = wait["marker_abi"]
        # Re-check actual kernel bytes and machine-code witnesses, never trust
        # profile offsets alone. Unknown/host5.15 builds fail in this helper.
        thread_params, scheduler_identity = donor.thread_state_profile(
            args.scheduler_profile, kernel.get_local_path(), task_clock=args.task_clock)
        thread_params["thread_state_marker_pc"] = wait["marker_pc"]

    outdir = Path(m5.options.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    trace_dir = outdir / args.trace_subdir
    trace_dir.mkdir(parents=True, exist_ok=True)
    profile_path = outdir / "uarch_profile.json"
    num_cores = args.omp_threads + args.extra_cores

    class CaptureProcessor(SimpleSwitchableProcessor):
        def switch(self):
            super().switch()
            # Match the framework's KVM -> native detailed handoff: the O3
            # cores use queue0, without periodic KVM quantum synchronization.
            from _m5.event import enterSingleEventQueueMode
            from m5.objects import Root
            enterSingleEventQueueMode()
            Root.getInstance().sim_quantum = 0

        def _pre_instantiate(self, root):
            super()._pre_instantiate(root)
            root.sim_quantum = m5.ticks.fromSeconds(args.kvm_quantum_us * 1e-6)
            # Board incorporation has created the actual Ruby controllers.
            write_profile(profile_path, self._switchable_cores["switch"][0].get_simobject(),
                          cache, memory, num_cores)

    processor = CaptureProcessor(starting_core_type=CPUTypes.KVM,
                                 switch_core_type=CPUTypes.O3,
                                 num_cores=num_cores, isa=ISA.X86)
    # Native CPU classes carry SPR frontend/allocation/partitioned-IQ/store
    # behavior. No generic O3 setter belongs in this entry.
    memory = ChanneledMemory(DDR5_4400_4x8, 16, 64, size="2GiB")
    cache = SPRCacheHierarchy(l3_size="12MiB", l3_assoc=12, num_l3_banks=8)
    board = X86Board(clk_freq="2.6GHz", processor=processor,
                     memory=memory, cache_hierarchy=cache)
    board.set_kernel_disk_workload(kernel=kernel, disk_image=disk,
                                   readfile_contents=donor.boot_script(args),
                                   exit_on_work_items=True)
    traced = [wrapped.get_simobject() for wrapped in
              processor._switchable_cores["switch"][:args.omp_threads]]
    attach_traces(traced, TaoTrace,
                  trace_options(args, trace_dir, profile_path, identity, thread_params))
    from roi_capture_policy import BoundedWarmup
    from spec_rate_checkpoint import RateCheckpointFlow
    warmup = BoundedWarmup(args.warmup_insts,
                           defer_until_identities=args.native_warmup_after_identities)
    checkpoint_flow = RateCheckpointFlow("staged", False)
    state = {"switched": False, "common_end": False, "measurement_open": False}

    def milestone(phase):
        with (outdir / "capture-phases.jsonl").open("a") as stream:
            stream.write(json.dumps(dict(phase=phase, wall_time=time.time(),
                                         tick=int(m5.curTick()))) + "\n")

    def on_checkpoint():
        if args.rate_copies:
            action = checkpoint_flow.checkpoint()
            if action in ("continue", "save"):
                milestone("staged-before-fork")
                return False
        if not state["switched"]:
            print("[fastsim-spr] CHECKPOINT marker: switch KVM -> O3+Ruby", flush=True)
            milestone("switch-begin")
            simulator.switch_processor()
            state["switched"] = True
            milestone("switch-complete")
            if warmup.checkpoint() and args.warmup_insts:
                traced[0].scheduleInstStopAnyThread(args.warmup_insts)
        elif args.native_warmup_after_identities:
            warmup.identities_ready()
            milestone("native-identities-ready")
            traced[0].scheduleInstStopAnyThread(args.warmup_insts)
        return False

    def on_workbegin():
        # The workload emits both a work-item m5op and a serial marker. UART
        # delivery may lag behind the m5op, and a work-item ID may be zero.
        # Only the producers' actual functional boundary disambiguates them.
        work_item = simulator.get_last_exit_event_code()
        started = [cpu.tao_trace.functionalMeasurementStarted() for cpu in traced]
        if not any(started):
            print(f"[fastsim-spr] raw WORKBEGIN work-item {work_item}: notification only", flush=True)
            milestone("raw-work-item-notification")
            return False
        if not all(started):
            raise RuntimeError("inconsistent participant functional measurement state")
        if state["measurement_open"]:
            # A delayed raw work-item notification can arrive after the UART
            # marker. Its code cannot prove a second serial marker occurred.
            milestone("work-item-notification-after-measurement")
            return False
        if not state["switched"] or args.warmup_insts:
            raise RuntimeError("unexpected workload WORKBEGIN outside switched marker mode")
        state["measurement_open"] = True
        print("[fastsim-spr] WORKBEGIN serial marker: measurement window open", flush=True)
        # The serial terminal already invoked traceMeasurementBegin globally.
        m5.stats.reset()
        milestone("measurement-open")
        return False

    def on_max_insts():
        if warmup.instruction_exit() == "open-measurement":
            traced[0].tao_trace.startFunctionalMeasurement()
            state["measurement_open"] = True
            print("[fastsim-spr] WARMUP instruction budget reached: measurement window open", flush=True)
            m5.stats.reset()
            milestone("measurement-open")
            return False
        if not state["measurement_open"]:
            raise RuntimeError("common-end instruction exit without an opened measurement")
        state["common_end"] = True
        print("[fastsim-spr] functional_user_target reached: common-end, finish", flush=True)
        milestone("common-end")
        return True

    def on_exit():
        milestone("guest-exit-before-common-end")
        return True

    simulator = Simulator(board=board, on_exit_event={
        ExitEvent.CHECKPOINT: on_checkpoint, ExitEvent.WORKBEGIN: on_workbegin,
        ExitEvent.MAX_INSTS: on_max_insts, ExitEvent.EXIT: on_exit,
    })
    request = dict(
        schema="fastsim-spr-collection-request-v1", diagnostic=True,
        participant_core_ids=participant_ids(args),
        topology="legacy-capture: all guest CPUs are O3 after KVM handoff; no reserved Timing CPU0",
        common_end="first-participant measured-user-UOP target closes every attached stream",
        arguments={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        binary_identity=identity, scheduler_identity=scheduler_identity,
        kernel=dict(path=kernel.get_local_path(), sha256=sha256(kernel.get_local_path())),
        disk=dict(path=disk.get_local_path(), root_partition=disk.get_root_partition()),
        collector_helpers=dict(path=str(args.fastsim_root / "scripts/run_gem5_sync_probe.py"),
                               sha256=sha256(args.fastsim_root / "scripts/run_gem5_sync_probe.py")),
    )
    (outdir / "sync-run-request.json").write_text(json.dumps(request, indent=2) + "\n")
    print("[fastsim-spr] native SPR capture participants=" + str(participant_ids(args)), flush=True)
    milestone("boot-start")
    simulator.run(max_ticks=args.max_ticks or None)
    result = dict(cause=simulator.get_last_exit_event_cause(),
                  exit_code=simulator.get_last_exit_event_code(),
                  tick=simulator.get_current_tick(), **state)
    (outdir / "sync-run-result.json").write_text(json.dumps(result, indent=2) + "\n")
    if not state["common_end"]:
        raise RuntimeError("capture stopped before native common end; source is incomplete")


if __name__ in ("__m5_main__", "__main__"):
    main()
