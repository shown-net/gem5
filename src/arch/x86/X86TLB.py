# Copyright (c) 2007 The Hewlett-Packard Development Company
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.BaseTLB import BaseTLB
from m5.objects.ClockedObject import ClockedObject
from m5.SimObject import SimObject
from m5.params import *
from m5.proxy import *


class X86PagingStructureCache(SimObject):
    type = "X86PagingStructureCache"
    cxx_class = "gem5::X86ISA::PagingStructureCache"
    cxx_header = "arch/x86/paging_structure_cache.hh"
    entriesPerLevel = Param.Unsigned(16, "Shared entries per upper paging level; engineering prior")


class X86PagetableWalker(ClockedObject):
    type = "X86PagetableWalker"
    cxx_class = "gem5::X86ISA::Walker"
    cxx_header = "arch/x86/pagetable_walker.hh"

    port = RequestPort("Port for the hardware table walker")
    system = Param.System(Parent.any, "system object")
    num_squash_per_cycle = Param.Unsigned(
        4, "Number of outstanding walks that can be squashed per cycle"
    )
    max_active_walks = Param.Unsigned(1, "Concurrent timing page walks")
    coalesce_fault_results = Param.Bool(
        False, "Complete joined translations from the same page-walk fault result"
    )
    cancel_squashed_walks = Param.Bool(
        False, "Stop further PTE reads when all walk users have been squashed"
    )
    paging_structure_cache = Param.X86PagingStructureCache(NULL, "Optional shared virtual-prefix cache")
    prefix_hit_latency = Param.Cycles(1, "Upper-prefix hit service latency")
    pte_response_latency = Param.Cycles(
        0, "PTE read response processing before the next walk step"
    )


class X86TLB(BaseTLB):
    type = "X86TLB"
    cxx_class = "gem5::X86ISA::TLB"
    cxx_header = "arch/x86/tlb.hh"

    size = Param.Unsigned(64, "TLB size")
    itlb_4k_ways = Param.Unsigned(0, "4K instruction TLB ways; zero uses fully associative replacement")
    dtlb_4k_ways = Param.Unsigned(0, "4K data TLB ways; zero uses fully associative replacement")
    spr_shared_domains = Param.Bool(False, "Two 1024-entry page-size support domains")
    spr_shared_index_shift = Param.Unsigned(
        0, "4K VPN XOR fold: 8 folds every byte; other nonzero values fold once"
    )
    spr_split_l1 = Param.Bool(False, "SPR first-level page-size and data-access replacement domains")
    next_level_hit_latency = Param.Cycles(7, "Modeled next-level TLB hit latency")
    system = Param.System(Parent.any, "system object")
    walker = Param.X86PagetableWalker(
        X86PagetableWalker(), "page table walker"
    )
