"""Small SE system for the O3 wakeup regression, not a cache model."""
import sys

import m5
from m5.objects import *

system = System()
system.clk_domain = SrcClockDomain(clock="2.6GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]
system.cpu = X86O3CPU()
system.membus = NoncoherentXBar(width=32, frontend_latency=1,
                               forward_latency=0, response_latency=1)
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="50ns")
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.workload = SEWorkload.init_compatible(sys.argv[1])
process = Process(cmd=[sys.argv[1]])
system.cpu.workload = process
system.cpu.createThreads()
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate()
print(f"probe exit at tick {m5.curTick()}: {event.getCause()}")
