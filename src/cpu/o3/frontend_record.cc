// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/frontend_record.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5::o3
{
FrontendRecord::FrontendRecord(CPU *cpu, ThreadID tid,
        const StaticInstPtr &inst, const StaticInstPtr &macro,
        const PCStateBase &at, FrontendSource supply, bool trace)
    : instruction(inst), macroop(macro), pc(at.clone()),
      predictedPC(at.clone()), sequence(cpu->getAndIncrementInstSeq()),
      thread(tid), source(supply), fetchedAt(curTick())
{
    if (cpu->markerObservation) {
        fromUser = cpu->inUserMode(tid);
        addressSpace = cpu->tcBase(tid)->readMiscRegNoEffect(
            X86ISA::misc_reg::Cr3) & ~uint64_t(0xfff);
    }
#if TRACING_ON
    if (trace)
        traceData.reset(cpu->getTracer()->getInstRecord(
            fetchedAt, cpu->tcBase(tid), inst, at, macro));
#endif
}

DynInstPtr
FrontendRecord::materialize(CPU *cpu)
{
    DynInst::Arrays arrays;
    arrays.numSrcs = instruction->numSrcRegs();
    arrays.numDests = instruction->numDestRegs();
    DynInstPtr inst = new (arrays) DynInst(
        arrays, instruction, macroop, *pc, *predictedPC, sequence, cpu);
    inst->setTid(thread);
    inst->setThreadState(cpu->thread[thread]);
    inst->setPredTaken(prediction.taken);
    inst->branchDirectionPredictionValid = prediction.directionValid;
    inst->originalBranchDirectionPrediction = prediction.originalDirection;
    inst->branchBtbHit = prediction.btbHit;
    if (instruction->isControl() && cpu->markerBranchObservation)
        set(inst->originalBranchPredPC, *predictedPC);
    inst->fetchedFromUser = fromUser;
    inst->fetchedAddressSpaceId = addressSpace;
    inst->pendingFrontendAcceptance = true;
    inst->fetchTick = fetchedAt;
    inst->fault = fault;
    if (notAnInstruction)
        inst->setNotAnInst();
    if (inst->numSrcRegs() == 0)
        inst->setCanIssue();
    inst->traceData = traceData.release();
    inst->setInstListIt(cpu->addInst(inst));
    return inst;
}
}
