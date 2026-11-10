// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/frontend_record.hh"
#include <cstring>
#include "arch/x86/insts/macroop.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5::o3
{
RepStringKind
repStringKind(const StaticInst *macro)
{
    const auto *x86_macro = dynamic_cast<const X86ISA::MacroopBase *>(macro);
    if (!x86_macro) return RepStringKind::None;
    const char *block = x86_macro->getMicrocodeBlock();
    if (std::strcmp(block, "MOVS_E_M_M") == 0) return RepStringKind::Copy;
    if (std::strcmp(block, "STOS_E_M") == 0) return RepStringKind::Store;
    return RepStringKind::None;
}

NearStackControl
nearStackControl(const StaticInst *macro)
{
    const auto *x86_macro = dynamic_cast<const X86ISA::MacroopBase *>(macro);
    if (!x86_macro) return NearStackControl::None;
    const auto env =
        const_cast<X86ISA::MacroopBase *>(x86_macro)->getEmulEnv();
    if (env.stackSize != 8 || env.dataSize != 8)
        return NearStackControl::None;
    const char *block = x86_macro->getMicrocodeBlock();
    if (std::strcmp(block, "CALL_NEAR_I") == 0)
        return NearStackControl::DirectCall;
    if (std::strcmp(block, "CALL_NEAR_R") == 0)
        return NearStackControl::RegisterCall;
    if (std::strcmp(block, "CALL_NEAR_M") == 0 ||
        std::strcmp(block, "CALL_NEAR_P") == 0)
        return NearStackControl::MemoryCall;
    if (std::strcmp(block, "RET_NEAR") == 0)
        return NearStackControl::Return;
    if (std::strcmp(block, "RET_NEAR_I") == 0)
        return NearStackControl::ReturnImmediate;
    return NearStackControl::None;
}

StageDemand
initialDemand(const StaticInst &instruction, const StaticInst *macro)
{
    StageDemand demand;
    const auto name = instruction.getName();
    const auto stack_control = nearStackControl(macro);
    if (stack_control != NearStackControl::None) {
        const bool ip_companion = name == "rdip";
        const bool immediate_companion =
            stack_control == NearStackControl::ReturnImmediate && name == "limm";
        const bool direct_call = stack_control == NearStackControl::DirectCall;
        const bool direct_control = direct_call && instruction.isCall();
        const bool stack_update = instruction.numDestRegs() == 1 &&
            instruction.destRegIdx(0) == X86ISA::int_reg::Rsp &&
            (name == "subi" || name == "addi");
        if (ip_companion || direct_control || immediate_companion || stack_update) {
            // Keep the functional PC/value and implicit RSP records. They
            // are not additional frontend or retirement units. Rename
            // evaluates ready constants; Stack Engine owns absorbed updates.
            demand.frontendUnits = demand.idqUnits = 0;
            demand.allocationUnits = demand.retirementUnits = 0;
            demand.retirementUops = 0;
        }
        if (ip_companion || direct_control || immediate_companion) {
            demand.robUnits = demand.schedulerUnits = 0;
            demand.schedulerActions = 0;
        }
        if (direct_call && instruction.isStore()) {
            // The relative CALL has two store actions and no separate
            // operand-dependent branch action.
            demand.frontendUnits = demand.idqUnits = 2;
            demand.allocationUnits = demand.robUnits = 2;
            demand.retirementUnits = demand.retirementUops = 2;
            demand.schedulerUnits = 2;
        }
    }
    const bool rep_parent = repStringKind(macro) != RepStringKind::None;
    const bool rep_helper = rep_parent && !instruction.isMemRef() &&
        !instruction.isFirstMicroop() && !instruction.isLastMicroop();
    if (name == "repstart" || name.rfind("repselect", 0) == 0 ||
        name.rfind("repprogress", 0) == 0 || rep_helper) {
        demand.frontendUnits = demand.idqUnits = demand.allocationUnits = 0;
        demand.robUnits = demand.retirementUnits = 0;
        demand.retirementUops = 0;
        demand.schedulerUnits = demand.schedulerActions = 0;
    }
    const auto divide = instruction.intDivide();
    if (divide.bytes) {
        // The 64-bit form also has a portless frontend/retirement unit.
        // Four port actions share one logical instruction: preparation and
        // three p1 actions. Output dependency times are specified separately.
        const unsigned units = divide.bytes == 8 ? 5 : 4;
        demand.frontendUnits = demand.idqUnits = units;
        demand.allocationUnits = demand.robUnits = units;
        demand.retirementUnits = units;
        demand.retirementUops = units;
        demand.schedulerUnits = 4;
        demand.schedulerActions = 1;
    }
    return demand;
}

FrontendRecord::FrontendRecord(CPU *cpu, ThreadID tid,
        const StaticInstPtr &inst, const StaticInstPtr &macro,
        const PCStateBase &at, FrontendSource supply, bool trace)
    : instruction(inst), macroop(macro), pc(at.clone()),
      predictedPC(at.clone()), sequence(cpu->getAndIncrementInstSeq()),
      thread(tid), source(supply), fetchedAt(curTick())
{
    demand = initialDemand(*inst, macro.get());
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
    inst->untrackedFrontendBranch = prediction.untrackedBranch;
    inst->frontendDsbMiss = dsbMiss;
    inst->frontendDsbMissExposed = dsbMissExposed;
    inst->fetchedFromUser = fromUser;
    inst->fetchedAddressSpaceId = addressSpace;
    inst->fetchedCodeBlocks = codeBlocks;
    inst->fetchedCodeBlockCount = codeBlockCount;
    inst->staleCode = staleCode;
    inst->pendingFrontendAcceptance = true;
    inst->allocationGroup = allocationGroup;
    inst->stageDemand = demand;
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
