# Copyright 2021 Google, Inc.
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

from m5.objects.BaseAtomicSimpleCPU import BaseAtomicSimpleCPU
from m5.objects.BaseMinorCPU import BaseMinorCPU
from m5.objects.BaseNonCachingSimpleCPU import BaseNonCachingSimpleCPU
from m5.objects.BaseO3CPU import BaseO3CPU
from m5.objects.BaseTimingSimpleCPU import BaseTimingSimpleCPU
from m5.objects.BranchPredictor import (
    BranchPredictor, MultiLevelBTB, GoldenCoveBP,
    GoldenCoveIndirectPredictor, ReturnAddrStack,
)
from m5.objects.FuncUnit import *
from m5.objects.FUPool import *
from m5.objects.IQUnit import IQUnit
from m5.objects.ZeroIdiom import ZeroIdiom
from m5.objects.StackEngine import StackEngine
from m5.objects.Frontend import FrontendCoordinator
from m5.objects.IntExecution import (
    ExecutionPorts,
    IntegerALU,
    IntegerDivider,
    IntExecutionResource,
    ResultForwarding,
)
from m5.objects.X86Decoder import X86Decoder
from m5.objects.X86ISA import X86ISA
from m5.objects.X86LocalApic import X86LocalApic
from m5.objects.X86MMU import X86MMU
from m5.objects.X86TLB import X86TLB, X86PagingStructureCache
from m5.proxy import Self


class X86CPU:
    ArchDecoder = X86Decoder
    ArchMMU = X86MMU
    ArchInterrupts = X86LocalApic
    ArchISA = X86ISA


class X86AtomicSimpleCPU(BaseAtomicSimpleCPU, X86CPU):
    mmu = X86MMU()


class X86NonCachingSimpleCPU(BaseNonCachingSimpleCPU, X86CPU):
    mmu = X86MMU()


class X86TimingSimpleCPU(BaseTimingSimpleCPU, X86CPU):
    mmu = X86MMU()


class X86IntMult(IntMultDiv):
    opList = [OpDesc(opClass="IntMult", opLat=3)]
    count = 1


class X86IntDiv(IntMultDiv):
    # DIV and IDIV instructions in x86 are implemented using a loop which
    # issues division microops.  The latency of these microops should really be
    # one (or a small number) cycle each since each of these computes one bit
    # of the quotient.
    opList = [
        OpDesc(opClass="IntDiv", opLat=1, pipelined=False),
    ]

    count = 2


class X86StoreAddressPort(FUDesc):
    opList = [OpDesc(opClass="X86StoreAddress")]
    count = 2


class X86StoreDataPort(FUDesc):
    opList = [OpDesc(opClass="X86StoreData")]
    count = 2


class DefaultX86FUPool(FUPool):
    FUList = [
        IntALU(),
        X86IntMult(),
        X86IntDiv(),
        FP_ALU(),
        FP_MultDiv(),
        ReadPort(),
        SIMD_Unit(),
        PredALU(),
        WritePort(),
        X86StoreAddressPort(),
        X86StoreDataPort(),
        RdWrPort(),
        System_Unit(),
    ]


def spr_fu_pool():
    """One shared twelve-port pool, owned and cloned with each SPR core."""
    specs = {}
    for unit in DefaultX86FUPool().FUList:
        for op in unit.opList:
            specs.setdefault(str(op.opClass), op)
    reads = {str(op.opClass) for op in ReadPort().opList}
    writes = {str(op.opClass) for op in WritePort().opList}
    ports = [[] for _ in range(12)]
    latencies = {
        "FloatAdd": 3, "FloatMult": 4, "FloatDiv": 14, "FloatMisc": 1,
        "FloatMultAcc": 4, "SimdFloatMultAcc": 4,
        "SimdAlu": 1, "SimdAdd": 1, "SimdMisc": 1, "SimdCmp": 1,
        "SimdShift": 1, "SimdMult": 3, "SimdMultAcc": 5, "SimdFloatAdd": 3,
        "SimdFloatMult": 4, "SimdFloatDiv": 11,
        "X86FloatSqrt32": 12, "X86FloatSqrt64": 18,
    }
    for name, op in specs.items():
        if name == "X86StoreAddress":
            indices = (7, 8)
        elif name == "X86StoreData":
            indices = (4, 9)
        elif name in reads:
            indices = (2, 3, 10)
        elif name in writes:
            indices = (7, 8)
        elif name == "IntAlu":
            indices = (0, 1, 5, 6, 11)
        elif name in {"IntMult", "FloatAdd", "SimdFloatAdd", "SimdMisc"}:
            indices = (1, 5)
        elif name == "IntDiv":
            indices = (1,)
        elif name in {"FloatMult", "SimdFloatMult", "SimdMult", "FloatMultAcc",
                      "SimdFloatMultAcc", "SimdMultAcc", "SimdCmp", "SimdShift"}:
            indices = (0, 1)
        elif name in {"SimdPack", "SimdInsert"}:
            indices = (5,)
        elif (name in {"SimdExtract", "System", "SimdPredAlu"}
              or "Div" in name or "Sqrt" in name):
            indices = (0,)
        else:
            indices = (0, 1, 5)
        for index in indices:
            desc = OpDesc(
                opClass=op.opClass, opLat=latencies.get(name, op.opLat),
                pipelined=True if name in {"FloatDiv", "SimdFloatDiv",
                                          "X86FloatSqrt32", "X86FloatSqrt64"}
                else op.pipelined,
            )
            if name in {"FloatDiv", "SimdFloatDiv"}:
                desc.issueLat = 4 if name == "FloatDiv" else 3
            if name in {"X86FloatSqrt32", "X86FloatSqrt64"}:
                desc.issueLat = 6 if name == "X86FloatSqrt64" else 3
            ports[index].append(desc)
    return FUPool(
        FUList=[FUDesc(opList=ops, count=1) for ops in ports],
        storeDataPorts=[4, 9], branchPorts=[0, 6],
    )


