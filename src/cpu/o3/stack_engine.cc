// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/stack_engine.hh"

#include <string>

#include "arch/x86/insts/macroop.hh"
#include "arch/x86/regs/int.hh"
#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/cpu.hh"
#include "debug/StackEngine.hh"

namespace gem5::o3
{

namespace
{

class StackSyncInst : public StaticInst
{
    RegId sources[1];
    RegId destinations[1];

  public:
    StackSyncInst() : StaticInst("stack_sync", IntAluOp)
    {
        setRegIdxArrays(reinterpret_cast<RegIdArrayPtr>(&StackSyncInst::sources),
                        reinterpret_cast<RegIdArrayPtr>(&StackSyncInst::destinations));
        _numSrcRegs = _numDestRegs = 1;
        _numTypedDestRegs[IntRegClass] = 1;
        setSrcRegIdx(0, X86ISA::int_reg::Rsp);
        setDestRegIdx(0, X86ISA::int_reg::Rsp);
        flags[IsInteger] = true;
        flags[IsMicroop] = true;
        flags[IsDelayedCommit] = true;
        intExecutionInfo.family = IntExecutionFamily::ALU;
        intExecutionInfo.ports = (1 << 0) | (1 << 1) | (1 << 5) |
                                 (1 << 6) | (1 << 11);
        intExecutionInfo.resultLatency = 1;
    }

