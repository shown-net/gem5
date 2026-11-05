// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/cpu.hh"

#include "arch/generic/isa.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/probe/perf_retirement.hh"
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
      ADD_STAT(ALL_STORES, statistics::units::Count::get(), "Retired architectural instructions containing a memory store; port IO and prefetch excluded"),
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
      ADD_STAT(MACRO_FUSED, statistics::units::Count::get(),
               "Retired flag-producer/branch allocation groups, counted at the architectural branch")
{}

CPU::BranchInstStats::BranchInstStats(CPU *cpu)
    : statistics::Group(cpu, "BR_INST_RETIRED"),
      ADD_STAT(ALL_BRANCHES, statistics::units::Count::get(), "Retired architectural branches"),
      ADD_STAT(COND, statistics::units::Count::get(), "Retired conditional branches"),
      ADD_STAT(INDIRECT, statistics::units::Count::get(), "Retired near indirect branches excluding TSX"),
      ADD_STAT(NEAR_RETURN, statistics::units::Count::get(), "Retired near returns")
{}

CPU::UopsRetiredStats::UopsRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_RETIRED"), owner(cpu),
      ADD_STAT(SLOTS, statistics::units::Count::get(),
               "Retirement-domain slots, counted when the retirement group's final record commits"),
      ADD_STAT(CYCLES, statistics::units::Cycle::get(),
               "Cycles with a retirement-domain slot committed"),
      ADD_STAT(STALLS, statistics::units::Cycle::get(),
               "Candidate active-thread cycles without a retired slot; single-thread scope and snapshot boundaries apply")
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
      ADD_STAT(MS_CYCLES_ANY, statistics::units::Cycle::get(), "Cycles with positive microcode delivery")
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
               "Candidate active-thread cycles without an accepted execution action; single-thread scope and snapshot boundaries apply")
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

void
CPU::recordFrontendDelivery(FrontendSource source, unsigned units, uint64_t cycle)
{
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
    const ThreadID tid = inst->threadNumber;
    uopsIssuedStats.ANY += count;
    if (!uopsIssuedStats.activity.at(tid).add(uint64_t(curCycle()), count))
        ++uopsIssuedStats.CYCLES;
}

void
CPU::recordExecutedUops(unsigned count, ThreadID tid)
{
    assert(tid < numThreads);
    if (!count) return;
    uopsExecutedStats.THREAD += count;
    auto &activity = uopsExecutedStats.activity.at(tid);
    const unsigned before = activity.add(uint64_t(curCycle()), count);
    if (before < 1 && activity.units >= 1) ++uopsExecutedStats.CYCLES_GE_1;
    if (before < 2 && activity.units >= 2) ++uopsExecutedStats.CYCLES_GE_2;
    if (before < 3 && activity.units >= 3) ++uopsExecutedStats.CYCLES_GE_3;
    if (before < 4 && activity.units >= 4) ++uopsExecutedStats.CYCLES_GE_4;
}

void
CPU::recordRetiredUops(unsigned count, ThreadID tid)
{
    assert(tid < numThreads);
    if (!count) return;
    uopsRetiredStats.SLOTS += count;
    if (!uopsRetiredStats.activity.at(tid).add(uint64_t(curCycle()), count))
        ++uopsRetiredStats.CYCLES;
}

void
CPU::recordDispatchedPort(unsigned port, unsigned count)
{
    switch (port) {
      case 0: uopsDispatchedStats.PORT_0 += count; break;
      case 1: uopsDispatchedStats.PORT_1 += count; break;
      case 2: case 3: case 10: uopsDispatchedStats.PORT_2_3_10 += count; break;
      case 4: case 9: uopsDispatchedStats.PORT_4_9 += count; break;
      case 5: case 11: uopsDispatchedStats.PORT_5_11 += count; break;
      case 6: uopsDispatchedStats.PORT_6 += count; break;
      case 7: case 8: uopsDispatchedStats.PORT_7_8 += count; break;
      default: panic("Invalid SPR execution port %u", port);
    }
}

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
    const auto &group = inst->allocationGroup;
    if (group && group->branchProducer && group->branch == inst->seqNum)
        ++instRetiredStats.MACRO_FUSED;
    if (op->isArchitecturalConditionalBranch()) ++branchInstStats.COND;
    if (op->isNearIndirectBranch()) ++branchInstStats.INDIRECT;
    if (op->isRasReturn()) ++branchInstStats.NEAR_RETURN;
    if (!inst->executionBranchMispredicted) return;
    ++branchMispStats.ALL_BRANCHES;
    if (op->isArchitecturalConditionalBranch()) ++branchMispStats.COND;
    // TODO: Intel's INDIRECT event also covers TSX aborts; the current model
    // reports only ordinary near-indirect branch mismatches.
    if (op->isNearIndirectBranch()) ++branchMispStats.INDIRECT;
    if (op->isReturn()) ++branchMispStats.RET;
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
      ADD_STAT(frontendOnlyEmpty, statistics::units::Count::get(),
               "Empty allocation units when no candidate and no backend block were observed"),
      ADD_STAT(backendOnlyEmpty, statistics::units::Count::get(),
               "Empty allocation units with a backend block and an available candidate"),
      ADD_STAT(overlapEmpty, statistics::units::Count::get(),
               "Empty allocation units with both no candidate and a backend block"),
      ADD_STAT(recoveryEmpty, statistics::units::Count::get(),
               "Empty allocation units during branch recovery"),
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
    if (tracking && active && now > lastCycle)
        emptySlots((now - lastCycle) * width, EmptyReason::Unattributed);
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
    outstanding.emplace(inst->seqNum, inst->allocationGroup);
}

void
CPU::TmaStats::onRetire(const DynInstPtr &inst)
{
    if (!tracking) return;
    const auto group = inst->allocationGroup;
    if (!group || group->end == inst->seqNum) {
        if (outstanding.erase(group ? group->leader : inst->seqNum))
            ++retiring;
        else
            ++carryInRetired;
    }
}

void
CPU::TmaStats::onOtherSquash(const DynInstPtr &inst)
{
    if (tracking && outstanding.erase(inst->allocationGroup ?
        inst->allocationGroup->leader : inst->seqNum)) ++otherSquashes;
}

void
CPU::TmaStats::squashAfter(InstSeqNum sequence, bool speculation, bool branch)
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
            ++badSpeculation;
            if (branch) ++branchSquashedAllocations;
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
CPU::TmaStats::renameCycle(bool blocked, bool squashing, bool no_candidate)
{
    if (!tracking || !active) return;
    const uint64_t now = uint64_t(owner->curCycle());
    if (now > lastCycle + 1) accountUntil(now - 1);
    lastCycle = now;
    panic_if(accepted > width, "TMA accepted more than the allocation width");
    if (recovery && squashing) sawRecoverySquash = true;
    EmptyReason reason = EmptyReason::Frontend;
    const bool recovery_ended = recovery && sawRecoverySquash &&
                                !squashing && accepted;
    if (recovery_ended) {
        recovery = false;
        sawRecoverySquash = false;
    }
    if (recovery && (squashing || accepted == 0))
        reason = EmptyReason::Recovery;
    else if (blocked)
        reason = EmptyReason::Backend;
    const unsigned missing = width - accepted;
    emptySlots(missing, reason);
    // These are separate model observations, not Intel Top-down categories.
    // In particular, do not force a cause for front/back overlap.
    if (reason == EmptyReason::Recovery) recoveryEmpty += missing;
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
    statistics::Group::resetStats();
}

} // namespace gem5::o3
