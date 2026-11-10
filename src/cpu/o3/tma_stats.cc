// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/cpu.hh"

#include "arch/generic/isa.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/probe/perf_retirement.hh"
#include "mem/taotrace_response.hh"
#include "mem/functional_translation.hh"

namespace gem5::o3
{

CPU::MachineClearStats::MachineClearStats(CPU *cpu)
    : statistics::Group(cpu, "MACHINE_CLEARS"),
      ADD_STAT(COUNT, statistics::units::Count::get(),
               "Accepted non-branch reexecution clears, not raw violations"),
      ADD_STAT(MEMORY_ORDERING, statistics::units::Count::get(),
               "Accepted ordinary store/load ordering clears"),
      ADD_STAT(fsfp, statistics::units::Count::get(),
               "Accepted FSFP validation-failure clears"),
      ADD_STAT(snoop, statistics::units::Count::get(),
               "Accepted snoop/load-order reexecution clears"),
      ADD_STAT(code, statistics::units::Count::get(),
               "Accepted stale-code reexecution clears"),
      ADD_STAT(other, statistics::units::Count::get(),
               "Accepted remaining reexecution clears"),
      ADD_STAT(orderingCandidates, statistics::units::Count::get(),
               "Ordinary ordering, FSFP and snoop candidates; hardware membership unverified")
{}

CPU::RecoveryStats::RecoveryStats(CPU *cpu)
    : statistics::Group(cpu, "INT_MISC"), owner(cpu),
      ADD_STAT(RECOVERY_CYCLES, statistics::units::Cycle::get(),
               "Allocator-squashing cycles within accepted clear episodes"),
      ADD_STAT(CLEAR_RESTEER_CYCLES, statistics::units::Cycle::get(),
               "Allocator recovery end to first new-path allocation unit"),
      ADD_STAT(CLEARS_COUNT, statistics::units::Count::get(),
               "Accepted branch and machine-clear episodes"),
      ADD_STAT(UNKNOWN_BRANCH_CYCLES, statistics::units::Cycle::get(),
               "Pending accepted untracked-branch resteer cycles"),
      ADD_STAT(superseded, statistics::units::Count::get(),
               "Accepted clears superseding a still-pending episode")
{}

void
CPU::RecoveryStats::account()
{
    const uint64_t now = uint64_t(owner->curCycle());
    const uint64_t elapsed = now - lastCycle;
    for (ThreadID tid = 0; tid < owner->numThreads; ++tid) {
        const auto &episode = episodes[tid];
        if (!owner->unhaltedClockStats.running[tid]) continue;
        if (episode.phase == Phase::Recovering && episode.allocatorStalled)
            RECOVERY_CYCLES += elapsed;
        if (episode.phase == Phase::Resteering)
            CLEAR_RESTEER_CYCLES += elapsed;
        if (episode.phase != Phase::Idle &&
            episode.cause == RecoveryCause::UnknownBranch)
            UNKNOWN_BRANCH_CYCLES += elapsed;
    }
    lastCycle = now;
}

bool
CPU::RecoveryStats::pending(ThreadID tid) const
{
    return episodes.at(tid).phase != Phase::Idle;
}

bool
CPU::RecoveryStats::branchRecovery(ThreadID tid) const
{
    const auto cause = episodes.at(tid).cause;
    return pending(tid) && (cause == RecoveryCause::Branch ||
        cause == RecoveryCause::FrontendBranch ||
        cause == RecoveryCause::UnknownBranch);
}

void
CPU::RecoveryStats::accept(ThreadID tid, InstSeqNum sequence,
                           RecoveryCause cause)
{
    if (cause == RecoveryCause::None) return;
    account();
    if (pending(tid)) ++superseded;
    episodes.at(tid) = Episode{};
    auto &episode = episodes.at(tid);
    episode.cause = cause;
    episode.sequence = sequence;
    episode.phase = (cause == RecoveryCause::FrontendBranch ||
                     cause == RecoveryCause::UnknownBranch) ?
                        Phase::Resteering : Phase::Recovering;
    ++CLEARS_COUNT;
    if (cause == RecoveryCause::UnknownBranch) ++owner->baClearStats.ANY;
    if (cause == RecoveryCause::Microcode) return;
    if (branchRecovery(tid)) return;
    auto &stats = owner->machineClearStats;
    ++stats.COUNT;
    switch (cause) {
      case RecoveryCause::MemoryOrdering:
        ++stats.MEMORY_ORDERING;
        ++stats.orderingCandidates;
        break;
      case RecoveryCause::FastForward:
        ++stats.fsfp;
        ++stats.orderingCandidates;
        break;
      case RecoveryCause::Snoop:
        ++stats.snoop;
        ++stats.orderingCandidates;
        break;
      case RecoveryCause::Code: ++stats.code; break;
      default: ++stats.other; break;
    }
}

void
CPU::RecoveryStats::allocatorState(ThreadID tid, bool squashing)
{
    account();
    auto &episode = episodes.at(tid);
    episode.allocatorStalled = squashing;
    if (!pending(tid)) return;
    if (squashing) episode.sawAllocatorSquash = true;
    else if (episode.sawAllocatorSquash && episode.phase == Phase::Recovering)
        episode.phase = Phase::Resteering;
}

void
CPU::RecoveryStats::restarted(ThreadID tid)
{
    account();
    if (pending(tid))
        episodes.at(tid).restartSequence = owner->globalSeqNum;
}

void
CPU::RecoveryStats::allocated(const DynInstPtr &inst)
{
    if (!inst->allocationSlots()) return;
    account();
    auto &episode = episodes.at(inst->threadNumber);
    if (pending(inst->threadNumber) &&
        inst->seqNum >= episode.restartSequence)
        episode = Episode{};
}

void
CPU::RecoveryStats::preDumpStats()
{
    account();
    statistics::Group::preDumpStats();
}

void
CPU::RecoveryStats::resetStats()
{
    account();
    statistics::Group::resetStats();
}

CPU::MoveEliminationStats::MoveEliminationStats(CPU *cpu)
    : statistics::Group(cpu, "moveElimination"),
      ADD_STAT(accepted, statistics::units::Count::get(), "Full-width GPR and XMM moves renamed as physical-register aliases"),
      ADD_STAT(retired, statistics::units::Count::get(), "Retired move aliases; allocation and ROB slots retained")
{}

CPU::MemoryInstRetiredStats::MemoryInstRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "MEM_INST_RETIRED"),
      ADD_STAT(ALL_LOADS, statistics::units::Count::get(), "Retired architectural instructions containing a memory load, including software prefetch; port IO excluded"),
      ADD_STAT(ALL_STORES, statistics::units::Count::get(), "Retired architectural instructions containing a memory store; port IO and prefetch excluded"),
      ADD_STAT(ANY, statistics::units::Count::get(), "Candidate union of retired architectural load and store instructions"),
      ADD_STAT(SPLIT_LOADS, statistics::units::Count::get(), "Retired load instructions with an effective access spanning cache lines"),
      ADD_STAT(SPLIT_STORES, statistics::units::Count::get(), "Retired store instructions with an effective access spanning cache lines"),
      ADD_STAT(LOCK_LOADS, statistics::units::Count::get(), "Retired locked load instructions including implicit memory XCHG"),
      ADD_STAT(STLB_MISS_LOADS, statistics::units::Count::get(), "Retired load instructions with a request-local L1 and second-level TLB miss"),
      ADD_STAT(STLB_MISS_STORES, statistics::units::Count::get(), "Retired store instructions with a request-local L1 and second-level TLB miss"),
      ADD_STAT(ALL_STORES_USER, statistics::units::Count::get(), "Model retired store instructions whose store members retired at CPL 3"),
      ADD_STAT(ALL_STORES_NONUSER, statistics::units::Count::get(), "Model retired store instructions whose store members retired outside CPL 3"),
      ADD_STAT(ALL_STORES_MIXED, statistics::units::Count::get(), "Model retired store instructions with store members spanning CPL classes")
{}

