# Copyright (c) 2022 The Regents of the University of California
# All rights reserved.
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


from m5.objects import (
    DMASequencer,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
    SrcClockDomain,
)

from ....coherence_protocol import CoherenceProtocol
from ....utils.override import overrides
from ....utils.requires import requires

requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL)

from ....isas import ISA
from ...boards.abstract_board import AbstractBoard
from ..abstract_cache_hierarchy import AbstractCacheHierarchy
from ..abstract_three_level_cache_hierarchy import (
    AbstractThreeLevelCacheHierarchy,
)
from .abstract_ruby_cache_hierarchy import AbstractRubyCacheHierarchy
from .caches.mesi_three_level.directory import Directory
from .caches.mesi_three_level.dma_controller import DMAController
from .caches.mesi_three_level.l1_cache import L1Cache
from .caches.mesi_three_level.l2_cache import L2Cache
from .caches.mesi_three_level.l3_cache import L3Cache
from .topologies.simple_pt2pt import SimplePt2Pt, SimpleUncore


from .topologies.spr_mesh import SPRMesh


class MESIThreeLevelCacheHierarchy(
    AbstractRubyCacheHierarchy, AbstractThreeLevelCacheHierarchy
):
    """A three-level private-L1-private-L2-shared-L3 MESI hierarchy.

    The on-chip network is a point-to-point all-to-all simple network.
    """

    def __init__(
        self,
        l1i_size: str,
        l1i_assoc: int,
        l1d_size: str,
        l1d_assoc: int,
        l2_size: str,
        l2_assoc: int,
        l3_size: str,
        l3_assoc: int,
        num_l3_banks: int,
        uncore_clock: str = None,
    ):
        AbstractRubyCacheHierarchy.__init__(self=self)
        AbstractThreeLevelCacheHierarchy.__init__(
            self,
            l1i_size=l1i_size,
            l1i_assoc=l1i_assoc,
            l1d_size=l1d_size,
            l1d_assoc=l1d_assoc,
            l2_size=l2_size,
            l2_assoc=l2_assoc,
            l3_size=l3_size,
            l3_assoc=l3_assoc,
        )

        self._num_l3_banks = num_l3_banks
        self._uncore_clock = uncore_clock

    @overrides(AbstractCacheHierarchy)
    def get_coherence_protocol(self):
        return CoherenceProtocol.MESI_THREE_LEVEL

    def incorporate_cache(self, board: AbstractBoard) -> None:
        super().incorporate_cache(board)
        cache_line_size = board.get_cache_line_size()

        self.ruby_system = RubySystem()
        # TODO(SPR-MSR): Validate native ROI core/uncore frequencies before
        # considering HWP, power limits or idle-state timing in this baseline.
        self.ruby_system.clk_domain = (
            SrcClockDomain(clock=self._uncore_clock,
                           voltage_domain=board.get_clock_domain().voltage_domain)
            if self._uncore_clock else board.get_clock_domain()
        )

        # MESI_Three_Level needs 3 virtual networks
        self.ruby_system.number_of_virtual_networks = 3

        self.ruby_system.network = (
            SPRMesh(self.ruby_system, self._spr_network_parameters)
            if hasattr(self, "_spr_network_parameters") else
            (SimpleUncore(self.ruby_system) if self._uncore_clock
             else SimplePt2Pt(self.ruby_system))
        )
        self.ruby_system.network.clk_domain = self.ruby_system.clk_domain
        self.ruby_system.network.number_of_virtual_networks = 3

        self._l1_controllers = []
        self._l2_controllers = []
        self._l3_controllers = []
        cores = board.get_processor().get_cores()
        for core_idx, core in enumerate(cores):
            l1_cache = L1Cache(
                l1i_size=self._l1i_size,
                l1i_assoc=self._l1i_assoc,
                l1d_size=self._l1d_size,
                l1d_assoc=self._l1d_assoc,
                network=self.ruby_system.network,
                core=core,
                cache_line_size=cache_line_size,
                target_isa=board.processor.get_isa(),
                clk_domain=board.get_clock_domain(),
            )

            l1_cache.sequencer = RubySequencer(
                version=core_idx,
                dcache=l1_cache.Dcache,
                max_outstanding_requests=128,
                clk_domain=l1_cache.clk_domain,
                ruby_system=self.ruby_system,
            )

            if board.has_io_bus():
                l1_cache.sequencer.connectIOPorts(board.get_io_bus())

            l1_cache.ruby_system = self.ruby_system

            core.connect_icache(l1_cache.sequencer.in_ports)
            core.connect_dcache(l1_cache.sequencer.in_ports)

            core.connect_walker_ports(
                l1_cache.sequencer.in_ports, l1_cache.sequencer.in_ports
            )

            # Connect the interrupt ports
            if board.get_processor().get_isa() == ISA.X86:
                int_req_port = l1_cache.sequencer.interrupt_out_port
                int_resp_port = l1_cache.sequencer.in_ports
                core.connect_interrupt(int_req_port, int_resp_port)
            else:
                core.connect_interrupt()

            self._l1_controllers.append(l1_cache)

            # For testing purpose, we use point-to-point topology. So, the
            # assigned cluster ID is ignored by ruby.
            # Thus, we set cluster_id to 0.
            l2_cache = L2Cache(
                l2_size=self._l2_size,
                l2_assoc=self._l2_assoc,
                network=self.ruby_system.network,
                core=core,
                num_l3Caches=self._num_l3_banks,
                cache_line_size=cache_line_size,
                cluster_id=0,
                target_isa=board.processor.get_isa(),
                clk_domain=board.get_clock_domain(),
            )

            l2_cache.ruby_system = self.ruby_system
            # L0Cache in the ruby backend is l1 cache in stdlib
            # L1Cache in the ruby backend is l2 cache in stdlib
            l2_cache.bufferFromL0 = l1_cache.bufferToL1
            l2_cache.bufferToL0 = l1_cache.bufferFromL1

            self._l2_controllers.append(l2_cache)

        for _ in range(self._num_l3_banks):
            l3_cache = L3Cache(
                l3_size=self._l3_size,
                l3_assoc=self._l3_assoc,
                network=self.ruby_system.network,
                num_l3Caches=self._num_l3_banks,
                cache_line_size=cache_line_size,
                cluster_id=0,  # cluster_id is ignored in point-to-point topology
            )
            l3_cache.ruby_system = self.ruby_system
            l3_cache.clk_domain = self.ruby_system.clk_domain
            self._l3_controllers.append(l3_cache)

        # TODO: Make this prettier: The problem is not being able to proxy
        # the ruby system correctly
        for cache in self._l3_controllers:
            cache.ruby_system = self.ruby_system

        # Multiple physical RAM ranges can share one subchannel timing port.
        # One directory per port retains the same controller/queue ownership.
        directories = {}
        for mem_range, port in board.get_mem_ports():
            key = id(port)
            if key in directories:
                directories[key].addr_ranges = [*directories[key].addr_ranges, mem_range]
            else:
                directories[key] = Directory(self.ruby_system.network,
                                              cache_line_size, mem_range, port)
        self._directory_controllers = list(directories.values())
        # TODO: Make this prettier: The problem is not being able to proxy
        # the ruby system correctly
        for dir in self._directory_controllers:
            dir.ruby_system = self.ruby_system
            dir.clk_domain = self.ruby_system.clk_domain

        self._dma_controllers = []
        if board.has_dma_ports():
            dma_ports = board.get_dma_ports()
            for i, port in enumerate(dma_ports):
                ctrl = DMAController(
                    DMASequencer(
                        version=i,
                        in_ports=port,
                        ruby_system=self.ruby_system,
                    ),
                    self.ruby_system,
                )
                ctrl.clk_domain = self.ruby_system.clk_domain
                self._dma_controllers.append(ctrl)

        self.ruby_system.num_of_sequencers = len(self._l1_controllers) + len(
            self._dma_controllers
        )
        self.ruby_system.l1_controllers = self._l1_controllers
        self.ruby_system.l2_controllers = self._l2_controllers
        self.ruby_system.l3_controllers = self._l3_controllers
        self.ruby_system.directory_controllers = self._directory_controllers

        if len(self._dma_controllers) != 0:
            self.ruby_system.dma_controllers = self._dma_controllers

        # Create the network and connect the controllers.
        controllers = (self._l1_controllers + self._l2_controllers +
                       self._l3_controllers + self._directory_controllers +
                       self._dma_controllers)
        if hasattr(self, "_spr_network_parameters"):
            self.ruby_system.network.connectControllers(
                controllers, core_count=len(self._l1_controllers),
                bank_count=len(self._l3_controllers),
                directory_count=len(self._directory_controllers),
                channels=board.get_memory()._num_channels)
        else:
            self.ruby_system.network.connectControllers(controllers)
        self.ruby_system.network.setup_buffers()

        # Set up a proxy port for the system_port. Used for load binaries and
        # other functional-only things.
        self.ruby_system.sys_port_proxy = RubyPortProxy(
            ruby_system=self.ruby_system
        )
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    @overrides(AbstractRubyCacheHierarchy)
    def _reset_version_numbers(self):
        Directory._version = 0
        L1Cache._version = 0
        L2Cache._version = 0
        L3Cache._version = 0
        DMAController._version = 0


