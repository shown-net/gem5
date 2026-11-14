"""Exercise low/high RAM through the same sixteen timing controllers."""
import sys
import m5
from m5.objects import Root, System, SrcClockDomain, VoltageDomain, NoncoherentXBar, MemTest
from m5.params import AddrRange
from gem5.components.memory.spr_memory import SPRMemory

mode = sys.argv[1] if len(sys.argv) > 1 else "timing"
system = System(mem_mode=mode)
system.clk_domain = SrcClockDomain(clock="2.6GHz", voltage_domain=VoltageDomain())
system.mem_ranges = [AddrRange(0, size="3GiB"), AddrRange(4 * 2**30, size="1GiB")]
system.memory = SPRMemory(channels=16, interleaving=64, size="4GiB")
system.memory.set_memory_range(system.mem_ranges)
controllers = system.memory.get_memory_controllers()
assert len(controllers) == 16
assert all(len(c.dram.channel_ranges) == 2 for c in controllers)
system.memories = [m for c in controllers for m in (c.dram, *c.range_backings)]
system.bus = NoncoherentXBar(width=64, frontend_latency=0, forward_latency=0,
                            response_latency=0)
system.system_port = system.bus.cpu_side_ports
for controller in controllers:
    controller.port = system.bus.mem_side_ports
system.tester = MemTest(interval=16, base_addr_1=0x100000, base_addr_2=0x100100000,
                        percent_uncacheable=0, percent_functional=25,
                        percent_reads=65, max_loads=10000)
system.tester.port = system.bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate()
assert "maximum number of loads" in event.getCause(), event.getCause()
print(f"Shared sixteen-channel low/high RAM {mode} reads and writes verified")