CPU::MemoryLoadRetiredStats::MemoryLoadRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "MEM_LOAD_RETIRED"),
      ADD_STAT(L1_HIT, statistics::units::Count::get(), "Candidate retired L1 hits including full-coverage store forwards, software prefetches and locked operations"),
      ADD_STAT(L1_HIT_NO_FORWARD, statistics::units::Count::get(), "Retired observed L1 hits plus software prefetches and locked operations, without inferring hits from store forwarding"),
      ADD_STAT(L1_OBSERVED_HIT, statistics::units::Count::get(), "Retired load instructions with an observed L1 data hit"),
      ADD_STAT(L1_MISS, statistics::units::Count::get(), "Retired load instructions with an observed native L1 data miss"),
      ADD_STAT(L2_HIT, statistics::units::Count::get(), "Retired load instructions with an observed native L2 data hit"),
      ADD_STAT(L2_MISS, statistics::units::Count::get(), "Retired load instructions with an observed native L2 data miss"),
      ADD_STAT(L3_HIT, statistics::units::Count::get(), "Retired load instructions with an observed native LLC data hit"),
      ADD_STAT(L3_MISS, statistics::units::Count::get(), "Retired load instructions with an observed native LLC data miss"),
      ADD_STAT(FB_HIT, statistics::units::Count::get(), "Retired load instructions with an observed pending L1 data fill"),
      ADD_STAT(storeForward, statistics::units::Count::get(), "Retired load instructions supplied by a full-coverage LSQ store forward"),
      ADD_STAT(sourceIncompleteResponse, statistics::units::Count::get(), "Retired loads with unresolved or incomplete Ruby admission/response observations"),
      ADD_STAT(sharedFillServiceSource, statistics::units::Count::get(), "Retired loads consuming a shared fill, by modeled supplier; classes may overlap across fragments"),
      ADD_STAT(sourceUnobservedL1, statistics::units::Count::get(), "Retired loads with at least one Ruby request lacking a local L1 observation, including Sequencer-shared service"),
      ADD_STAT(sourceMixedLocalEvents, statistics::units::Count::get(), "Retired loads whose observed local outcomes do not satisfy the source mapping"),
      ADD_STAT(sourceSequencerShared, statistics::units::Count::get(), "Retired loads served by a Sequencer follower without a request-local L1 observation"),
      ADD_STAT(sourceOtherTerminal, statistics::units::Count::get(), "Retired loads with a recorded non-forwarding terminal path"),
      ADD_STAT(sourcePermissionUpgrade, statistics::units::Count::get(), "Retired loads with an observed L1 permission upgrade"),
      ADD_STAT(sourceRemoteSupply, statistics::units::Count::get(), "Retired loads with an observed L1 remote supply"),
      ADD_STAT(sharedParentL1Hit, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an L1 hit"),
      ADD_STAT(sharedParentL1Miss, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an L1 miss"),
      ADD_STAT(sharedParentL2Hit, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an L2 hit"),
      ADD_STAT(sharedParentL2Miss, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an L2 miss"),
      ADD_STAT(sharedParentL3Hit, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an L3 hit"),
      ADD_STAT(sharedParentL3Miss, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an L3 miss"),
      ADD_STAT(sharedParentOther, statistics::units::Count::get(), "Retired shared-service loads whose parent observed an upgrade, merge or remote supply"),
      ADD_STAT(sharedParentIFetch, statistics::units::Count::get(), "Retired shared-service loads with an instruction-fetch parent"),
      ADD_STAT(sharedParentMissing, statistics::units::Count::get(), "Retired shared-service loads whose parent lacks a required hierarchy observation"),
      ADD_STAT(sharedParentWriteCallback, statistics::units::Count::get(), "Retired shared-service loads completed during a write callback"),
      ADD_STAT(sourcePrefetchInFlight, statistics::units::Count::get(), "Retired software prefetches with a request still in flight; no demand data is consumed"),
      ADD_STAT(incompleteNoRequest, statistics::units::Count::get(), "Incomplete sources without a saved main request"),
      ADD_STAT(incompleteNoResponse, statistics::units::Count::get(), "Incomplete sources with no Ruby responses"),
      ADD_STAT(incompleteOpen, statistics::units::Count::get(), "Incomplete sources whose issuance has not closed"),
      ADD_STAT(incompleteMismatch, statistics::units::Count::get(), "Incomplete sources with unequal admissions and responses"),
      ADD_STAT(incompletePrefetch, statistics::units::Count::get(), "Incomplete sources on software prefetch instructions"),
      ADD_STAT(incompleteAtomic, statistics::units::Count::get(), "Incomplete sources on atomic instructions"),
      ADD_STAT(l2MissingLookup, statistics::units::Count::get(), "L2 association gaps without an L2 observation"),
      ADD_STAT(l2OtherOutcome, statistics::units::Count::get(), "L2 association gaps involving an L2 upgrade, merge or remote supply"),
      ADD_STAT(l2CountMismatch, statistics::units::Count::get(), "L2 association gaps with differing L1 miss and L2 lookup counts"),
      ADD_STAT(l2MismatchSplit, statistics::units::Count::get(), "L2 count mismatches involving a split request"),
      ADD_STAT(l2MismatchLocked, statistics::units::Count::get(), "L2 count mismatches involving an architectural locked access"),
      ADD_STAT(l2MismatchCoalesced, statistics::units::Count::get(), "L2 count mismatches involving a Sequencer follower"),
      ADD_STAT(sourceL2AssociationGap, statistics::units::Count::get(), "Retired loads without a complete association between L1 misses and L2 outcomes"),
      ADD_STAT(sourceL3AssociationGap, statistics::units::Count::get(), "Retired loads without a complete association between L2 misses and L3 outcomes")
{
    sharedFillServiceSource.init(5);
    sharedFillServiceSource.subname(0, "unknown");
    sharedFillServiceSource.subname(1, "l2");
    sharedFillServiceSource.subname(2, "llc");
    sharedFillServiceSource.subname(3, "memory_response");
    sharedFillServiceSource.subname(4, "remote_owner");
}

CPU::MemoryLoadCompletedStats::MemoryLoadCompletedStats(CPU *cpu)
    : statistics::Group(cpu, "MEM_LOAD_COMPLETED"),
      ADD_STAT(L1_MISS_ANY, statistics::units::Count::get(),
               "Completed demand load uops with an observed L1 miss or fill-buffer join")
{}

void
CPU::recordCompletedLoad(const DynInstPtr &inst)
{
    if (!memoryObservationActive || !inst || inst->isSquashed() ||
        !inst->isLoad() || inst->isDataPrefetch() || inst->isAtomic() ||
        !inst->savedRequest || inst->fault != NoFault)
        return;
    const auto req = inst->savedRequest->mainReq();
    if (!req || req->isUncacheable() || req->isStrictlyOrdered() ||
        req->isLocalAccess() || req->isLLSC() || req->isReadModifyWrite())
        return;
    const auto facts = req->getExtension<TaoTraceRubyResponseExtension>();
    if (facts && (facts->l1FinalMask & ((1u<<2) | (1u<<3))))
        ++memoryLoadCompletedStats.L1_MISS_ANY;
}

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
    pending.lockedLoad |= load && locked;
    if ((load || store) && inst->savedRequest) {
        bool stlb_miss = false;
        for (const auto &fragment : inst->savedRequest->_reqs) {
            const auto facts = fragment->getExtension<TranslationMissExtension>();
            stlb_miss |= facts && facts->stlbMiss;
        }
        pending.stlbMissLoad |= load && stlb_miss;
        pending.stlbMissStore |= store && stlb_miss;
        if (inst->effAddrValid() && inst->effSize &&
            inst->readMemAccPredicate()) {
            const auto line = cacheLineSize();
            const bool split = inst->effAddr / line !=
                (inst->effAddr + inst->effSize - 1) / line;
            pending.splitLoad |= load && split;
            pending.splitStore |= store && split;
        }
    }
    if (store) {
        // Commit invokes this before updating the retiring instruction's
        // architectural misc registers. Track each member independently so a
        // microcoded cross-privilege instruction is not counted in both.
        if (inUserMode(tid)) pending.storeUserSeen = true;
        else pending.storeNonuserSeen = true;
    }
    const auto context = uint32_t(tcBase(tid)->contextId());
    if (load) {
        const auto req = inst->savedRequest ? inst->savedRequest->mainReq() : nullptr;
        const auto terminal_reason =
            (!inst->readPredicate() || !inst->readMemAccPredicate()) ?
                TaoTraceNativeTerminalReason::PredicatedOff :
            !inst->hasRequest() ? TaoTraceNativeTerminalReason::NoRequest :
                TaoTraceNativeTerminalReason::None;
        if (terminal_reason != TaoTraceNativeTerminalReason::None) {
            if (req)
                TaoTraceNativeAccessRegistry::noteTerminal(req, terminal_reason);
            else
                TaoTraceNativeAccessRegistry::noteTerminal(
                    context, inst->seqNum, terminal_reason);
        }
        const auto facts = TaoTraceNativeAccessRegistry::snapshot(context, inst->seqNum);
        const auto extension = req ? req->getExtension<TaoTraceRubyResponseExtension>() : nullptr;
        const auto &h = extension ? extension->hierarchy : facts.hierarchy;
        const uint32_t forward = taoTraceNativeReasonMask(
            TaoTraceNativeTerminalReason::StoreForward);
        retirement::NativeFacts native;
        native.resolved = extension ?
            (extension->responses == extension->admissions &&
             (extension->issuanceClosed || extension->terminalNoRuby)) :
            facts.resolved();
        const auto terminal = extension ? extension->terminalReasonMask : facts.terminalReasonMask;
        native.store_forward = terminal & forward;
        native.other_terminal = terminal & ~forward;
        native.admissions = extension ? extension->admissions : facts.admissions;
        native.responses = extension ? extension->responses : facts.responses;
        native.coalesced = extension ? extension->coalesced : facts.coalesced;
        native.sequencer_shared = extension ? extension->sequencerShared : facts.sequencerShared;
        native.hierarchy_requests = extension ? extension->hierarchyRequests : facts.hierarchyRequests;
        native.external_hits = extension ? extension->externalHits : facts.externalHits;
        native.external_misses = extension ? extension->externalMisses : facts.externalMisses;
        native.accesses = h.accesses[0];
        native.hits = h.hits[0];
        native.tag_misses = h.tagMisses[0];
        native.permission_upgrades = h.permissionUpgrades[0];
        native.merged_misses = h.mergedMisses[0];
        native.remote_supplies = h.remoteSupplies[0];
        native.l2_accesses = h.accesses[1];
        native.l2_hits = h.hits[1];
        native.l2_tag_misses = h.tagMisses[1];
        native.l2_other = h.permissionUpgrades[1] + h.mergedMisses[1] +
                          h.remoteSupplies[1];
        native.l3_accesses = h.accesses[2];
        native.l3_hits = h.hits[2];
        native.l3_tag_misses = h.tagMisses[2];
        native.l3_other = h.permissionUpgrades[2] + h.mergedMisses[2] +
                          h.remoteSupplies[2];
        native.l1_final_mask = extension ? extension->l1FinalMask : facts.l1FinalMask;
        native.service_expected = extension ? extension->serviceExpected : facts.serviceExpected;
        native.service_identity_ambiguous = extension ?
            extension->serviceFactIdentityAmbiguous : facts.serviceFactIdentityAmbiguous;
        retirement::addServiceFacts(native,
            extension ? extension->serviceFacts : facts.serviceFacts);
        const auto &services = extension ? extension->serviceFacts : facts.serviceFacts;
        for (const auto &service : services) {
            if (service.follower)
                pending.sharedFillSources |= 1u << (service.source <= 4 ? service.source : 0);
        }
        // Software prefetches retire without consuming returned data. The
        // eventual cache response can legitimately follow retirement.
        const bool prefetch_in_flight = prefetch && !native.resolved &&
            native.admissions > native.responses && !native.responses;
        pending.prefetchInFlight |= prefetch_in_flight;
        const unsigned upper = prefetch_in_flight ? retirement::OtherTerminal :
            retirement::classifyIndependent(native);
        const unsigned lower = prefetch_in_flight ? 0 :
            retirement::classifyRetiredCache(native);
        uint8_t source = 0;
        // SPR's L1_HIT separately includes software prefetch and locked
        // instructions regardless of the model's observed data source.
        if ((upper & retirement::L1) || prefetch || locked) source |= 1;
        pending.physicalL1Hit |= (native.l1_final_mask & (1u<<1)) != 0;
        pending.l1HitWithoutForward |= pending.physicalL1Hit || prefetch || locked;
        if (upper & retirement::Miss) source |= 2;
        if (lower & retirement::L2Hit) source |= 4;
        if (lower & retirement::L2Miss) source |= 8;
        if (lower & retirement::L3Hit) source |= 16;
        if (lower & retirement::L3Miss) source |= 32;
        if (upper & retirement::FillBuffer) source |= 64;
        if (upper & retirement::Forwarded) source |= 128;
        pending.sources |= source;
        pending.sequencerShared |= native.sequencer_shared != 0;
        // Forwarding and software prefetch retirement need not produce a
        // demand-data L1 observation. For actual Ruby admissions, count a
        // missing local observation even when the Sequencer classified the
        // request as shared rather than Unknown.
        if (!prefetch && native.admissions &&
            !(native.resolved && native.store_forward && !native.other_terminal)) {
            // The main Request contains the per-fragment OR for split loads.
            // An instruction-scoped registry cannot prove attempt identity.
            pending.unobservedL1 |= extension ?
                (extension->l1ObservationMissing || extension->l1ObservationPending()) :
                (facts.l1ObservationMissing || facts.serviceFactIdentityAmbiguous ||
                 !facts.resolved());
        }
        pending.otherTerminal |= native.other_terminal;
        pending.permissionUpgrade |= native.permission_upgrades != 0;
        pending.remoteSupply |= native.remote_supplies != 0;
        pending.sharedParentSources |= extension ?
            extension->sharedParentSources : facts.sharedParentSources;
        if (upper & retirement::Unknown) {
            uint8_t gaps = 0;
            if (!native.resolved || !native.responses ||
                native.responses != native.admissions) {
                gaps |= 1;
                if (!req) pending.incompleteReasons |= 1;
                if (!native.responses) pending.incompleteReasons |= 2;
                if (extension && !extension->issuanceClosed && !extension->terminalNoRuby)
                    pending.incompleteReasons |= 4;
                if (native.responses != native.admissions) pending.incompleteReasons |= 8;
                if (prefetch) pending.incompleteReasons |= 16;
                if (inst->isAtomic()) pending.incompleteReasons |= 32;
            }
            if (native.responses &&
                native.accesses + native.sequencer_shared < native.admissions)
                gaps |= 2;
            if (!gaps) gaps |= 4;
            pending.sourceGaps |= gaps;
        }
        if (lower & retirement::L2Unknown) {
            pending.sourceGaps |= 8;
            if (!native.l2_accesses) pending.l2Reasons |= 1;
            if (native.l2_other) pending.l2Reasons |= 2;
            if (native.l2_accesses != native.tag_misses) {
                pending.l2Reasons |= 4;
                if (inst->savedRequest && inst->savedRequest->isSplit()) pending.l2MismatchKinds |= 1;
                if (locked || inst->isAtomic()) pending.l2MismatchKinds |= 2;
                if (native.coalesced) pending.l2MismatchKinds |= 4;
            }
        }
        if (lower & retirement::L3Unknown) pending.sourceGaps |= 16;
    }
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        if (pending.load || pending.store) ++memoryInstRetiredStats.ANY;
        if (pending.splitLoad) ++memoryInstRetiredStats.SPLIT_LOADS;
        if (pending.splitStore) ++memoryInstRetiredStats.SPLIT_STORES;
        if (pending.lockedLoad) ++memoryInstRetiredStats.LOCK_LOADS;
        if (pending.stlbMissLoad) ++memoryInstRetiredStats.STLB_MISS_LOADS;
        if (pending.stlbMissStore) ++memoryInstRetiredStats.STLB_MISS_STORES;
        if (pending.load) {
            ++memoryInstRetiredStats.ALL_LOADS;
            if (pending.sources & 1) ++memoryLoadRetiredStats.L1_HIT;
            if (pending.l1HitWithoutForward) ++memoryLoadRetiredStats.L1_HIT_NO_FORWARD;
            if (pending.physicalL1Hit) ++memoryLoadRetiredStats.L1_OBSERVED_HIT;
            if (pending.sources & 2) ++memoryLoadRetiredStats.L1_MISS;
            if (pending.sources & 4) ++memoryLoadRetiredStats.L2_HIT;
            if (pending.sources & 8) ++memoryLoadRetiredStats.L2_MISS;
            if (pending.sources & 16) ++memoryLoadRetiredStats.L3_HIT;
            if (pending.sources & 32) ++memoryLoadRetiredStats.L3_MISS;
            if (pending.sources & 64) ++memoryLoadRetiredStats.FB_HIT;
            if (pending.sources & 128) ++memoryLoadRetiredStats.storeForward;
            for (unsigned source = 0; source < 5; ++source) {
                if (pending.sharedFillSources & (1u << source))
                    ++memoryLoadRetiredStats.sharedFillServiceSource[source];
            }
            if (pending.sourceGaps & 1) ++memoryLoadRetiredStats.sourceIncompleteResponse;
            if (pending.unobservedL1) ++memoryLoadRetiredStats.sourceUnobservedL1;
            if (pending.sourceGaps & 4) ++memoryLoadRetiredStats.sourceMixedLocalEvents;
            if (pending.sequencerShared) ++memoryLoadRetiredStats.sourceSequencerShared;
            if (pending.otherTerminal) ++memoryLoadRetiredStats.sourceOtherTerminal;
            if (pending.permissionUpgrade) ++memoryLoadRetiredStats.sourcePermissionUpgrade;
            if (pending.remoteSupply) ++memoryLoadRetiredStats.sourceRemoteSupply;
            if (pending.sharedParentSources & SharedParentL1Hit) ++memoryLoadRetiredStats.sharedParentL1Hit;
            if (pending.sharedParentSources & SharedParentL1Miss) ++memoryLoadRetiredStats.sharedParentL1Miss;
            if (pending.sharedParentSources & SharedParentL2Hit) ++memoryLoadRetiredStats.sharedParentL2Hit;
            if (pending.sharedParentSources & SharedParentL2Miss) ++memoryLoadRetiredStats.sharedParentL2Miss;
            if (pending.sharedParentSources & SharedParentL3Hit) ++memoryLoadRetiredStats.sharedParentL3Hit;
            if (pending.sharedParentSources & SharedParentL3Miss) ++memoryLoadRetiredStats.sharedParentL3Miss;
            if (pending.sharedParentSources & (SharedParentL1Other|SharedParentL2Other|SharedParentL3Other)) ++memoryLoadRetiredStats.sharedParentOther;
            if (pending.sharedParentSources & SharedParentIFetch) ++memoryLoadRetiredStats.sharedParentIFetch;
            if (pending.sharedParentSources & SharedParentMissing) ++memoryLoadRetiredStats.sharedParentMissing;
            if (pending.sharedParentSources & SharedParentWriteCallback) ++memoryLoadRetiredStats.sharedParentWriteCallback;
            if (pending.prefetchInFlight) ++memoryLoadRetiredStats.sourcePrefetchInFlight;
            if (pending.incompleteReasons & 1) ++memoryLoadRetiredStats.incompleteNoRequest;
            if (pending.incompleteReasons & 2) ++memoryLoadRetiredStats.incompleteNoResponse;
            if (pending.incompleteReasons & 4) ++memoryLoadRetiredStats.incompleteOpen;
            if (pending.incompleteReasons & 8) ++memoryLoadRetiredStats.incompleteMismatch;
            if (pending.incompleteReasons & 16) ++memoryLoadRetiredStats.incompletePrefetch;
            if (pending.incompleteReasons & 32) ++memoryLoadRetiredStats.incompleteAtomic;
            if (pending.l2Reasons & 1) ++memoryLoadRetiredStats.l2MissingLookup;
            if (pending.l2Reasons & 2) ++memoryLoadRetiredStats.l2OtherOutcome;
            if (pending.l2Reasons & 4) ++memoryLoadRetiredStats.l2CountMismatch;
            if (pending.l2MismatchKinds & 1) ++memoryLoadRetiredStats.l2MismatchSplit;
            if (pending.l2MismatchKinds & 2) ++memoryLoadRetiredStats.l2MismatchLocked;
            if (pending.l2MismatchKinds & 4) ++memoryLoadRetiredStats.l2MismatchCoalesced;
            if (pending.sourceGaps & 8) ++memoryLoadRetiredStats.sourceL2AssociationGap;
            if (pending.sourceGaps & 16) ++memoryLoadRetiredStats.sourceL3AssociationGap;
        }
        if (pending.store) {
            ++memoryInstRetiredStats.ALL_STORES;
            if (pending.storeUserSeen && pending.storeNonuserSeen)
                ++memoryInstRetiredStats.ALL_STORES_MIXED;
            else if (pending.storeUserSeen)
                ++memoryInstRetiredStats.ALL_STORES_USER;
            else
                ++memoryInstRetiredStats.ALL_STORES_NONUSER;
        }
        pending = {};
    }
}

CPU::InstRetiredStats::InstRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "INST_RETIRED"),
      ADD_STAT(ANY, statistics::units::Count::get(),
               "Retired architectural instructions, including NOPs, across this O3 core"),
      ADD_STAT(NOP, statistics::units::Count::get(),
               "Retired architectural NOP and ENDBR encodings; excludes PAUSE and helpers"),
      ADD_STAT(MACRO_FUSED, statistics::units::Count::get(),
               "Retired flag-producer/branch allocation groups, counted at the architectural branch"),
      ADD_STAT(repProgressUpdates, statistics::units::Count::get(),
               "Successfully committed REP bulk/scalar progress records; not hardware REP_ITERATION"),
      ADD_STAT(repProgressElements, statistics::units::Count::get(),
               "Elements completed by committed REP progress; not hardware fast-string iteration count")
{}

CPU::FrontendRetiredStats::FrontendRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "FRONTEND_RETIRED"),
      ADD_STAT(ANY_DSB_MISS, statistics::units::Count::get(),
               "Retired architectural instructions decoded after a DSB template miss"),
      ADD_STAT(DSB_MISS, statistics::units::Count::get(),
               "Retired DSB-miss instructions with observed receiver-accepting supply deficit")
{}

CPU::DecodeStats::DecodeStats(CPU *cpu)
    : statistics::Group(cpu, "DECODE"),
      ADD_STAT(LCP, statistics::units::Cycle::get(),
               "MITE ILD stalls for each qualified operand-sized immediate prefix in a traversed 16-byte block")
{}

CPU::BaClearStats::BaClearStats(CPU *cpu)
    : statistics::Group(cpu, "BACLEARS"),
      ADD_STAT(ANY, statistics::units::Count::get(),
               "Accepted frontend redirects for architectural branches lacking a supplying predictor history")
{}

CPU::IcacheStallStats::IcacheStallStats(CPU *cpu, const char *group)
    : statistics::Group(cpu, group), owner(cpu),
      ADD_STAT(STALLS, statistics::units::Cycle::get(),
               "Candidate cycles awaiting a live L1I tag miss while no other frontend source is ready; tag/data phases are not distinguished")
{}

void
CPU::IcacheStallStats::preDumpStats()
{
    owner->fetch.accountIcacheStalls();
    statistics::Group::preDumpStats();
}

CPU::SoftwarePrefetchStats::SoftwarePrefetchStats(CPU *cpu)
    : statistics::Group(cpu, "SW_PREFETCH_ACCESS"),
      ADD_STAT(NTA, statistics::units::Count::get(), "Executed PREFETCHNTA hints, including speculation and replay"),
      ADD_STAT(T0, statistics::units::Count::get(), "Executed PREFETCHT0 hints, including speculation and replay"),
      ADD_STAT(T1_T2, statistics::units::Count::get(), "Executed PREFETCHT1/T2 hints; hierarchy level hints are not enforced"),
      ADD_STAT(PREFETCHW, statistics::units::Count::get(), "Executed PREFETCHW hints"),
      ADD_STAT(ANY, statistics::units::Count::get(), "All executed supported software-prefetch hints")
{}

CPU::FpArithmeticStats::FpArithmeticStats(CPU *cpu)
    : statistics::Group(cpu, "FP_ARITH_INST_RETIRED"),
      ADD_STAT(SCALAR_DOUBLE, statistics::units::Count::get(), "Retired original scalar-double SSE arithmetic"),
      ADD_STAT(SCALAR_SINGLE, statistics::units::Count::get(), "Retired original scalar-single SSE arithmetic"),
      ADD_STAT(PACKED_128B_DOUBLE, statistics::units::Count::get(),
               "Retired original packed-double 128-bit SSE arithmetic"),
      ADD_STAT(PACKED_128B_SINGLE, statistics::units::Count::get(),
               "Retired original packed-single 128-bit SSE arithmetic"),
      ADD_STAT(mxcsrDazFtz, statistics::units::Count::get(),
               "Retired SSE arithmetic with both MXCSR DAZ and FTZ set"),
      ADD_STAT(mxcsrOther, statistics::units::Count::get(),
               "Retired SSE arithmetic without both required MXCSR DAZ and FTZ bits")
{}

CPU::BranchInstStats::BranchInstStats(CPU *cpu)
    : statistics::Group(cpu, "BR_INST_RETIRED"),
      ADD_STAT(ALL_BRANCHES, statistics::units::Count::get(), "Retired architectural branches"),
      ADD_STAT(COND, statistics::units::Count::get(), "Retired conditional branches"),
      ADD_STAT(INDIRECT, statistics::units::Count::get(), "Retired near indirect branches excluding TSX"),
      ADD_STAT(NEAR_RETURN, statistics::units::Count::get(), "Retired near returns"),
      ADD_STAT(COND_TAKEN, statistics::units::Count::get(), "Retired conditional branches resolved taken"),
      ADD_STAT(COND_NTAKEN, statistics::units::Count::get(), "Retired conditional branches resolved not taken"),
      ADD_STAT(NEAR_CALL, statistics::units::Count::get(), "Retired direct and indirect near CALL encodings, including zero displacement"),
      ADD_STAT(NEAR_TAKEN, statistics::units::Count::get(), "Retired near branches resolved taken"),
      ADD_STAT(FAR_BRANCH, statistics::units::Count::get(), "Retired far transfers and system branch encodings"),
      ADD_STAT(unresolvedDirection, statistics::units::Count::get(), "Retired architectural branches without an execution direction fact")
{}

CPU::UopsRetiredStats::UopsRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_RETIRED"), owner(cpu),
      ADD_STAT(SLOTS, statistics::units::Count::get(),
               "Retirement-domain slots, counted when the retirement group's final record commits"),
      ADD_STAT(CYCLES, statistics::units::Cycle::get(),
               "Cycles with a retirement-domain slot committed"),
      ADD_STAT(STALLS, statistics::units::Cycle::get(),
               "Candidate active-thread cycles without a retired slot; single-thread scope and snapshot boundaries apply"),
      ADD_STAT(HEAVY, statistics::units::Count::get(),
               "Candidate excess model retirement uops over one per successfully retired architectural instruction"),
      ADD_STAT(MS, statistics::units::Count::get(),
               "Model retirement uops of successfully retired instructions carrying microcode identity"),
      ADD_STAT(completedGroups, statistics::units::Count::get(),
               "Retirement groups whose final functional member committed successfully"),
      ADD_STAT(interruptedGroups, statistics::units::Count::get(),
               "Started retirement groups interrupted before their final member; no partial slots inferred"),
      ADD_STAT(faultInterruptedGroups, statistics::units::Count::get(),
               "Started retirement groups interrupted by a fault before their final member"),
      ADD_STAT(interruptedInstructions, statistics::units::Count::get(),
               "Architectural instructions with committed model uops but no successful final member")
{}

void
CPU::UopsRetiredStats::preDumpStats()
{
    owner->unhaltedClockStats.account();
    const auto cycles = owner->unhaltedClockStats.THREAD.value();
    panic_if(CYCLES.value() > cycles, "Retirement activity exceeds active thread cycles");
    STALLS = cycles - CYCLES.value();
    statistics::Group::preDumpStats();
}

CPU::IdqStats::IdqStats(CPU *cpu)
    : statistics::Group(cpu, "IDQ"),
      ADD_STAT(MITE_UOPS, statistics::units::Count::get(), "MITE units actually delivered to IDQ, including speculative work"),
      ADD_STAT(DSB_UOPS, statistics::units::Count::get(), "DSB units actually delivered to IDQ, including speculative work"),
      ADD_STAT(MS_UOPS, statistics::units::Count::get(), "Microcode units actually delivered to IDQ; SPR121 hardware undercount is not emulated"),
      ADD_STAT(MITE_CYCLES_ANY, statistics::units::Cycle::get(), "Cycles with positive MITE delivery"),
      ADD_STAT(MITE_CYCLES_OK, statistics::units::Cycle::get(), "Cycles with at least six MITE delivery units"),
      ADD_STAT(DSB_CYCLES_ANY, statistics::units::Cycle::get(), "Cycles with positive DSB delivery"),
      ADD_STAT(DSB_CYCLES_OK, statistics::units::Cycle::get(), "Cycles with at least six DSB delivery units"),
      ADD_STAT(MS_CYCLES_ANY, statistics::units::Cycle::get(), "Cycles with positive microcode delivery"),
      ADD_STAT(MS_SWITCHES, statistics::units::Count::get(), "Actual positive IDQ deliveries entering the MS source"),
      ADD_STAT(knownMicrocodeUnits, statistics::units::Count::get(), "Delivered units with evidence-backed instruction microcode identity"),
      ADD_STAT(inferredMicrocodeUnits, statistics::units::Count::get(), "Delivered units classified by internal functional control alone")
{}

CPU::LsdStats::LsdStats(CPU *cpu)
    : statistics::Group(cpu, "LSD"),
      ADD_STAT(UOPS, statistics::units::Count::get(), "LSD replay units actually sent toward the backend"),
      ADD_STAT(CYCLES_ACTIVE, statistics::units::Cycle::get(), "Cycles with positive LSD replay delivery"),
      ADD_STAT(CYCLES_OK, statistics::units::Cycle::get(), "Cycles with at least six LSD replay delivery units")
{}

CPU::UopsIssuedStats::UopsIssuedStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_ISSUED"),
      ADD_STAT(ANY, statistics::units::Count::get(), "Allocation-domain units admitted by Rename after resource reservation; includes later squashed work"),
      ADD_STAT(CYCLES, statistics::units::Cycle::get(), "Thread cycles with positive Rename admission")
{}

CPU::UopsExecutedStats::UopsExecutedStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_EXECUTED"), owner(cpu),
      ADD_STAT(THREAD, statistics::units::Count::get(), "Execution actions accepted by an execution port, including true replay"),
      ADD_STAT(CYCLES_GE_1, statistics::units::Cycle::get(), "Cycles with at least one accepted execution action"),
      ADD_STAT(CYCLES_GE_2, statistics::units::Cycle::get(), "Cycles with at least two accepted execution actions"),
      ADD_STAT(CYCLES_GE_3, statistics::units::Cycle::get(), "Cycles with at least three accepted execution actions"),
      ADD_STAT(CYCLES_GE_4, statistics::units::Cycle::get(), "Cycles with at least four accepted execution actions"),
      ADD_STAT(STALLS, statistics::units::Cycle::get(),
               "Candidate active-thread cycles without an accepted execution action; single-thread scope and snapshot boundaries apply"),
      ADD_STAT(X87, statistics::units::Count::get(),
               "Port actions of original architectural x87 instructions"),
      ADD_STAT(unmappedPortActions, statistics::units::Count::get(),
               "Accepted FU actions without a configured physical execution-port identity")
{}

void
CPU::UopsExecutedStats::preDumpStats()
{
    owner->unhaltedClockStats.account();
    const auto cycles = owner->unhaltedClockStats.THREAD.value();
    panic_if(CYCLES_GE_1.value() > cycles, "Execution activity exceeds active thread cycles");
    STALLS = cycles - CYCLES_GE_1.value();
    statistics::Group::preDumpStats();
}

CPU::UopsDispatchedStats::UopsDispatchedStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_DISPATCHED"),
      ADD_STAT(PORT_0, statistics::units::Count::get(), "Actions accepted on port 0"),
      ADD_STAT(PORT_1, statistics::units::Count::get(), "Actions accepted on port 1"),
      ADD_STAT(PORT_2_3_10, statistics::units::Count::get(), "Actions accepted on ports 2, 3, 10"),
      ADD_STAT(PORT_4_9, statistics::units::Count::get(), "Actions accepted on ports 4, 9"),
      ADD_STAT(PORT_5_11, statistics::units::Count::get(), "Actions accepted on ports 5, 11"),
      ADD_STAT(PORT_6, statistics::units::Count::get(), "Actions accepted on port 6"),
      ADD_STAT(PORT_7_8, statistics::units::Count::get(), "Actions accepted on ports 7, 8")
{}

CPU::RsStats::RsStats(CPU *cpu)
    : statistics::Group(cpu, "RS"), owner(cpu),
      ADD_STAT(EMPTY, statistics::units::Cycle::get(),
               "Active-thread cycles with no resident IQ record, observed after dispatch and before scheduling")
{}

void
CPU::RsStats::preDumpStats()
{
    owner->executionActivityStats.account();
    statistics::Group::preDumpStats();
}

CPU::ExecutionActivityStats::ExecutionActivityStats(CPU *cpu)
    : statistics::Group(cpu, "EXE_ACTIVITY"), owner(cpu),
      ADD_STAT(PORTS_UTIL_1, statistics::units::Cycle::get(),
               "Candidate cycles with exactly one port action and nonempty resident RS"),
      ADD_STAT(PORTS_UTIL_2, statistics::units::Cycle::get(),
               "Candidate cycles with exactly two port actions and nonempty resident RS"),
      ADD_STAT(PORTS_UTIL_3, statistics::units::Cycle::get(),
               "Candidate cycles with exactly three port actions and nonempty resident RS"),
      ADD_STAT(PORTS_UTIL_4, statistics::units::Cycle::get(),
               "Candidate cycles with exactly four port actions and nonempty resident RS"),
      ADD_STAT(rsNonemptyNoActionCycles, statistics::units::Cycle::get(),
               "Active cycles with resident RS records but no port action"),
      ADD_STAT(BOUND_ON_LOADS, statistics::units::Cycle::get(),
               "No-port-action candidate cycles with an actual demand load request in flight"),
      ADD_STAT(BOUND_ON_STORES, statistics::units::Cycle::get(),
               "No-port-action candidate cycles with full store resources and no demand load in flight"),
      ADD_STAT(loadTranslationCycles, statistics::units::Cycle::get(),
               "Observed cycles with a live demand-load timing translation"),
      ADD_STAT(loadAdmissionCycles, statistics::units::Cycle::get(),
               "Observed cycles with a translated demand load awaiting request admission"),
      ADD_STAT(loadResponseCycles, statistics::units::Cycle::get(),
               "Observed cycles with an accepted demand-load packet awaiting response"),
      ADD_STAT(loadCompletionCycles, statistics::units::Cycle::get(),
               "Observed cycles with returned load data awaiting functional completion")
{}

void
CPU::ExecutionActivityStats::account()
{
    const uint64_t now = uint64_t(owner->curCycle());
    if (valid && now > lastCycle) {
        const uint64_t elapsed = now - lastCycle;
        for (ThreadID tid = 0; tid < owner->numThreads; ++tid) {
            auto &sample = samples[tid];
            if (owner->unhaltedClockStats.running[tid]) {
                const uint64_t stalled = elapsed - (sample.actions ? 1 : 0);
                if (sample.loadStages) BOUND_ON_LOADS += stalled;
                else if (sample.storesFull) BOUND_ON_STORES += stalled;
                if (sample.loadStages & 1) loadTranslationCycles += elapsed;
                if (sample.loadStages & 2) loadAdmissionCycles += elapsed;
                if (sample.loadStages & 4) loadResponseCycles += elapsed;
                if (sample.loadStages & 8) loadCompletionCycles += elapsed;
                if (!sample.nonempty) {
                    owner->rsStats.EMPTY += elapsed;
                } else {
                    if (!sample.actions) ++rsNonemptyNoActionCycles;
                    switch (sample.actions) {
                      case 1: ++PORTS_UTIL_1; break;
                      case 2: ++PORTS_UTIL_2; break;
                      case 3: ++PORTS_UTIL_3; break;
                      case 4: ++PORTS_UTIL_4; break;
                    }
                    // With no CPU tick or action the saved residency remains
                    // unchanged. No unsampled port actions are invented.
                    rsNonemptyNoActionCycles += elapsed - 1;
                }
            }
            sample.actions = 0;
        }
    }
    lastCycle = now;
    valid = true;
}

void
CPU::ExecutionActivityStats::observe()
{
    account();
    for (ThreadID tid = 0; tid < owner->numThreads; ++tid) {
        samples[tid].nonempty = owner->iew.instQueue.getResidentCount(tid) != 0;
        samples[tid].loadStages = owner->iew.ldstQueue.demandLoadStages(tid);
        samples[tid].storesFull =
            owner->iew.ldstQueue.numFreeStoreEntries(tid) == 0;
    }
}

void
CPU::ExecutionActivityStats::action(ThreadID tid)
{
    account();
    ++samples.at(tid).actions;
}

void
CPU::ExecutionActivityStats::preDumpStats()
{
    account();
    statistics::Group::preDumpStats();
}

void
CPU::ExecutionActivityStats::resetStats()
{
    account();
    statistics::Group::resetStats();
    for (auto &sample : samples) sample.actions = 0;
}

CPU::DividerStats::DividerStats(CPU *cpu)
    : statistics::Group(cpu, "ARITH"), owner(cpu),
      ADD_STAT(IDIV_ACTIVE, statistics::units::Cycle::get(),
               "Integer arithmetic-active cycles, excluding output publication and ending early on cancellation"),
      ADD_STAT(FPDIV_ACTIVE, statistics::units::Cycle::get(),
               "Union of admitted floating divide or square-root arithmetic-active intervals"),
      ADD_STAT(DIV_ACTIVE, statistics::units::Cycle::get(),
               "Union of integer and floating divide busy intervals; concurrent units count once")
{}

void
CPU::DividerStats::account()
{
    const uint64_t now = uint64_t(owner->curCycle());
    const uint64_t elapsed = now - lastCycle;
    if (integerBusy) IDIV_ACTIVE += elapsed;
    if (floatingBusy) FPDIV_ACTIVE += elapsed;
    if (integerBusy || floatingBusy) DIV_ACTIVE += elapsed;
    lastCycle = now;
}

void
CPU::DividerStats::start(bool floating)
{
    account();
    ++(floating ? floatingBusy : integerBusy);
}

void
CPU::DividerStats::finish(bool floating)
{
    account();
    auto &busy = floating ? floatingBusy : integerBusy;
    panic_if(!busy, "Divider completion without a busy operation");
    --busy;
}

void
CPU::DividerStats::preDumpStats()
{
    account();
    statistics::Group::preDumpStats();
}

void
CPU::DividerStats::resetStats()
{
    account();
    statistics::Group::resetStats();
}

void
CPU::recordFrontendDelivery(FrontendSource source, unsigned units, uint64_t cycle)
{
    if (units) {
        if (source == FrontendSource::Microcode &&
            (!idqStats.haveSource || idqStats.previousSource != source))
            ++idqStats.MS_SWITCHES;
        idqStats.previousSource = source;
        idqStats.haveSource = true;
    }
    if (!units) return;
    statistics::Scalar *total, *any, *ok = nullptr;
    ActionCycle *activity;
    if (source == FrontendSource::Lsd) {
        total = &lsdStats.UOPS;
        any = &lsdStats.CYCLES_ACTIVE;
        ok = &lsdStats.CYCLES_OK;
        activity = &lsdStats.activity;
    } else if (source == FrontendSource::Mite) {
        total = &idqStats.MITE_UOPS;
        any = &idqStats.MITE_CYCLES_ANY;
        ok = &idqStats.MITE_CYCLES_OK;
        activity = &idqStats.activity[0];
    } else if (source == FrontendSource::Dsb) {
        total = &idqStats.DSB_UOPS;
        any = &idqStats.DSB_CYCLES_ANY;
        ok = &idqStats.DSB_CYCLES_OK;
        activity = &idqStats.activity[1];
    } else {
        total = &idqStats.MS_UOPS;
        any = &idqStats.MS_CYCLES_ANY;
        activity = &idqStats.activity[2];
    }
    const unsigned before = activity->add(cycle, units);
    *total += units;
    if (!before) ++*any;
    if (ok && before < 6 && activity->units >= 6) ++*ok;
}

void
CPU::recordAllocatedUops(unsigned count, const DynInstPtr &inst)
{
    if (!count) return;
    recoveryStats.allocated(inst);
    const ThreadID tid = inst->threadNumber;
    uopsIssuedStats.ANY += count;
    if (!uopsIssuedStats.activity.at(tid).add(uint64_t(curCycle()), count))
        ++uopsIssuedStats.CYCLES;
}

void
CPU::recordExecutionAction(const DynInstPtr &inst, int port)
{
    const ThreadID tid = inst->threadNumber;
    assert(tid < numThreads);
    executionActivityStats.action(tid);
    ++uopsExecutedStats.THREAD;
    const auto &architecture = inst->macroop ? inst->macroop : inst->staticInst;
    if (!inst->notAnInst() && architecture->isArchitecturalX87())
        ++uopsExecutedStats.X87;
    if (inst->isDataPrefetch()) {
        switch (architecture->softwarePrefetchKind()) {
          case 1: ++softwarePrefetchStats.NTA; break;
          case 2: ++softwarePrefetchStats.T0; break;
          case 3: ++softwarePrefetchStats.T1_T2; break;
          case 4: ++softwarePrefetchStats.PREFETCHW; break;
          default: break;
        }
        if (architecture->softwarePrefetchKind()) ++softwarePrefetchStats.ANY;
    }
    auto &activity = uopsExecutedStats.activity.at(tid);
    const unsigned before = activity.add(uint64_t(curCycle()), 1);
    if (before < 1 && activity.units >= 1) ++uopsExecutedStats.CYCLES_GE_1;
    if (before < 2 && activity.units >= 2) ++uopsExecutedStats.CYCLES_GE_2;
    if (before < 3 && activity.units >= 3) ++uopsExecutedStats.CYCLES_GE_3;
    if (before < 4 && activity.units >= 4) ++uopsExecutedStats.CYCLES_GE_4;
    switch (port) {
      case -1: ++uopsExecutedStats.unmappedPortActions; break;
      case 0: ++uopsDispatchedStats.PORT_0; break;
      case 1: ++uopsDispatchedStats.PORT_1; break;
      case 2: case 3: case 10: ++uopsDispatchedStats.PORT_2_3_10; break;
      case 4: case 9: ++uopsDispatchedStats.PORT_4_9; break;
      case 5: case 11: ++uopsDispatchedStats.PORT_5_11; break;
      case 6: ++uopsDispatchedStats.PORT_6; break;
      case 7: case 8: ++uopsDispatchedStats.PORT_7_8; break;
      default: panic("Invalid SPR execution port %d", port);
    }
}

void
CPU::recordRetiredUops(const DynInstPtr &inst)
{
    const ThreadID tid = inst->threadNumber;
    assert(tid < numThreads);
    if (inst->notAnInst()) return;
    auto &pending = pendingRetirement.at(tid);
    pending.key = inst->retirementKey();
    ++pending.members;
    pending.architecturalUops += inst->stageDemand.retirementUops;
    const auto &architecture = inst->macroop ? inst->macroop : inst->staticInst;
    if (architecture->usesMicrocodeService())
        pending.microcodeUops += inst->stageDemand.retirementUops;
    if (inst->staticInst->getName().rfind("repprogress", 0) == 0) {
        for (int i = 0; i < inst->numSrcRegs(); ++i) {
            if (inst->srcRegIdx(i).is(IntRegClass) &&
                inst->srcRegIdx(i).index() == X86ISA::intRegMicro(3)) {
                ++instRetiredStats.repProgressUpdates;
                instRetiredStats.repProgressElements +=
                    inst->getRegOperand(inst->staticInst.get(), i);
                break;
            }
        }
    }
    pending.dsbMiss |= inst->frontendDsbMiss;
    if (inst->frontendDsbMissExposed)
        pending.dsbMissExposed = inst->frontendDsbMissExposed;
    if (inst->retirementGroupEnd()) {
        ++uopsRetiredStats.completedGroups;
        const unsigned slots = inst->retirementSlots();
        uopsRetiredStats.SLOTS += slots;
        if (slots && !uopsRetiredStats.activity.at(tid).add(
                uint64_t(curCycle()), slots))
            ++uopsRetiredStats.CYCLES;
        pending.key = 0;
        pending.members = 0;
    }
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        if (pending.dsbMiss) ++frontendRetiredStats.ANY_DSB_MISS;
        if (pending.dsbMissExposed && *pending.dsbMissExposed)
            ++frontendRetiredStats.DSB_MISS;
        pending.dsbMiss = false;
        pending.dsbMissExposed.reset();
        const unsigned fp = architecture->sseArithmeticKind();
        if (fp) {
            switch (fp) {
              case 1: ++fpArithmeticStats.SCALAR_DOUBLE; break;
              case 2: ++fpArithmeticStats.SCALAR_SINGLE; break;
              case 3: ++fpArithmeticStats.PACKED_128B_DOUBLE; break;
              case 4: ++fpArithmeticStats.PACKED_128B_SINGLE; break;
            }
            const uint64_t mxcsr = tcBase(tid)->readMiscRegNoEffect(X86ISA::misc_reg::Mxcsr);
            if ((mxcsr & 0x8040) == 0x8040) ++fpArithmeticStats.mxcsrDazFtz;
            else ++fpArithmeticStats.mxcsrOther;
        }
        if (pending.architecturalUops > 1)
            uopsRetiredStats.HEAVY += pending.architecturalUops - 1;
        uopsRetiredStats.MS += pending.microcodeUops;
        pending.architecturalUops = pending.microcodeUops = 0;
    }
}

void
CPU::discardRetirement(ThreadID tid, bool fault)
{
    auto &pending = pendingRetirement.at(tid);
    if (pending.members) {
        ++uopsRetiredStats.interruptedGroups;
        if (fault) ++uopsRetiredStats.faultInterruptedGroups;
    }
    if (pending.architecturalUops)
        ++uopsRetiredStats.interruptedInstructions;
    pending = {};
}

CPU::BranchMispStats::BranchMispStats(CPU *cpu)
    : statistics::Group(cpu, "BR_MISP_RETIRED"),
      ADD_STAT(ALL_BRANCHES, statistics::units::Count::get(), "Retired branches with execution-stage address mismatch"),
      ADD_STAT(COND, statistics::units::Count::get(), "Retired conditional branches with execution-stage address mismatch"),
      ADD_STAT(INDIRECT, statistics::units::Count::get(), "Retired near indirect mismatches excluding TSX"),
      ADD_STAT(RET, statistics::units::Count::get(), "Retired return mismatches detected at execution"),
      ADD_STAT(COND_TAKEN, statistics::units::Count::get(), "Retired taken conditional execution-stage mismatches"),
      ADD_STAT(COND_NTAKEN, statistics::units::Count::get(), "Retired non-taken conditional execution-stage mismatches"),
      ADD_STAT(INDIRECT_CALL, statistics::units::Count::get(), "Retired near indirect CALL execution-stage mismatches"),
      ADD_STAT(NEAR_TAKEN, statistics::units::Count::get(), "Retired taken near branch execution-stage mismatches")
{}

void
CPU::recordBranchRetirement(const DynInstPtr &inst)
{
    if (inst->notAnInst()) return;
    auto &pending = pendingBranchRetirement.at(inst->threadNumber);
    if (inst->executionBranchResolved) {
        pending.resolved = true;
        pending.taken = inst->executionBranchTaken;
        pending.mispredicted |= inst->executionBranchMispredicted;
    }
    if (inst->isMicroop() && !inst->isLastMicroop()) return;
    const auto facts = pending;
    pending = {};
    const auto &group = inst->allocationGroup;
    if (group && group->branchProducer && group->branch == inst->seqNum)
        ++instRetiredStats.MACRO_FUSED;
    const auto &op = inst->macroop ? inst->macroop : inst->staticInst;
    if (!op->isArchitecturalBranch()) return;
    ++branchInstStats.ALL_BRANCHES;
    const bool conditional = op->isArchitecturalConditionalBranch();
    const bool far = op->isArchitecturalFarBranch();
    const bool call = op->isArchitecturalNearCall();
    const bool indirect = op->isArchitecturalNearIndirectBranch();
    const bool near_return = op->isArchitecturalNearReturn();
    if (conditional) {
        ++branchInstStats.COND;
        if (facts.resolved) {
            if (facts.taken) ++branchInstStats.COND_TAKEN;
            else ++branchInstStats.COND_NTAKEN;
        }
    }
    if (!facts.resolved) ++branchInstStats.unresolvedDirection;
    if (call) ++branchInstStats.NEAR_CALL;
    if (far) ++branchInstStats.FAR_BRANCH;
    else if (facts.resolved && facts.taken) ++branchInstStats.NEAR_TAKEN;
    if (indirect) ++branchInstStats.INDIRECT;
    if (near_return) ++branchInstStats.NEAR_RETURN;
    if (!facts.mispredicted) return;
    ++branchMispStats.ALL_BRANCHES;
    if (conditional) {
        ++branchMispStats.COND;
        if (facts.taken) ++branchMispStats.COND_TAKEN;
        else ++branchMispStats.COND_NTAKEN;
    }
    if (!far && facts.taken) ++branchMispStats.NEAR_TAKEN;
    if (call && indirect) ++branchMispStats.INDIRECT_CALL;
    // TODO: Intel's INDIRECT event also covers TSX aborts; the current model
    // reports only ordinary near-indirect branch mismatches.
    if (indirect) ++branchMispStats.INDIRECT;
    if (near_return) ++branchMispStats.RET;
}

CPU::UnhaltedClockStats::UnhaltedClockStats(CPU *cpu)
    : statistics::Group(cpu, "CPU_CLK_UNHALTED"), owner(cpu),
      ADD_STAT(THREAD, statistics::units::Cycle::get(),
               "Cycles while an O3 thread is active, including unscheduled cycles"),
      ADD_STAT(REF_TSC, statistics::units::Cycle::get(),
               "Active O3 thread reference-clock cycles, where the ISA exposes a reference clock")
{}

void
CPU::UnhaltedClockStats::account()
{
    const uint64_t now = uint64_t(owner->curCycle());
    const Tick tick = curTick();
    for (ThreadID tid = 0; tid < owner->numThreads; ++tid) {
        if (running[tid]) {
            const uint64_t elapsed = now - lastCycle[tid];
            THREAD += elapsed;
            uint64_t begin_ref, end_ref;
            if (owner->isa[tid]->referenceClockCycles(lastTick[tid], begin_ref) &&
                owner->isa[tid]->referenceClockCycles(tick, end_ref))
                REF_TSC += end_ref - begin_ref;
        }
        lastCycle[tid] = now;
        lastTick[tid] = tick;
    }
}

void
CPU::UnhaltedClockStats::setActive(ThreadID tid, bool value)
{
    account();
    owner->executionActivityStats.account();
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
               "Window-allocated slots confirmed retired before the snapshot"),
      ADD_STAT(badSpeculation, statistics::units::Count::get(),
               "Discarded speculative slots and recovery bubbles"),
      ADD_STAT(frontendBound, statistics::units::Count::get(),
               "Unfilled slots with no determined backend block or speculation recovery"),
      ADD_STAT(backendBound, statistics::units::Count::get(),
               "Unfilled slots blocked by backend resources"),
      ADD_STAT(pending, statistics::units::Count::get(),
               "Window allocations still in flight at the snapshot"),
      ADD_STAT(otherSquashes, statistics::units::Count::get(),
               "Window allocations discarded for a non-speculation reason"),
      ADD_STAT(unattributed, statistics::units::Count::get(),
               "Active-window slots in cycles without a Rename observation"),
      ADD_STAT(carryInRetired, statistics::units::Count::get(),
               "Pre-window allocation units retired within the window"),
      ADD_STAT(frontendAmbiguous, statistics::units::Count::get(),
               "Frontend-classified empty slots without a candidate to assess"),
      ADD_STAT(frontendBackendOverlap, statistics::units::Count::get(),
               "Backend-classified empty slots with no frontend candidate"),
      ADD_STAT(allocatedOps, statistics::units::Count::get(),
               "Functional operations accepted in the measured window"),
      ADD_STAT(fusedPairs, statistics::units::Count::get(),
               "Flag producer/Jcc groups formed by the frontend and allocated"),
      ADD_STAT(branchSquashedAllocations, statistics::units::Count::get(),
               "Window allocation units squashed by branch recovery; excludes recovery bubbles"),
      ADD_STAT(branchRecoveryEmpty, statistics::units::Count::get(),
               "Recovery empty units with an accepted architectural branch cause"),
      ADD_STAT(frontendOnlyEmpty, statistics::units::Count::get(),
               "Empty allocation units when no candidate and no backend block were observed"),
      ADD_STAT(backendOnlyEmpty, statistics::units::Count::get(),
               "Empty allocation units with a backend block and an available candidate"),
      ADD_STAT(overlapEmpty, statistics::units::Count::get(),
               "Empty allocation units with both no candidate and a backend block"),
      ADD_STAT(recoveryEmpty, statistics::units::Count::get(),
               "Empty allocation units during an accepted clear/resteer episode"),
      ADD_STAT(unknownEmpty, statistics::units::Count::get(),
               "Observed empty allocation units with no exclusive cause")
{}

void
CPU::TmaStats::emptySlots(uint64_t count, EmptyReason reason)
{
    slots += count;
    switch (reason) {
      case EmptyReason::Frontend: frontendBound += count; break;
      case EmptyReason::Backend: backendBound += count; break;
      case EmptyReason::Recovery: badSpeculation += count; break;
      case EmptyReason::Unattributed: unattributed += count; break;
    }
}

void
CPU::TmaStats::accountUntil(uint64_t now)
{
    if (tracking && active && now > lastCycle) {
        const uint64_t missing = (now - lastCycle) * width;
        const bool recovering = owner->recoveryStats.pending(0);
        emptySlots(missing, recovering ?
            EmptyReason::Recovery : EmptyReason::Unattributed);
        if (recovering) {
            recoveryEmpty += missing;
            if (owner->recoveryStats.branchRecovery(0))
                branchRecoveryEmpty += missing;
        }
    }
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
    lastCycle = uint64_t(owner->curCycle());
    tracking = true;
}

void
CPU::TmaStats::end()
{
    accountUntil(uint64_t(owner->curCycle()));
    pending = outstandingUnits();
    tracking = false;
}

uint64_t
CPU::TmaStats::outstandingUnits() const
{
    uint64_t units = 0;
    for (const auto &[sequence, allocation] : outstanding)
        units += allocation.units;
    return units;
}

void
CPU::TmaStats::allocated(const DynInstPtr &inst)
{
    if (!tracking || !active) return;
    ++allocatedOps;
    const unsigned units = inst->allocationSlots();
    if (!units) return;
    if (inst->allocationGroup && inst->allocationGroup->branch) ++fusedPairs;
    accepted += units;
    slots += units;
    const auto inserted = outstanding.emplace(inst->allocationKey(),
        Allocation{inst->allocationGroup, units});
    panic_if(!inserted.second, "TMA allocation group charged twice");
}

void
CPU::TmaStats::onRetire(const DynInstPtr &inst)
{
    if (!tracking) return;
    const auto group = inst->allocationGroup;
    if (!group || group->end == inst->seqNum) {
        const auto found = outstanding.find(inst->allocationKey());
        if (found != outstanding.end()) {
            retiring += found->second.units;
            outstanding.erase(found);
        } else {
            // Formed fusion groups have one allocation unit. Standalone
            // operations can have several, while helpers can have none.
            carryInRetired += group ? 1 : inst->allocationSlots();
        }
    }
}

void
CPU::TmaStats::onOtherSquash(const DynInstPtr &inst)
{
    if (!tracking) return;
    const auto found = outstanding.find(inst->allocationKey());
    if (found != outstanding.end()) {
        otherSquashes += found->second.units;
        outstanding.erase(found);
    }
}

void
CPU::TmaStats::squashAfter(InstSeqNum sequence, bool speculation, bool branch)
{
    if (!tracking) return;
    auto it = outstanding.upper_bound(sequence);
    if (it != outstanding.begin()) {
        auto preceding = std::prev(it);
        if (preceding->second.group &&
            preceding->second.group->end > sequence) it = preceding;
    }
    while (it != outstanding.end()) {
        if ((it->second.group ? it->second.group->end : it->first) <= sequence) {
            ++it;
            continue;
        }
        if (speculation) {
            badSpeculation += it->second.units;
            if (branch) branchSquashedAllocations += it->second.units;
        } else {
            otherSquashes += it->second.units;
        }
        it = outstanding.erase(it);
    }
}

void
CPU::TmaStats::renameCycle(bool blocked, bool squashing, bool no_candidate)
{
    if (!tracking || !active) return;
    const uint64_t now = uint64_t(owner->curCycle());
    if (now > lastCycle + 1) accountUntil(now - 1);
    lastCycle = now;
    panic_if(accepted > width, "TMA accepted more than the allocation width");
    EmptyReason reason = EmptyReason::Frontend;
    if (owner->recoveryStats.pending(0) && (squashing || accepted == 0))
        reason = EmptyReason::Recovery;
    else if (blocked)
        reason = EmptyReason::Backend;
    const unsigned missing = width - accepted;
    emptySlots(missing, reason);
    // These are separate model observations, not Intel Top-down categories.
    // In particular, do not force a cause for front/back overlap.
    if (reason == EmptyReason::Recovery) {
        recoveryEmpty += missing;
        if (owner->recoveryStats.branchRecovery(0))
            branchRecoveryEmpty += missing;
    }
    else if (no_candidate && blocked) overlapEmpty += missing;
    else if (no_candidate) frontendOnlyEmpty += missing;
    else if (blocked) backendOnlyEmpty += missing;
    else unknownEmpty += missing;
    if (no_candidate && reason == EmptyReason::Frontend)
        frontendAmbiguous += width - accepted;
    if (no_candidate && reason == EmptyReason::Backend)
        frontendBackendOverlap += width - accepted;
    accepted = 0;
}

void
CPU::TmaStats::preDumpStats()
{
    accountUntil(uint64_t(owner->curCycle()));
    if (tracking) pending = outstandingUnits();
    statistics::Group::preDumpStats();
}

void
CPU::TmaStats::resetStats()
{
    accountUntil(uint64_t(owner->curCycle()));
    outstanding.clear();
    accepted = 0;
    statistics::Group::resetStats();
}

} // namespace gem5::o3