class SPRCacheHierarchy(MESIThreeLevelCacheHierarchy):
    """Selected SPR private caches with system-supplied LLC geometry."""

    def __init__(self, *, l3_size: str, l3_assoc: int, num_l3_banks: int,
                 l1i_size: str, l1i_assoc: int, l1d_size: str, l1d_assoc: int,
                 l2_size: str, l2_assoc: int, uncore_clock: str,
                 l1_refill_bytes_per_cycle: int, l1_fill_response_width: int,
                 l1_fill_capacity: int, l1_write_fill_capacity: int,
                 l1d_data_access_cycles: int, l2_fill_capacity: int,
                 sequencer_max_outstanding_requests: int,
                 l1_tbe_capacity: int, l2_tbe_capacity: int,
                 network_parameters: dict):
        super().__init__(l1i_size=l1i_size, l1i_assoc=l1i_assoc,
                         l1d_size=l1d_size, l1d_assoc=l1d_assoc,
                         l2_size=l2_size, l2_assoc=l2_assoc,
                         l3_size=l3_size, l3_assoc=l3_assoc,
                         num_l3_banks=num_l3_banks, uncore_clock=uncore_clock)
        self._spr_network_parameters = dict(network_parameters)
        self._l1_refill_bytes_per_cycle = l1_refill_bytes_per_cycle
        self._l1_fill_capacity = l1_fill_capacity
        self._l1_write_fill_capacity = l1_write_fill_capacity
        self._l1_fill_response_width = l1_fill_response_width
        self._l1d_data_access_cycles = l1d_data_access_cycles
        self._l2_fill_capacity = l2_fill_capacity
        self._sequencer_max_outstanding_requests = sequencer_max_outstanding_requests
        self._l1_tbe_capacity = l1_tbe_capacity
        self._l2_tbe_capacity = l2_tbe_capacity

    def incorporate_cache(self, board: AbstractBoard) -> None:
        super().incorporate_cache(board)
        for controller in self._l1_controllers:
            # Native dependent L1 loads take five issue-to-issue cycles.
            # Three Ruby service cycles plus the AGU/LSU handoffs reproduce
            # that interval; independent three-load throughput is unchanged.
            controller.Dcache.dataAccessLatency = self._l1d_data_access_cycles
            controller.Dcache.fill_install_bytes_per_cycle = self._l1_refill_bytes_per_cycle
            controller.Dcache.fill_capacity = self._l1_fill_capacity
            controller.Dcache.write_fill_capacity = self._l1_write_fill_capacity
            controller.sequencer.data_fill_response_width = self._l1_fill_response_width
            controller.sequencer.max_outstanding_requests = self._sequencer_max_outstanding_requests
            controller.number_of_TBEs = self._l1_tbe_capacity
        for controller in self._l2_controllers:
            controller.cache.fill_capacity = self._l2_fill_capacity
            controller.number_of_TBEs = self._l2_tbe_capacity
