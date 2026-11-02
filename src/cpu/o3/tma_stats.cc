// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/cpu.hh"

#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq.hh"
#include "mem/taotrace_response.hh"

namespace gem5::o3
{

CPU::MoveEliminationStats::MoveEliminationStats(CPU *cpu)
    : statistics::Group(cpu, "moveElimination"),
      ADD_STAT(accepted, statistics::units::Count::get(), "Full-width GPR and XMM moves renamed as physical-register aliases"),
      ADD_STAT(retired, statistics::units::Count::get(), "Retired move aliases; allocation and ROB slots retained")
{}

CPU::MemoryInstRetiredStats::MemoryInstRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "MEM_INST_RETIRED"),
      ADD_STAT(ALL_LOADS, statistics::units::Count::get(), "Retired architectural instructions containing a memory load, including software prefetch; port IO excluded"),
      ADD_STAT(ALL_STORES, statistics::units::Count::get(), "Retired architectural instructions containing a memory store; port IO and prefetch excluded")
{}

CPU::MemoryLoadRetiredStats::MemoryLoadRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "MEM_LOAD_RETIRED"),
      ADD_STAT(L1_HIT, statistics::units::Count::get(), "Retired load instructions with an observed native L1 data hit"),
      ADD_STAT(L1_MISS, statistics::units::Count::get(), "Retired load instructions with an observed native L1 data miss"),
      ADD_STAT(L2_HIT, statistics::units::Count::get(), "Retired load instructions with an observed native L2 data hit"),
      ADD_STAT(L2_MISS, statistics::units::Count::get(), "Retired load instructions with an observed native L2 data miss"),
      ADD_STAT(L3_HIT, statistics::units::Count::get(), "Retired load instructions with an observed native LLC data hit"),
      ADD_STAT(L3_MISS, statistics::units::Count::get(), "Retired load instructions with an observed native LLC data miss"),
      ADD_STAT(FB_HIT, statistics::units::Count::get(), "Retired load instructions merged into an outstanding native L1 miss"),
      ADD_STAT(storeForward, statistics::units::Count::get(), "Retired load instructions supplied by an observed LSQ store-forward terminal; distinct from native cache hits"),
      ADD_STAT(unknownSource, statistics::units::Count::get(), "Retired load instructions without a complete native cache-source classification")
{}

void CPU::beginMemoryObservation()
{
    memoryObservationActive = true;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        const auto context = uint32_t(tcBase(tid)->contextId());
        TaoTraceNativeAccessRegistry::enableRetirementContext(context);
    }
}

void CPU::endMemoryObservation()
{
    memoryObservationActive = false;
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        TaoTraceNativeAccessRegistry::disableRetirementContext(uint32_t(tcBase(tid)->contextId()));
}

void CPU::releaseMemoryRetirement(const DynInstPtr &inst, bool squashed)
{
    if (!memoryObservationActive) return;
    const auto context = uint32_t(tcBase(inst->threadNumber)->contextId());
    if (squashed)
        TaoTraceNativeAccessRegistry::noteSquashed(context, inst->seqNum);
    else
        TaoTraceNativeAccessRegistry::releaseRetired(context, inst->seqNum,
                                                    inst->isStore());
}

