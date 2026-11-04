"""Capacity-only SPR RAM adapter; native DDR5 parameters remain unchanged.

Reuse the maintained split-range controller/E820 algorithm, selecting DDR5
before any interfaces are created. Two ranges have 16 channels each (32 actual
controllers/directories), not one 16-controller set covering a PCI hole.
"""
from gem5.components.boards.x86_board import X86Board
from gem5.components.boards.x86_large_memory_board import X86LargeMemoryBoard
from gem5.components.memory.dram_interfaces.ddr5 import DDR5_4400_4x8
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.memory.x86_multirange_memory import X86MultiRangeDDR4Memory

from collection import memory_size_bytes


class NativeSPRMultiRangeMemory(X86MultiRangeDDR4Memory):
    def __init__(self, size):
        self._x86_ranges = []
        ChanneledMemory.__init__(self, DDR5_4400_4x8, 16, 64, size=size)


def make_memory(args):
    if memory_size_bytes(args.mem_size) > 3 * 2**30:
        if not args.x86_large_memory:
            raise ValueError("RAM above 3GiB requires --x86-large-memory")
        return X86LargeMemoryBoard, NativeSPRMultiRangeMemory(args.mem_size)
    return X86Board, ChanneledMemory(DDR5_4400_4x8, 16, 64, size=args.mem_size)
