# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject


class InstructionBytes(SimObject):
    type = "InstructionBytes"
    cxx_class = "gem5::o3::InstructionBytes"
    cxx_header = "cpu/o3/frontend.hh"
    bufferBytes = Param.Unsigned(64, "Bytes in each resident refill block")
    residentBlocks = Param.Unsigned(4, "Bounded resident instruction refill blocks")
    requestDepth = Param.Unsigned(2, "Maximum demand plus predicted refill requests")
    bytesPerCycle = Param.Unsigned(32, "One aligned byte-service block")
    lookaheadEnabled = Param.Bool(True, "Overlap the next predicted refill with decode")


class InstructionDeliveryQueue(SimObject):
    type = "InstructionDeliveryQueue"
    cxx_class = "gem5::o3::InstructionDeliveryQueue"
    cxx_header = "cpu/o3/frontend.hh"
    capacity = Param.Unsigned(144, "Queued plus sent but unaccepted IDQ units")


class MiteService(SimObject):
    type = "MiteService"
    cxx_class = "gem5::o3::MiteService"
    cxx_header = "cpu/o3/frontend.hh"
    instructionsPerBatch = Param.Unsigned(6, "Machine instruction accept budget")
    opsPerCycle = Param.Unsigned(6, "Frontend unit delivery budget")
    batchOps = Param.Unsigned(64, "Maximum buffered expansion fragment")
    latency = Param.Cycles(1, "Service completion latency")


class MicrocodeService(MiteService):
    type = "MicrocodeService"
    cxx_class = "gem5::o3::MicrocodeService"
    cxx_header = "cpu/o3/frontend.hh"
    instructionsPerBatch = 1
    opsPerCycle = 4


class DsbTemplates(MiteService):
    type = "DsbTemplates"
    cxx_class = "gem5::o3::DsbTemplates"
    cxx_header = "cpu/o3/frontend.hh"
    instructionsPerBatch = 64
    opsPerCycle = 8
    numSets = Param.Unsigned(64, "Decoded-template sets, indexed by PC[11:6]")
    associativity = Param.Unsigned(8, "Entries per decoded-template set")
    opsPerEntry = Param.Unsigned(8, "Maximum proxy operations per entry")
    maxRegionOps = Param.Unsigned(
        0, "Encoded units per 64-byte code region; zero disables the limit")


class FrontendCoordinator(SimObject):
    type = "FrontendCoordinator"
    cxx_class = "gem5::o3::FrontendCoordinator"
    cxx_header = "cpu/o3/frontend.hh"
    enableLsdReplay = Param.Bool(True, "Enable simple-loop replay")
    instructionBytes = Param.InstructionBytes(InstructionBytes(), "Byte service")
    mite = Param.MiteService(MiteService(), "Legacy decode service")
    dsb = Param.DsbTemplates(DsbTemplates(), "Decoded template service")
    ms = Param.MicrocodeService(MicrocodeService(), "Microcode operation service")
    lsd = Param.MiteService(
        MiteService(instructionsPerBatch=144, batchOps=144),
        "Simple-loop replay service; eligibility is a bounded model proxy",
    )
    idq = Param.InstructionDeliveryQueue(InstructionDeliveryQueue(), "Ordered IDQ")