    Fault execute(ExecContext *xc, trace::InstRecord *) const override
    {
        auto *inst = static_cast<DynInst *>(xc);
        const RegVal base = xc->getRegOperand(this, 0);
        const RegVal result = base + uint64_t(inst->stackSyncImmediate);
        DPRINTF(StackEngine,
                "[tid:%u] [sn:%llu] execute sync base=%#llx delta=%lld result=%#llx\n",
                inst->threadNumber, inst->seqNum, base,
                inst->stackSyncImmediate, result);
        xc->setRegOperand(this, 0, result);
        return NoFault;
    }
    void advancePC(PCStateBase &) const override {}
    IntResultRole intResultRole(RegIndex) const override
    { return IntResultRole::GPR; }
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override
    { return "stack_sync"; }
};

} // anonymous namespace

StackEngine::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(tracked, statistics::units::Count::get(), "Tracked stack-pointer updates"),
      ADD_STAT(fallback, statistics::units::Count::get(), "Unsupported RSP uses"),
      ADD_STAT(syncCreated, statistics::units::Count::get(), "Internal synchronization operations created"),
      ADD_STAT(syncExecuted, statistics::units::Count::get(), "Internal synchronizations executed, including speculation"),
      ADD_STAT(syncRetired, statistics::units::Count::get(), "Internal synchronizations retired"),
      ADD_STAT(syncSquashed, statistics::units::Count::get(), "Internal synchronizations squashed"),
      ADD_STAT(overflowSync, statistics::units::Count::get(), "Synchronizations due to offset range"),
      ADD_STAT(unsupportedSync, statistics::units::Count::get(), "Synchronizations due to unsupported RSP use"),
      ADD_STAT(branchCheckpoints, statistics::units::Count::get(), "Branch offset checkpoints"),
      ADD_STAT(branchRestores, statistics::units::Count::get(), "Restores from branch checkpoints"),
      ADD_STAT(otherRestores, statistics::units::Count::get(), "Other ordered offset recoveries")
{}

StackEngine::StackEngine(const StackEngineParams &p)
    : SimObject(p), active(p.enabled), minimum(p.minOffsetBytes),
      maximum(p.maxOffsetBytes), immediateMinimum(p.minImmediate),
      immediateMaximum(p.maxImmediate), syncZero(p.syncOnZero), stats(this)
{
    fatal_if(minimum >= 0 || maximum <= 0 || minimum > maximum ||
             immediateMinimum > immediateMaximum,
             "Invalid StackEngine offset or immediate interval");
}

void
StackEngine::configure(unsigned num_threads)
{
    threads.resize(num_threads);
}

StackEngine::Decision
StackEngine::classify(const DynInstPtr &inst) const
{
    Decision d;
    if (inst->stackSync || inst->isSquashed())
        return d;

    if (!active)
        return d;

    auto *isa = inst->tcBase()->getIsaPtr();
    const auto rsp = X86ISA::int_reg::Rsp.flatten(*isa);
    const auto is_rsp = [&](const RegId &reg) { return reg.flatten(*isa) == rsp; };
    bool writes_rsp = false;
    for (unsigned i = 0; i < inst->numSrcRegs(); ++i)
        if (is_rsp(inst->srcRegIdx(i))) {
            fatal_if(i >= 64, "Too many RSP source operands");
            d.sourceMask |= uint64_t(1) << i;
        }
    for (unsigned i = 0; i < inst->numDestRegs(); ++i)
        writes_rsp |= is_rsp(inst->destRegIdx(i));

    const auto *macro = dynamic_cast<X86ISA::MacroopBase *>(inst->macroop.get());
    const bool first = !inst->isMicroop() || inst->isFirstMicroop();
    const int64_t current = offset(inst->threadNumber);
    if (macro) {
        const auto env = const_cast<X86ISA::MacroopBase *>(macro)->getEmulEnv();
        const auto emi = const_cast<X86ISA::MacroopBase *>(macro)->getExtMachInst();
        const std::string block(macro->getMicrocodeBlock());
        const bool stack_size = env.stackSize == 8 &&
                                (env.dataSize == 8 || env.dataSize == 2);
        const bool push = stack_size && (block == "PUSH_R" || block == "PUSH_I" ||
            block == "CALL_NEAR_I" || block == "CALL_NEAR_R" ||
            block == "CALL_NEAR_M" || block == "CALL_NEAR_P");
        const bool ret_imm = block == "RET_NEAR_I";
        const bool pop = stack_size && ((block == "POP_R" &&
            env.reg != X86ISA::int_reg::_RspIdx) || block == "RET_NEAR" || ret_imm);
        if (push || pop) {
            const int64_t immediate = ret_imm ? (emi.immediate & 0xffff) : 0;
            const int64_t total = push ? -env.dataSize : env.dataSize + immediate;
            const int64_t step = ret_imm && writes_rsp &&
                inst->staticInst->getName() == "add" ? immediate :
                (push ? -env.dataSize : env.dataSize);
            if (!inRange(total) || !inRange(step)) {
                d.fallback = first || (!first && current != 0 &&
                                       (d.sourceMask || writes_rsp));
                d.sync = d.fallback && (current != 0 || (first && syncZero));
                d.reason = d.sync ? SyncReason::Unsupported : SyncReason::None;
                return d;
            }
            // A replay can re-enter an ungrouped macro after its first
            // functional member, so check its actual update as well.
            if ((first && !inRange(current + total)) ||
                (writes_rsp && !inRange(current + step))) {
                d.sync = true;
                d.reason = SyncReason::Overflow;
            }
            if (writes_rsp) {
                d.trackedWrite = true;
                d.absorbedUpdate = true;
                d.increment = step;
            }
            return d;
        }

        const int64_t imm = int64_t(emi.immediate);
        if ((block == "ADD_R_I" || block == "SUB_R_I") &&
            env.reg == X86ISA::int_reg::_RspIdx && env.dataSize == 8 &&
            tracksImmediate(imm)) {
            const int64_t step = block == "ADD_R_I" ? imm : -imm;
            if (!inRange(step)) {
                d.fallback = first;
                d.sync = first && (current != 0 || syncZero);
                d.reason = d.sync ? SyncReason::Unsupported : SyncReason::None;
                return d;
            }
            if (first && !inRange(current + step)) {
                d.sync = true;
                d.reason = SyncReason::Overflow;
            }
            if (writes_rsp) {
                d.trackedWrite = true;
                d.increment = step;
            }
            return d;
        }

        if (block == "LEA_R_M" && env.reg == X86ISA::int_reg::_RspIdx &&
            env.dataSize == 8 && env.addressSize == 8 && is_rsp(env.base) &&
            env.index == X86ISA::int_reg::T0 && emi.displacement == 0) {
            d.trackedWrite = writes_rsp;
            return d;
        }

        // These operations can consume the logical RSP without making a new
        // physical base. RSP destinations instead take the conservative path.
        if (!writes_rsp && (block == "MOV_R_R" || block == "LEA_R_M" ||
                            block == "MOV_R_M"))
            return d;

        // Synchronize once at the beginning of an unsupported macro, before
        // any member of a shared allocation group can be admitted.
        bool macro_uses_rsp = false;
        if (first) {
            for (unsigned u = 0; u < macro->functionalMicroops(); ++u) {
                const auto op = inst->macroop->fetchMicroop(u);
                for (unsigned i = 0; i < op->numSrcRegs(); ++i)
                    macro_uses_rsp |= is_rsp(op->srcRegIdx(i));
                for (unsigned i = 0; i < op->numDestRegs(); ++i)
                    macro_uses_rsp |= is_rsp(op->destRegIdx(i));
            }
        }
        d.fallback = (first && macro_uses_rsp) ||
            (!first && current != 0 && (d.sourceMask || writes_rsp));
    } else {
        d.fallback = d.sourceMask || writes_rsp;
    }
    if (d.fallback && (current != 0 || syncZero)) {
        d.sync = true;
        d.reason = SyncReason::Unsupported;
    }
    return d;
}

DynInstPtr
StackEngine::makeSync(const DynInstPtr &before, SyncReason reason)
{
    static StaticInstPtr operation = new StackSyncInst();
    DynInst::Arrays arrays{};
    arrays.numSrcs = arrays.numDests = 1;
    DynInstPtr inst = new (arrays) DynInst(arrays, operation, nullptr,
        before->pcState(), before->pcState(), before->seqNum - 1, before->cpu);
    inst->setTid(before->threadNumber);
    inst->setThreadState(before->thread);
    inst->fetchedFromUser = before->fetchedFromUser;
    inst->fetchedAddressSpaceId = before->fetchedAddressSpaceId;
    inst->fetchTick = before->fetchTick;
    inst->stageDemand.frontendUnits = 0;
    inst->stageDemand.idqUnits = 0;
    inst->stackSync = true;
    inst->stackSyncImmediate = offset(before->threadNumber);
    DPRINTF(StackEngine,
            "[tid:%u] [sn:%llu] create sync before [sn:%llu] PC:%s delta=%lld reason=%u\n",
            inst->threadNumber, inst->seqNum, before->seqNum,
            before->pcState(), inst->stackSyncImmediate, unsigned(reason));
    inst->setNotAnInst();
    inst->setInstListIt(before->cpu->insertInstBefore(inst, before));
    noteSyncCreated(reason);
    return inst;
}

void
StackEngine::apply(ThreadID tid, InstSeqNum sequence, int64_t value)
{
    auto &s = threads.at(tid);
    DPRINTF(StackEngine, "[tid:%u] [sn:%llu] speculative delta %lld -> %lld\n",
            tid, sequence, s.speculative, value);
    s.changes.push_back({sequence, s.speculative, value});
    s.speculative = value;
}

void
StackEngine::checkpoint(ThreadID tid, InstSeqNum sequence)
{
    auto &s = threads.at(tid);
    s.checkpoints[sequence] = s.speculative;
    DPRINTF(StackEngine, "[tid:%u] [sn:%llu] checkpoint delta=%lld\n",
            tid, sequence, s.speculative);
    ++stats.branchCheckpoints;
}

void
StackEngine::squash(ThreadID tid, InstSeqNum through)
{
    auto &s = threads.at(tid);
    const int64_t before = s.speculative;
    const auto checkpoint = s.checkpoints.find(through);
    const bool branch = checkpoint != s.checkpoints.end();
    const int64_t branch_value = branch ? checkpoint->second : 0;
    while (!s.changes.empty() && s.changes.back().sequence > through) {
        s.speculative = s.changes.back().before;
        s.changes.pop_back();
    }
    if (branch) {
        s.speculative = branch_value;
        ++stats.branchRestores;
    } else {
        ++stats.otherRestores;
    }
    s.checkpoints.erase(s.checkpoints.upper_bound(through), s.checkpoints.end());
    DPRINTF(StackEngine,
            "[tid:%u] squash through [sn:%llu] delta %lld -> %lld checkpoint=%u\n",
            tid, through, before, s.speculative, branch);
}

void
StackEngine::retire(ThreadID tid, InstSeqNum through, bool changed, int64_t value)
{
    auto &s = threads.at(tid);
    if (changed) {
        DPRINTF(StackEngine, "[tid:%u] [sn:%llu] committed delta %lld -> %lld\n",
                tid, through, s.committed, value);
        s.committed = value;
    }
    while (!s.changes.empty() && s.changes.front().sequence <= through)
        s.changes.pop_front();
    s.checkpoints.erase(s.checkpoints.begin(), s.checkpoints.upper_bound(through));
}

void
StackEngine::reset(ThreadID tid)
{
    threads.at(tid) = ThreadState{};
}

void
StackEngine::noteSyncCreated(SyncReason reason)
{
    ++stats.syncCreated;
    if (reason == SyncReason::Overflow) ++stats.overflowSync;
    if (reason == SyncReason::Unsupported) ++stats.unsupportedSync;
}

} // namespace gem5::o3
