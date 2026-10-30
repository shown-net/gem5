# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject


class ExecutionPort(SimObject):
    type = "ExecutionPort"
    cxx_class = "gem5::o3::ExecutionPort"
    cxx_header = "cpu/o3/int_execution.hh"
    number = Param.Unsigned("Execution port number")


class ExecutionPorts(SimObject):
    type = "ExecutionPorts"
    cxx_class = "gem5::o3::ExecutionPorts"
    cxx_header = "cpu/o3/int_execution.hh"
    ports = VectorParam.ExecutionPort(
        [ExecutionPort(number=i) for i in range(12)], "Shared port instances"
    )


class IntExecutionResource(SimObject):
    type = "IntExecutionResource"
    cxx_class = "gem5::o3::IntExecutionResource"
    cxx_header = "cpu/o3/int_execution.hh"
    family = Param.Unsigned("1=ALU, 2=SHFT, 3=SlowInt, 4=BM, 5=Branch")
    issueInterval = Param.Cycles(1, "Service acceptance interval per connected port")
    completionDelay = Param.Cycles(0, "Result formation to operation completion")
    prfDelay = Param.Cycles(0, "Result formation to PRF writeback")


class IntegerALU(IntExecutionResource):
    type = "IntegerALU"
    cxx_class = "gem5::o3::IntegerALU"
    cxx_header = "cpu/o3/int_execution.hh"
    family = 1
    port = Param.ExecutionPort("Port connected to this ALU instance")


class ResultPathRule(SimObject):
    type = "ResultPathRule"
    cxx_class = "gem5::o3::ResultPathRule"
    cxx_header = "cpu/o3/int_execution.hh"
    producerFamily = Param.String("Any", "Producer execution family or Any")
    producerPort = Param.Int(-1, "Producer port; -1 matches any port")
    resultKind = Param.String("Any", "Any, GPR, CF, OF, Flags or Other")
    consumerFamily = Param.String("Any", "Consumer execution family or Any")
    consumerPort = Param.Int(-1, "Consumer port; -1 matches any port")
    sourceIndex = Param.Int(-1, "Consumer operand index; -1 matches any input")
    delay = Param.Int(0, "Signed CPU-cycle adjustment from nominal result time")


class ResultForwarding(SimObject):
    type = "ResultForwarding"
    cxx_class = "gem5::o3::ResultForwarding"
    cxx_header = "cpu/o3/int_execution.hh"
    paths = VectorParam.ResultPathRule([], "Explicit producer/result/consumer paths")
