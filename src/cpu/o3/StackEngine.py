# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject


class StackEngine(SimObject):
    type = "StackEngine"
    cxx_class = "gem5::o3::StackEngine"
    cxx_header = "cpu/o3/stack_engine.hh"

    # Research prototype: do not activate it implicitly in general runs.
    enabled = Param.Bool(False, "Enable speculative x86 stack-pointer tracking")
    minOffsetBytes = Param.Int(-4096, "Candidate minimum tracked byte offset")
    maxOffsetBytes = Param.Int(4096, "Candidate maximum tracked byte offset")
    minImmediate = Param.Int(-1024, "Minimum tracked signed immediate")
    maxImmediate = Param.Int(1023, "Maximum tracked signed immediate")
    syncOnZero = Param.Bool(False, "Emit sync for unsupported RSP uses at zero offset")