void CPU::recordMemoryRetirement(const DynInstPtr &inst)
{
    if (!memoryObservationActive || inst->notAnInst()) return;
    const ThreadID tid = inst->threadNumber;
    auto &pending = pendingMemoryRetirement[tid];
    const bool port_io = inst->staticInst->isArchitecturalPortIO() ||
        (inst->macroop && inst->macroop->isArchitecturalPortIO());
    const bool prefetch = inst->isDataPrefetch();
    const bool locked = inst->staticInst->isArchitecturalLockedMemory() ||
        (inst->macroop && inst->macroop->isArchitecturalLockedMemory());
    const bool load = !port_io && (inst->isLoad() || inst->isAtomic() || prefetch);
    const bool store = !port_io && !inst->isDataPrefetch() && (inst->isStore() || inst->isAtomic());
    pending.load |= load;
    pending.store |= store;
    const auto context = uint32_t(tcBase(tid)->contextId());
    if (load) {
        const auto facts = TaoTraceNativeAccessRegistry::snapshot(context, inst->seqNum);
        const auto &h = facts.hierarchy;
        uint8_t source = 0;
        if (facts.terminalReasonMask & taoTraceNativeReasonMask(TaoTraceNativeTerminalReason::StoreForward))
            source |= 128;
        // Intel's L1_HIT definition includes software prefetch and locked
        // instructions regardless of the actual data source.
        if (h.hits[0] || h.permissionUpgrades[0] || prefetch || locked) source |= 1;
        // A fill-buffer merge is a distinct source from a new L1 miss.
        // Split loads can observe both and therefore set both bits.
        if (h.tagMisses[0]) source |= 2;
        if (h.hits[1] || h.permissionUpgrades[1]) source |= 4;
        if (h.tagMisses[1]) source |= 8;
        if (h.hits[2] || h.permissionUpgrades[2]) source |= 16;
        if (h.tagMisses[2] || h.rubyMemoryFetches || h.memoryReadTransactions) source |= 32;
        if (h.mergedMisses[0]) source |= 64;
        if (inst->savedRequest) {
            const auto req = inst->savedRequest->mainReq();
            const auto extension = req ? req->getExtension<TaoTraceRubyResponseExtension>() : nullptr;
            if (extension) {
                if (extension->terminalReasonMask & taoTraceNativeReasonMask(TaoTraceNativeTerminalReason::StoreForward))
                    source |= 128;
                if (!h.accesses[0] && !inst->isAtomic() && !inst->strictlyOrdered()) {
                    if (extension->externalHits) source |= 1;
                    if (extension->externalMisses > extension->coalesced) source |= 2;
                }
                if (extension->coalesced && extension->externalMisses) source |= 64;
            }
        }
        pending.sources |= source;
        pending.unknown |= !source || ((source & 2) && !(source & (4 | 16 | 32 | 64)));
    }
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        if (pending.load) {
            ++memoryInstRetiredStats.ALL_LOADS;
            if (pending.sources & 1) ++memoryLoadRetiredStats.L1_HIT;
            if (pending.sources & 2) ++memoryLoadRetiredStats.L1_MISS;
            if (pending.sources & 4) ++memoryLoadRetiredStats.L2_HIT;
            if (pending.sources & 8) ++memoryLoadRetiredStats.L2_MISS;
            if (pending.sources & 16) ++memoryLoadRetiredStats.L3_HIT;
            if (pending.sources & 32) ++memoryLoadRetiredStats.L3_MISS;
            if (pending.sources & 64) ++memoryLoadRetiredStats.FB_HIT;
            if (pending.sources & 128) ++memoryLoadRetiredStats.storeForward;
            if (pending.unknown) ++memoryLoadRetiredStats.unknownSource;
        }
        if (pending.store) ++memoryInstRetiredStats.ALL_STORES;
        pending = {};
    }
}

CPU::InstRetiredStats::InstRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "INST_RETIRED"),
      ADD_STAT(ANY, statistics::units::Count::get(),
               "Retired architectural instructions, including NOPs, across this O3 core")
{}

CPU::BranchInstStats::BranchInstStats(CPU *cpu)
    : statistics::Group(cpu, "BR_INST_RETIRED"),
      ADD_STAT(ALL_BRANCHES, statistics::units::Count::get(), "Retired architectural branches"),
      ADD_STAT(COND, statistics::units::Count::get(), "Retired conditional branches"),
      ADD_STAT(INDIRECT, statistics::units::Count::get(), "Retired near indirect branches excluding TSX"),
      ADD_STAT(NEAR_RETURN, statistics::units::Count::get(), "Retired near returns")
{}

CPU::UopsRetiredStats::UopsRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_RETIRED"),
      ADD_STAT(SLOTS, statistics::units::Count::get(),
               "Retired model allocation units; shared groups counted at their final record")
{}

CPU::BranchMispStats::BranchMispStats(CPU *cpu)
    : statistics::Group(cpu, "BR_MISP_RETIRED"),
      ADD_STAT(ALL_BRANCHES, statistics::units::Count::get(), "Retired branches with execution-stage address mismatch"),
      ADD_STAT(COND, statistics::units::Count::get(), "Retired conditional branches with execution-stage address mismatch"),
      ADD_STAT(INDIRECT, statistics::units::Count::get(), "Retired near indirect mismatches excluding TSX"),
      ADD_STAT(RET, statistics::units::Count::get(), "Retired return mismatches detected at execution")
{}

