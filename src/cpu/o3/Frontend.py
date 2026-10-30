# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject


class InstructionBytes(SimObject):
    type = "InstructionBytes"
    cxx_class = "gem5::o3::InstructionBytes"
    cxx_header = "cpu/o3/frontend.hh"
    bufferBytes = Param.Unsigned(64, "Resident instruction bytes")
    bytesPerCycle = Param.Unsigned(32, "One aligned byte-service block")


class InstructionDeliveryQueue(SimObject):
    type = "InstructionDeliveryQueue"
    cxx_class = "gem5::o3::InstructionDeliveryQueue"
    cxx_header = "cpu/o3/frontend.hh"
    capacity = Param.Unsigned(144, "Queued plus sent but unaccepted gem5 ops")


class MiteService(SimObject):
    type = "MiteService"
    cxx_class = "gem5::o3::MiteService"
    cxx_header = "cpu/o3/frontend.hh"
    instructionsPerBatch = Param.Unsigned(6, "Machine instruction accept budget")
    opsPerCycle = Param.Unsigned(8, "Gem5 operation delivery budget")
    batchOps = Param.Unsigned(64, "Maximum buffered expansion fragment")
    latency = Param.Cycles(1, "Service completion latency")


class DsbTemplates(MiteService):
    type = "DsbTemplates"
    cxx_class = "gem5::o3::DsbTemplates"
    cxx_header = "cpu/o3/frontend.hh"
    instructionsPerBatch = 64
    capacity = Param.Unsigned(4096, "Whole-template gem5 operation capacity")


class FrontendCoordinator(SimObject):
    type = "FrontendCoordinator"
    cxx_class = "gem5::o3::FrontendCoordinator"
    cxx_header = "cpu/o3/frontend.hh"
    instructionBytes = Param.InstructionBytes(InstructionBytes(), "Byte service")
    mite = Param.MiteService(MiteService(), "Legacy decode service")
    dsb = Param.DsbTemplates(DsbTemplates(), "Decoded template service")
    idq = Param.InstructionDeliveryQueue(InstructionDeliveryQueue(), "Ordered IDQ")
    dsbToMiteDelay = Param.Cycles(0, "Extra source-side DSB to MITE wait")