_spr_mem_classes = sorted(
    {str(op.opClass) for op in ReadPort().opList}
    | {str(op.opClass) for op in WritePort().opList}
    | {"X86StoreAddress"}
)


def x86_o3_mmu():
    """Default O3 instruction translation and shared second-level proxy."""
    mmu = X86MMU()
    mmu.pagingStructureCache = X86PagingStructureCache()
    mmu.itb.walker.paging_structure_cache = mmu.pagingStructureCache
    mmu.dtb.walker.paging_structure_cache = mmu.pagingStructureCache
    mmu.stlb = X86TLB(size=2048, entry_type="unified", spr_shared_domains=True)
    mmu.itb.size = 256
    mmu.itb.itlb_4k_ways = 8
    mmu.itb.spr_split_l1 = True
    mmu.itb.next_level = mmu.stlb
    mmu.dtb.size = 96
    mmu.dtb.dtlb_4k_ways = 6
    mmu.dtb.spr_split_l1 = True
    mmu.dtb.next_level_hit_latency = 6
    mmu.dtb.walker.max_active_walks = 4
    mmu.dtb.next_level = mmu.stlb
    return mmu


class X86O3CPU(BaseO3CPU, X86CPU):
    mmu = x86_o3_mmu()
    needsTSO = True
    fetchWidth = 8
    fetchBufferSize = 64
    fetchQueueSize = 144
    decodeWidth = 6
    renameWidth = 6
    dispatchWidth = 6
    issueWidth = 12
    commitWidth = 8
    numROBEntries = 512
    LQEntries = 240
    SQEntries = 112
    cacheLoadPorts = 3
    cacheStorePorts = 2
    loadCompletionWidth = 3
    # Storage covers the bounded O3 record window and functional results;
    # hardware allocation credits remain independent of that representation.
    numPhysIntRegEntries = 288
    numPhysIntRegs = numPhysIntRegEntries + numROBEntries * 16 * 4
    numPhysFloatRegEntries = 320
    numPhysFloatRegs = numPhysFloatRegEntries * 8 + numROBEntries * 16 * 2
    numPhysFloatWideEntries = 220
    sprL1LoadBankConflicts = False
    sprMemoryDependence = True
    sprFastStoreForwarding = True
    sprL1WidthService = True
    sprStoreBuffer = True
    sprEarlyStorePrepare = True
    # Native 8457C fence controls distinguish architectural MFENCE service
    # from SFENCE and from the embedded fences of locked microcode.
    sprFullFenceServiceCycles = 34
    branchPred = BranchPredictor(
        conditionalBranchPred=GoldenCoveBP(), btb=MultiLevelBTB(),
        indirectBranchPred=GoldenCoveIndirectPredictor(),
        ras=ReturnAddrStack(numEntries=16),
    )
    decoupledFrontEnd = True
    # One target range covers the resident 64-byte refill; MITE still receives
    # at most 32 bytes per cycle from InstructionBytes.
    fetchTargetWidth = 64

    # Development measurements always use the implemented integer structure.
    # SimObject cloning preserves shared port references within each core.
    executionPorts = ExecutionPorts()
    intALUs = [
        IntegerALU(port=port)
        for port in executionPorts.ports
        if int(port.number) in (0, 1, 5, 6, 11)
    ]
    resultForwarding = ResultForwarding()
    zeroIdiom = ZeroIdiom()
    stackEngine = StackEngine(enabled=True)
    frontend = FrontendCoordinator(enableLsdReplay=False)
    intSHFT = IntExecutionResource(family=2)
    intSlow = IntExecutionResource(family=3)
    intBM = IntExecutionResource(family=4)
    intBranch = IntExecutionResource(family=5)
    intDivider = IntegerDivider()

    # For x86, each CC reg is used to hold only a subset of the
    # flags, so we need 4-5 times the number of CC regs as
    # physical integer regs to be sure we don't run out.  In
    # typical real machines, CC regs are not explicitly renamed
    # (it's a side effect of int reg renaming), so they should
    # never be the bottleneck here.
    numPhysCCRegs = (numPhysIntRegEntries + numROBEntries * 16) * 5

    # Shared scheduler capacity; no hard EU/MEM/STD partition is assumed.
    sprFUPool = spr_fu_pool()
    sharedIQ = IQUnit(numEntries=248, fuPool=sprFUPool)
    instQueues = [sharedIQ]


class X86MinorCPU(BaseMinorCPU, X86CPU):
    mmu = X86MMU()
