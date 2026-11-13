// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/frontend_record.hh"
#include "arch/x86/pcstate.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5::o3
{
RepStringKind
repStringKind(const StaticInst *macro)
{
    if (!macro) return RepStringKind::None;
    if (macro->macroopKind() == MacroopKind::RepCopy)
        return RepStringKind::Copy;
    if (macro->macroopKind() == MacroopKind::RepStore)
        return RepStringKind::Store;
    return RepStringKind::None;
}

NearStackControl
nearStackControl(const StaticInst *macro)
{
    return macro ? macro->nearStackControl() : NearStackControl::None;
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
    if (cpu->markerObservation) {
        fromUser = cpu->inUserMode(tid);
        addressSpace = cpu->tcBase(tid)->readMiscRegNoEffect(
            X86ISA::misc_reg::Cr3) & ~uint64_t(0xfff);
    }
    if (cpu->coreFunctionalObservation) {
        coreFunctionalContext = std::make_unique<CoreFunctionalContext>();
        auto &context = *coreFunctionalContext;
        context.observeDecoder(at.as<X86ISA::PCState>());
        context.addressSpace = cpu->tcBase(tid)->readMiscRegNoEffect(
            X86ISA::misc_reg::Cr3) & ~uint64_t(0xfff);
        context.fromUser = cpu->inUserMode(tid);
        context.frontendKnown = true;
        context.origin = fastsim::fst::CoreFrontendOrigin::ObserverRecord;
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
    inst->functionalBoundary=pc->as<X86ISA::PCState>().functionalBoundary();
    inst->functionalMacroOrigin=pc->as<X86ISA::PCState>().functionalMacroOrigin();
    inst->setTid(thread);
    inst->setThreadState(cpu->thread[thread]);
    inst->setPredTaken(prediction.taken);
    inst->untrackedFrontendBranch = prediction.untrackedBranch;
    inst->predictionTransaction = prediction.transaction;
    inst->predictionVersion = prediction.version;
    if (auto history = prediction.transaction.lock();
        history && history->valid && (history->pendingTarget || history->noTargetTaken))
        history->lateInstruction = inst;
    inst->frontendDsbMiss = dsbMiss;
    inst->frontendDsbMissExposed = dsbMissExposed;
    inst->fetchedFromUser = fromUser;
    inst->fetchedAddressSpaceId = addressSpace;
    if (coreFunctionalContext) {
        inst->coreFunctionalFacts = std::make_unique<CoreFunctionalSnapshot>(
            inst->numSrcRegs(), inst->numDestRegs());
        inst->coreFunctionalFacts->context = *coreFunctionalContext;
    }
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
