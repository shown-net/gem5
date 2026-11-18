"""Exercise populated x86 DTLB checkpoint state with a fixed SE fixture.

Run save, reference and restore using the same binary and checkpoint policy.
The caller compares post-checkpoint translation misses and checkpoint slots.
"""
import argparse
import configparser
from pathlib import Path

import m5
from m5.objects import (AddrRange, Process, Root, SEWorkload, SimpleMemory,
                        SrcClockDomain, System, SystemXBar, TimingSimpleCPU,
                        VoltageDomain, X86MMU, X86TLB)

parser = argparse.ArgumentParser()
parser.add_argument("mode", choices=("save", "reference", "restore"))
parser.add_argument("binary", type=Path)
parser.add_argument("checkpoint", type=Path)
args = parser.parse_args()

system = System()
system.clk_domain = SrcClockDomain(clock="2.6GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]
system.membus = SystemXBar()
system.cpu = TimingSimpleCPU(mmu=X86MMU(dtb=X86TLB(
    entry_type="data", size=96, dtlb_4k_ways=6, spr_split_l1=True,
    spr_dtlb_4k_pair_replacement=True)))
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.mmu.itb.walker.port = system.membus.cpu_side_ports
system.cpu.mmu.dtb.walker.port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="1ns")
system.memory.port = system.membus.mem_side_ports
system.workload = SEWorkload.init_compatible(str(args.binary))
system.cpu.workload = Process(cmd=[str(args.binary)])
system.cpu.createThreads()
if args.mode != "restore":
    system.cpu.max_insts_any_thread = 8
root = Root(full_system=False, system=system)
m5.instantiate(str(args.checkpoint) if args.mode == "restore" else None)

if args.mode != "restore":
    event = m5.simulate()
    assert "instruction count" in event.getCause(), event.getCause()
    m5.checkpoint(str(args.checkpoint))
    state = configparser.ConfigParser()
    state.read(args.checkpoint / "m5.cpt")
    dtlb = state["system.cpu.mmu.dtb"]
    assert int(dtlb["_size"]) == 6
    assert [int(dtlb[f"slot{i}"]) for i in range(6)] == list(range(6))
    assert int(dtlb["dtlb4kPairVersion"]) == 1
    assert int(dtlb["smallPageNextPair0"]) == 2
    if args.mode == "save":
        print("Populated DTLB checkpoint saved")
        raise SystemExit(0)

m5.stats.reset()
event = m5.simulate()
assert event.getCode() == 0, (event.getCause(), event.getCode())
assert "exiting" in event.getCause(), event.getCause()
m5.stats.dump()
stats = {}
for line in (Path(m5.options.outdir) / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) > 1 and fields[0].startswith("system.cpu.mmu.dtb."):
        stats[fields[0]] = fields[1]
assert int(stats["system.cpu.mmu.dtb.rdAccesses"]) == 3
assert int(stats["system.cpu.mmu.dtb.rdMisses"]) == 3
print("Post-checkpoint translation sequence completed")
