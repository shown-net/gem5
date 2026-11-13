"""One DDR5 timing controller per physical subchannel, across RAM holes."""
from math import log2
from m5.objects import SimpleMemory
from m5.params import AddrRange
from .memory import ChanneledMemory
from .dram_interfaces.ddr5 import DDR5_4400_4x8


class SPRMemory(ChanneledMemory):
    def __init__(self, channels=16, interleaving=64, size="2GiB"):
        self._physical_ranges = []
        super().__init__(DDR5_4400_4x8, channels, interleaving, size=size)

    def set_memory_range(self, ranges):
        ranges = sorted(ranges, key=lambda r: int(r.start))
        if not ranges or sum(r.size() for r in ranges) != self._size:
            raise ValueError("RAM ranges must cover the configured capacity")
        stripe = self._intlv_size * self._num_channels
        if any(r.intlvBits or int(r.start) % stripe or r.size() % stripe for r in ranges):
            raise ValueError("RAM ranges must be aligned to complete channel stripes")
        if any(int(a.end) >= int(b.start) for a, b in zip(ranges, ranges[1:])):
            raise ValueError("RAM ranges overlap")
        self._physical_ranges = ranges
        bits = int(log2(self._num_channels))
        high = int(log2(self._intlv_size)) + bits - 1
        for channel, controller in enumerate(self.mem_ctrl):
            parts = [AddrRange(start=r.start, size=r.size(), intlvHighBit=high,
                               xorHighBit=0, intlvBits=bits, intlvMatch=channel)
                     for r in ranges]
            controller.dram.range = parts[0]
            controller.dram.channel_ranges = parts
            # Data backings expose the real physical ranges to KVM and system
            # functional access. They have no connected timing ports: all
            # requests share this controller's queues and DRAM bank state.
            controller.range_backings = [SimpleMemory(range=r) for r in parts[1:]]

    def get_mem_ports(self):
        return [(part, controller.port) for index in range(len(self._physical_ranges))
                for controller in self.mem_ctrl
                for part in [list(controller.dram.channel_ranges)[index]]]

    def get_uninterleaved_range(self):
        return list(self._physical_ranges)