void
CPU::recordBranchRetirement(const DynInstPtr &inst)
{
    if (inst->notAnInst() || (inst->isMicroop() && !inst->isLastMicroop()) ||
        !inst->staticInst->isArchitecturalBranch()) return;
    const auto &op = inst->staticInst;
    ++branchInstStats.ALL_BRANCHES;
    if (op->isArchitecturalConditionalBranch()) ++branchInstStats.COND;
    if (op->isNearIndirectBranch()) ++branchInstStats.INDIRECT;
    if (op->isRasReturn()) ++branchInstStats.NEAR_RETURN;
    if (!inst->executionBranchMispredicted) return;
    ++branchMispStats.ALL_BRANCHES;
    if (op->isArchitecturalConditionalBranch()) ++branchMispStats.COND;
    if (op->isNearIndirectBranch()) ++branchMispStats.INDIRECT;
    if (op->isReturn()) ++branchMispStats.RET;
}

CPU::UnhaltedClockStats::UnhaltedClockStats(CPU *cpu)
    : statistics::Group(cpu, "CPU_CLK_UNHALTED"), owner(cpu),
      ADD_STAT(THREAD, statistics::units::Cycle::get(),
               "Cycles while an O3 thread is active, including unscheduled cycles"),
      ADD_STAT(REF_TSC, statistics::units::Cycle::get(),
               "Active O3 thread cycles at the model TSC frequency; TSC follows the CPU clock")
{}

void
CPU::UnhaltedClockStats::account()
{
    const uint64_t now = uint64_t(owner->curCycle());
    for (ThreadID tid = 0; tid < owner->numThreads; ++tid) {
        if (running[tid]) {
            const uint64_t elapsed = now - lastCycle[tid];
            THREAD += elapsed;
            REF_TSC += elapsed;
        }
        lastCycle[tid] = now;
    }
}

void
CPU::UnhaltedClockStats::setActive(ThreadID tid, bool value)
{
    account();
    running.at(tid) = value;
}

void
CPU::UnhaltedClockStats::preDumpStats()
{
    account();
    statistics::Group::preDumpStats();
}

void
CPU::UnhaltedClockStats::resetStats()
{
    account();
    statistics::Group::resetStats();
}

CPU::TmaStats::TmaStats(CPU *cpu, unsigned allocation_width)
    : statistics::Group(cpu, "tma"), owner(cpu), width(allocation_width),
      ADD_STAT(slots, statistics::units::Count::get(), "Model allocation slots"),
      ADD_STAT(retiring, statistics::units::Count::get(),
               "Allocated slots not known to be discarded by speculation"),
      ADD_STAT(badSpeculation, statistics::units::Count::get(),
               "Discarded speculative slots and recovery bubbles"),
      ADD_STAT(frontendBound, statistics::units::Count::get(),
               "Unfilled slots when the backend can accept work"),
      ADD_STAT(backendBound, statistics::units::Count::get(),
               "Unfilled slots blocked by backend resources"),
      ADD_STAT(pending, statistics::units::Count::get(),
               "Window allocations still in flight at the snapshot"),
      ADD_STAT(retired, statistics::units::Count::get(),
               "Window allocations that retired before the snapshot"),
      ADD_STAT(otherSquashes, statistics::units::Count::get(),
               "Window allocations discarded for a non-speculation reason"),
      ADD_STAT(allocatedOps, statistics::units::Count::get(),
               "Functional operations accepted in the measured window"),
      ADD_STAT(fusedPairs, statistics::units::Count::get(),
               "Flag producer/Jcc groups formed by the frontend and allocated")
{}

void
CPU::TmaStats::emptySlots(uint64_t count, EmptyReason reason)
{
    slots += count;
    switch (reason) {
      case EmptyReason::Frontend: frontendBound += count; break;
      case EmptyReason::Backend: backendBound += count; break;
      case EmptyReason::Recovery: badSpeculation += count; break;
    }
}

void
CPU::TmaStats::accountUntil(uint64_t now)
{
    if (tracking && active && now > lastCycle)
        emptySlots((now - lastCycle) * width, previous);
    lastCycle = now;
}

void
CPU::TmaStats::setActive(bool value)
{
    if (active == value) return;
    accountUntil(uint64_t(owner->curCycle()));
    active = value;
}

void
CPU::TmaStats::begin()
{
    fatal_if(owner->numThreads != 1 || width != 6,
             "TMA slot model requires one thread and renameWidth=6");
    statistics::Group::resetStats();
    outstanding.clear();
    accepted = 0;
    recovery = sawRecoverySquash = false;
    previous = EmptyReason::Frontend;
    lastCycle = uint64_t(owner->curCycle());
    tracking = true;
}

void
CPU::TmaStats::end()
{
    accountUntil(uint64_t(owner->curCycle()));
    pending = outstanding.size();
    tracking = false;
}

void
CPU::TmaStats::allocated(const DynInstPtr &inst)
{
    if (!tracking || !active) return;
    ++allocatedOps;
    if (!inst->allocationSlots()) return;
    if (inst->allocationGroup && inst->allocationGroup->branch) ++fusedPairs;
    ++accepted;
    ++slots;
    if (recovery && !sawRecoverySquash) {
        ++badSpeculation;
    } else {
        ++retiring;
        outstanding.emplace(inst->seqNum, inst->allocationGroup);
    }
}

void
CPU::TmaStats::onRetire(const DynInstPtr &inst)
{
    if (!tracking) return;
    const auto group = inst->allocationGroup;
    if ((!group || group->end == inst->seqNum) &&
        outstanding.erase(group ? group->leader : inst->seqNum)) ++retired;
}

void
CPU::TmaStats::onOtherSquash(const DynInstPtr &inst)
{
    if (tracking && outstanding.erase(inst->allocationGroup ?
        inst->allocationGroup->leader : inst->seqNum)) ++otherSquashes;
}

void
CPU::TmaStats::squashAfter(InstSeqNum sequence, bool speculation)
{
    if (!tracking) return;
    auto it = outstanding.upper_bound(sequence);
    if (it != outstanding.begin()) {
        auto preceding = std::prev(it);
        if (preceding->second && preceding->second->end > sequence) it = preceding;
    }
    while (it != outstanding.end()) {
        if ((it->second ? it->second->end : it->first) <= sequence) {
            ++it;
            continue;
        }
        if (speculation) {
            retiring -= 1;
            ++badSpeculation;
        } else {
            ++otherSquashes;
        }
        it = outstanding.erase(it);
    }
    if (speculation) {
        recovery = true;
        sawRecoverySquash = false;
    }
}

void
CPU::TmaStats::renameCycle(bool blocked, bool squashing)
{
    if (!tracking || !active) return;
    const uint64_t now = uint64_t(owner->curCycle());
    if (now > lastCycle + 1) accountUntil(now - 1);
    lastCycle = now;
    panic_if(accepted > width, "TMA accepted more than the allocation width");
    if (recovery && squashing) sawRecoverySquash = true;
    EmptyReason reason = EmptyReason::Frontend;
    if (recovery && squashing)
        reason = EmptyReason::Recovery;
    else if (blocked)
        reason = EmptyReason::Backend;
    else if (recovery && accepted == 0)
        reason = EmptyReason::Recovery;
    emptySlots(width - accepted, reason);
    previous = reason;
    if (recovery && sawRecoverySquash && accepted) {
        recovery = false;
        sawRecoverySquash = false;
    }
    accepted = 0;
}

void
CPU::TmaStats::preDumpStats()
{
    accountUntil(uint64_t(owner->curCycle()));
    if (tracking) pending = outstanding.size();
    statistics::Group::preDumpStats();
}

void
CPU::TmaStats::resetStats()
{
    accountUntil(uint64_t(owner->curCycle()));
    outstanding.clear();
    accepted = 0;
    recovery = sawRecoverySquash = false;
    previous = EmptyReason::Frontend;
    statistics::Group::resetStats();
}

} // namespace gem5::o3
