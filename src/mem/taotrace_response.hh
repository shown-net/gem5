/*
 * Oracle-only Ruby response facts carried on a Request. These fields never
 * enter the functional trace and do not affect cache behavior.
 */

#ifndef __MEM_TAOTRACE_RESPONSE_HH__
#define __MEM_TAOTRACE_RESPONSE_HH__

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/extensible.hh"
#include "mem/request.hh"
#include "mem/spr_retired_load_miss.hh"
#include "mem/spr_prepare_service.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

enum class TaoTraceNativeTerminalReason : uint32_t
{
    None = 0,
    PredicatedOff = 1u << 0,
    NoRequest = 1u << 1,
    StoreForward = 1u << 2,
    LocalAccess = 1u << 3,
    FailedStoreConditional = 1u << 4,
    ZeroSize = 1u << 5,
    PrefetchSkipped = 1u << 6,
    StoreOverwriteElided = 1u << 7,
    StoreBufferCombined = 1u << 8,
    DeviceBypassComplete = 1u << 9,
};

enum class TaoTraceNativeCacheOutcome : uint32_t
{
    Hit = 0,
    TagMiss = 1,
    PermissionUpgrade = 2,
    MergedMiss = 3,
    RemoteSupply = 4,
};

// Why a late-prefetch parent witness was rejected. Bits are independent of
// the request diagnostic outcomes and never classify by themselves.
enum class TaoTraceParentReject : uint8_t
{
    None = 0,
    Uniqueness = 1u << 0, // conflicting identity/edge or follower storage overflow
    StaleInvalidated = 1u << 1,
};

// The origin of the data in this particular fill, not the last network
// sender and not an extra lookup attributed to a merged load.
enum class TaoTraceServiceSource : uint32_t
{
    Unknown = 0, L2 = 1, LLC = 2, Memory = 3, RemoteOwner = 4,
};

// A hierarchy outcome for the fill transaction, kept distinct from the
// eventual data supplier. Zero is explicitly unknown when recorded.
enum class TaoTraceServiceOutcome : uint32_t
{
    Unknown = 0, Hit = 1, Miss = 2, NotAccessed = 3,
};

// One fact per completed physical fragment.  Preserve the tuple across split
// aggregation; independent masks would combine outcomes from different lines.
struct TaoTraceServiceFact
{
    uint8_t source = 0;
    uint8_t l2Outcome = 0;
    uint8_t l3Outcome = 0;
    bool follower = false;
    // This fragment's own lookups, independent of a shared fill's path.
    // Bits: 0=unknown, 1=hit, 2=miss, 3=not accessed.
    uint8_t ownL2Mask = 0;
    uint8_t ownL3Mask = 0;
};

enum TaoTraceSharedParent : uint16_t
{
    SharedParentL1Hit = 1u << 0, SharedParentL1Miss = 1u << 1,
    SharedParentL1Other = 1u << 2, SharedParentL2Hit = 1u << 3,
    SharedParentL2Miss = 1u << 4, SharedParentL2Other = 1u << 5,
    SharedParentL3Hit = 1u << 6, SharedParentL3Miss = 1u << 7,
    SharedParentL3Other = 1u << 8, SharedParentIFetch = 1u << 9,
    SharedParentMissing = 1u << 10, SharedParentWriteCallback = 1u << 11
};

struct TaoTraceNativeHierarchyFacts
{
    static constexpr unsigned Levels = 3;
    std::array<uint32_t, Levels> accesses{};
    std::array<uint32_t, Levels> hits{};
    std::array<uint32_t, Levels> tagMisses{};
    std::array<uint32_t, Levels> permissionUpgrades{};
    std::array<uint32_t, Levels> mergedMisses{};
    std::array<uint32_t, Levels> remoteSupplies{};
    // Oracle-only evidence: demand encountered an in-flight prefetch.
    std::array<uint32_t, Levels> latePrefetches{};
    // Diagnostic witness of the explicitly linked private-L2 prefetch's LLC
    // lookup, indexed by TaoTraceNativeCacheOutcome. Not demand classification.
    std::array<uint32_t, 5> l2PrefetchParentLlc{};
    // Immediate message sender and response type; not ultimate cache/DRAM tier.
    std::array<uint32_t, 3> l2PrefetchParentSender{};
    std::array<uint32_t, 3> l2PrefetchParentResponse{};
    // Parallel observer chain begun at the L1D (L0 controller) prefetcher:
    // its carried generation is forwarded by the private L2 (L1 controller)
    // and looked up at the LLC.  Same witness semantics as the L2 chain but
    // an independent population; never a retired PMU value.
    std::array<uint32_t, 5> l1PrefetchParentLlc{};
    std::array<uint32_t, 3> l1PrefetchParentSender{};
    std::array<uint32_t, 3> l1PrefetchParentResponse{};
    uint32_t uniqueFills = 0;
    uint32_t rubyMemoryFetches = 0;
    uint32_t memoryReadTransactions = 0;

    void
    observe(unsigned level, TaoTraceNativeCacheOutcome outcome)
    {
        if (level >= Levels) return;
        ++accesses[level];
        switch (outcome) {
          case TaoTraceNativeCacheOutcome::Hit: ++hits[level]; break;
          case TaoTraceNativeCacheOutcome::TagMiss: ++tagMisses[level]; break;
          case TaoTraceNativeCacheOutcome::PermissionUpgrade:
            ++permissionUpgrades[level];
            break;
          case TaoTraceNativeCacheOutcome::MergedMiss:
            ++mergedMisses[level];
            break;
          case TaoTraceNativeCacheOutcome::RemoteSupply:
            ++remoteSupplies[level];
            break;
        }
    }

    void
    merge(const TaoTraceNativeHierarchyFacts &other)
    {
        for (unsigned level = 0; level < Levels; ++level) {
            accesses[level] += other.accesses[level];
            hits[level] += other.hits[level];
            tagMisses[level] += other.tagMisses[level];
            permissionUpgrades[level] += other.permissionUpgrades[level];
            mergedMisses[level] += other.mergedMisses[level];
            remoteSupplies[level] += other.remoteSupplies[level];
            latePrefetches[level] += other.latePrefetches[level];
        }
        for (unsigned i = 0; i < l2PrefetchParentLlc.size(); ++i)
            l2PrefetchParentLlc[i] += other.l2PrefetchParentLlc[i];
        for (unsigned i = 0; i < l2PrefetchParentSender.size(); ++i) {
            l2PrefetchParentSender[i] += other.l2PrefetchParentSender[i];
            l2PrefetchParentResponse[i] += other.l2PrefetchParentResponse[i];
        }
        for (unsigned i = 0; i < l1PrefetchParentLlc.size(); ++i)
            l1PrefetchParentLlc[i] += other.l1PrefetchParentLlc[i];
        for (unsigned i = 0; i < l1PrefetchParentSender.size(); ++i) {
            l1PrefetchParentSender[i] += other.l1PrefetchParentSender[i];
            l1PrefetchParentResponse[i] += other.l1PrefetchParentResponse[i];
        }
        uniqueFills += other.uniqueFills;
        rubyMemoryFetches += other.rubyMemoryFetches;
        memoryReadTransactions += other.memoryReadTransactions;
    }
};

inline constexpr uint32_t
taoTraceNativeReasonMask(TaoTraceNativeTerminalReason reason)
{
    return static_cast<uint32_t>(reason);
}

// Device service is independent of Ruby. A fragment response cannot close the
// main request; an empty closed request is not evidence of any service.
template <class Facts>
inline bool taoTraceNativeLifecycleResolved(const Facts &facts)
{
    if (facts.bypassProtocolErrors || facts.responses != facts.admissions ||
        facts.bypassResponses != facts.bypassAccepted) return false;
    const bool has_service = facts.admissions || facts.bypassAccepted;
    return has_service ? facts.issuanceClosed : facts.terminalNoRuby;
}

template <class Facts>
inline void taoTraceNativeRefreshBypassTerminal(Facts &facts)
{
    const uint32_t reason = taoTraceNativeReasonMask(
        TaoTraceNativeTerminalReason::DeviceBypassComplete);
    facts.terminalReasonMask &= ~reason;
    if (facts.issuanceClosed && !facts.admissions && facts.bypassAccepted &&
        facts.bypassResponses == facts.bypassAccepted &&
        !facts.bypassProtocolErrors) facts.terminalReasonMask |= reason;
    facts.terminalNoRuby = facts.terminalReasonMask != 0 &&
        !(facts.admissions && facts.bypassAccepted);
}

class TaoTraceRubyResponseExtension
    : public Extension<Request, TaoTraceRubyResponseExtension>
{
  public:
    uint32_t bypassAccepted = 0;
    uint32_t bypassResponses = 0;
    uint32_t bypassProtocolErrors = 0;
    uint32_t admissions = 0;
    uint32_t aliasedAdmissions = 0;
    uint32_t hierarchyRequests = 0;
    uint32_t responses = 0;
    uint32_t externalHits = 0;
    uint32_t externalMisses = 0;
    uint32_t coalesced = 0;
    // Responses supplied by the Sequencer without a request-local L1 lookup
    // or an admission-time L1/fill observation.
    uint32_t sequencerShared = 0;
    uint16_t sharedParentSources = 0;
    // Empty means no service observation. Fields with value zero record an
    // observed but unknown supplier or hierarchy outcome.
    std::vector<TaoTraceServiceFact> serviceFacts;
    uint64_t responderMachineMask = 0;
    uint32_t responderMachineUnknown = 0;
    bool hasAdmissionTick = false;
    uint64_t firstAdmissionTick = 0;
    uint64_t lastAdmissionTick = 0;
    bool hasResponseTick = false;
    uint64_t lastResponseTick = 0;
    bool issuanceClosed = false;
    bool terminalNoRuby = false;
    uint32_t terminalReasonMask = 0;
    // Demand received Data_Stale service (data not for storage). Independent
    // attribute: it does not invalidate the v2 lookup membership.
    uint32_t staleServiced = 0;
    spr_retired::Aggregate sprMiss;
    spr_retired::Result sprAlias;
    std::shared_ptr<spr_retired::PrepareService> sprPrepareService;
    bool sprPrepareAdmissionValid = false;
    bool sprPrepareAdmissionNeedsFill = false;
    bool sprAliasObserved = false;
    unsigned sprAliasUnclassifiedCategory = 255;
    bool sprCompletionSeen = false;
    uint64_t sprLastCompletion = 0;
    TaoTraceNativeHierarchyFacts hierarchy;
    // Bound only at the physical device-route acceptance; clones retain the
    // identity. Aggregate split ledgers merge only equal identities.
    bool bypassIdentityKnown = false;
    uint32_t bypassContext = 0;
    uint64_t bypassSequence = 0;

    bool resolved() const { return taoTraceNativeLifecycleResolved(*this); }
    bool hasL1MissTick = false;
    Tick firstL1MissTick = 0;
    // The same Request can be retried. These snapshots identify observations
    // made after the current admission instead of reusing an earlier attempt.
    std::array<uint32_t, TaoTraceNativeHierarchyFacts::Levels> admissionAccesses{};
    std::array<uint32_t, TaoTraceNativeHierarchyFacts::Levels> admissionHits{};
    std::array<uint32_t, TaoTraceNativeHierarchyFacts::Levels> admissionMisses{};
    uint32_t admissionL1Merges = 0;
    uint32_t admissionL1Upgrades = 0;
    uint32_t admissionL1Remote = 0;
    // Final local L1 observation for this physical request. Bits are
    // unknown, hit, tag miss, fill merge, upgrade, remote supply.
    uint8_t l1FinalMask = 0;
    uint32_t serviceExpected = 0;
    bool l1ObservationMissing = false;
    bool serviceFactIdentityAmbiguous = false;

    bool l1ObservationPending() const
    {
        return admissions > responses &&
            hierarchy.accesses[0] <= admissionAccesses[0];
    }

    void
    admit(bool aliased)
    {
        const uint64_t tick = uint64_t(curTick());
        // Multiple concurrent admissions of one Request cannot be associated
        // with the single current-attempt snapshot without more identities.
        serviceFactIdentityAmbiguous = admissions > responses;
        l1ObservationMissing = serviceFactIdentityAmbiguous;
        l1FinalMask = 0;
        serviceExpected = 0;
        serviceFacts.clear();
        admissionAccesses = hierarchy.accesses;
        admissionHits = hierarchy.hits;
        admissionMisses = hierarchy.tagMisses;
        admissionL1Merges = hierarchy.mergedMisses[0];
        admissionL1Upgrades = hierarchy.permissionUpgrades[0];
        admissionL1Remote = hierarchy.remoteSupplies[0];
        ++admissions;
        taoTraceNativeRefreshBypassTerminal(*this);
        if (aliased) ++aliasedAdmissions;
        if (!hasAdmissionTick) {
            hasAdmissionTick = true;
            firstAdmissionTick = tick;
            lastAdmissionTick = tick;
        } else {
            firstAdmissionTick = std::min(firstAdmissionTick, tick);
            lastAdmissionTick = std::max(lastAdmissionTick, tick);
        }
    }

    void
    observe(bool external_hit, bool was_coalesced, int machine)
    {
        const uint64_t tick = uint64_t(curTick());
        const auto accesses = hierarchy.accesses[0] - admissionAccesses[0];
        const auto hits = hierarchy.hits[0] - admissionHits[0];
        const auto misses = hierarchy.tagMisses[0] - admissionMisses[0];
        const auto merges = hierarchy.mergedMisses[0] - admissionL1Merges;
        const auto upgrades = hierarchy.permissionUpgrades[0] - admissionL1Upgrades;
        const auto remote = hierarchy.remoteSupplies[0] - admissionL1Remote;
        if (l1ObservationMissing || accesses != 1 ||
            accesses != hits + misses + merges + upgrades + remote) {
            // More than one lookup may contain discarded/replayed outcomes;
            // without a final-attempt identity don't infer a positive result.
            l1FinalMask = 1u;
            l1ObservationMissing = true;
        } else {
            if (hits) l1FinalMask |= 1u << 1;
            if (misses) l1FinalMask |= 1u << 2;
            if (merges) l1FinalMask |= 1u << 3;
            if (upgrades) l1FinalMask |= 1u << 4;
            if (remote) l1FinalMask |= 1u << 5;
        }
        serviceExpected = (l1FinalMask & ((1u << 2) | (1u << 3))) ? 1 : 0;
        ++responses;
        if (external_hit) {
            ++externalMisses;
        } else {
            ++externalHits;
        }
        if (was_coalesced) ++coalesced;
        if (machine >= 0 && machine < 64) {
            responderMachineMask |= uint64_t(1) << unsigned(machine);
        } else {
            ++responderMachineUnknown;
        }
        hasResponseTick = true;
        lastResponseTick = std::max(lastResponseTick, tick);
    }

    void
    merge(const TaoTraceRubyResponseExtension &other)
    {
        sprMiss.add(other.sprMiss);
        if (other.bypassIdentityKnown) {
            if (bypassIdentityKnown &&
                (bypassContext != other.bypassContext ||
                 bypassSequence != other.bypassSequence)) ++bypassProtocolErrors;
            bypassIdentityKnown = true;
            bypassContext = other.bypassContext;
            bypassSequence = other.bypassSequence;
        }
        bypassAccepted += other.bypassAccepted;
        bypassResponses += other.bypassResponses;
        bypassProtocolErrors += other.bypassProtocolErrors;
        admissions += other.admissions;
        aliasedAdmissions += other.aliasedAdmissions;
        hierarchyRequests += other.hierarchyRequests;
        responses += other.responses;
        externalHits += other.externalHits;
        externalMisses += other.externalMisses;
        coalesced += other.coalesced;
        sequencerShared += other.sequencerShared;
        sharedParentSources |= other.sharedParentSources;
        l1ObservationMissing |= other.l1ObservationMissing ||
                                other.l1ObservationPending();
        serviceFactIdentityAmbiguous |= other.serviceFactIdentityAmbiguous;
        l1FinalMask |= other.l1FinalMask;
        serviceExpected += other.serviceExpected;
        serviceFacts.insert(serviceFacts.end(), other.serviceFacts.begin(),
                            other.serviceFacts.end());
        responderMachineMask |= other.responderMachineMask;
        responderMachineUnknown += other.responderMachineUnknown;
        if (other.hasAdmissionTick) {
            if (!hasAdmissionTick) {
                hasAdmissionTick = true;
                firstAdmissionTick = other.firstAdmissionTick;
                lastAdmissionTick = other.lastAdmissionTick;
            } else {
                firstAdmissionTick = std::min(
                    firstAdmissionTick, other.firstAdmissionTick);
                lastAdmissionTick = std::max(
                    lastAdmissionTick, other.lastAdmissionTick);
            }
        }
        if (other.hasResponseTick) {
            hasResponseTick = true;
            lastResponseTick = std::max(
                lastResponseTick, other.lastResponseTick);
        }
        issuanceClosed = issuanceClosed || other.issuanceClosed;
        terminalNoRuby = terminalNoRuby || other.terminalNoRuby;
        terminalReasonMask |= other.terminalReasonMask;
        staleServiced += other.staleServiced;
        hierarchy.merge(other.hierarchy);
        taoTraceNativeRefreshBypassTerminal(*this);
    }

    std::unique_ptr<ExtensionBase>
    clone() const override
    {
        return std::make_unique<TaoTraceRubyResponseExtension>(*this);
    }
};

/**
 * Measurement-only join table for O3 request lifecycle facts.
 *
 * Ruby sees a Request before retirement, while TaoTrace decides at commit
 * whether the UOP belongs to the measured population.  Keeping this table
 * outside the functional trace lets the two observers join by
 * (ContextID, InstSeqNum) without changing timing or FST contents.  Tracking
 * is enabled only during the measurement interval and disabled per context
 * at its exact target boundary.  Existing identities remain live after that
 * boundary so committed stores can finish draining.
 */
class TaoTraceNativeAccessRegistry
{
  public:
    struct Snapshot
    {
        spr_retired::Aggregate sprMiss;
        uint64_t sprIncompleteCompletions = 0, sprFailedRequest = 0;
        uint64_t sprFailedAddress = 0;
        unsigned sprMissingLevels = 0;
        unsigned sprFailedAliasCategory = 255;
        unsigned sprFailedPrepareStatus = 255;
        unsigned sprFailedPrepareInvalidation = 0;
        std::array<spr_retired::Level,3> sprFailedLevels{};
        struct Attempt {
            uint64_t id = 0;
            uint32_t completions = 0;
            bool coalesced = false, resident = false;
            TaoTraceNativeHierarchyFacts hierarchy;
            uint32_t parentCount[2] = {};
            int parentOutcome[2] = {-1, -1};
        };
        // Observer storage is bounded independently of workload/window size.
        std::vector<Attempt> attempts;
        bool attemptOverflow = false;
        Attempt *attempt(uint64_t id, bool create = false) {
            for (auto &a : attempts) if (id && a.id == id) return &a;
            if (!create) return nullptr;
            if (attempts.size() >= 16) { attemptOverflow = true; return nullptr; }
            attempts.emplace_back();
            attempts.back().id = id;
            return &attempts.back();
        }
        bool found = false;
        bool committed = false;
        bool squashed = false;
        uint32_t bypassAccepted = 0;
        uint32_t bypassResponses = 0;
        uint32_t bypassProtocolErrors = 0;
        uint32_t admissions = 0;
        uint32_t aliasedAdmissions = 0;
        uint32_t hierarchyRequests = 0;
        // Saturated uniqueness checks for the observer-only parent witness.
        // A context/sequence can contain several physical fragments/attempts.
        uint32_t hierarchyAttemptCount = 0;
        uint64_t hierarchyAttempt = 0;
        uint32_t hierarchyCompletionCount = 0;
        uint64_t hierarchyCompletionAttempt = 0;
        uint32_t l2ParentWitnessCount = 0;
        uint64_t l2ParentWitnessAttempt = 0;
        int l2ParentSenderId = -1;
        // Independent L1D-begun chain; its follower attempt is the demand
        // that merged into the outstanding L0 prefetch TBE.
        uint32_t l1ParentWitnessCount = 0;
        uint64_t l1ParentWitnessAttempt = 0;
        int l1ParentSenderId = -1;
        uint32_t responses = 0;
        uint32_t externalHits = 0;
        uint32_t externalMisses = 0;
        uint32_t coalesced = 0;
        uint32_t sequencerShared = 0;
        uint16_t sharedParentSources = 0;
        std::vector<TaoTraceServiceFact> serviceFacts;
        bool l1ObservationMissing = false;
        bool serviceFactIdentityAmbiguous = false;
        uint8_t l1FinalMask = 0;
        uint32_t serviceExpected = 0;
        uint64_t responderMachineMask = 0;
        uint32_t responderMachineUnknown = 0;
        bool hasAdmissionTick = false;
        uint64_t firstAdmissionTick = 0;
        uint64_t lastAdmissionTick = 0;
        bool hasResponseTick = false;
        uint64_t lastResponseTick = 0;
        bool issuanceClosed = false;
        bool terminalNoRuby = false;
        uint32_t terminalReasonMask = 0;
        uint32_t staleServiced = 0;
        // Bitmask of TaoTraceParentReject values; chain 0 = L1D-begun (G0),
        // chain 1 = private-L2-begun (G1).  Any set bit forbids the
        // see-parent estimated membership for that chain.
        uint8_t l1ParentRejectMask = 0;
        uint8_t l2ParentRejectMask = 0;
        TaoTraceNativeHierarchyFacts hierarchy;

        bool
        resolved() const
        {
            return found && taoTraceNativeLifecycleResolved(*this);
        }

        // Squash abandons the unissued tail of a split request, so normal
        // main issuance closure may never occur. This is storage reclamation
        // only, never a terminal/scored lifecycle. Keep all accepted service
        // and its already-enqueued SLICC L1 callback alive until they drain.
        bool
        abandonedServiceDrained() const
        {
            return found && squashed && !bypassProtocolErrors &&
                responses == admissions && bypassResponses == bypassAccepted &&
                hierarchy.accesses[0] == hierarchyRequests;
        }
    };

    static void
    prepareContext(uint32_t context_id)
    {
        records_.erase(context_id);
        enabled_contexts_.erase(context_id);
        pretracking_contexts_.insert(context_id);
    }

    static void
    enableContext(uint32_t context_id)
    {
        // Keep the finite set of requests still in flight or resident in the
        // ROB at the warmup/measurement boundary. Premeasurement retirement
        // removes older identities, so retaining this map does not import the
        // warmup population into the measured window.
        pretracking_contexts_.erase(context_id);
        enabled_contexts_.insert(context_id);
    }

    static void
    disableContext(uint32_t context_id)
    {
        enabled_contexts_.erase(context_id);
        pretracking_contexts_.erase(context_id);
    }

    static void enableRetirementContext(uint32_t context_id)
    {
        retirement_contexts_.insert(context_id);
    }

    static bool retirementObservationActive()
    {
        return !retirement_contexts_.empty() || !enabled_contexts_.empty() ||
               !pretracking_contexts_.empty();
    }

    static void disableRetirementContext(uint32_t context_id)
    {
        retirement_contexts_.erase(context_id);
        if (!traceOwnsContext(context_id)) records_.erase(context_id);
    }

    // Marker counters read before the commit probes. TaoTrace may retain a
    // retired store until its response, so only the standalone marker owner
    // releases identities here.
    static void releaseRetired(uint32_t context_id, uint64_t seq_num,
                               bool pending_store = false)
    {
        if (traceOwnsContext(context_id)) return;
        const auto facts = snapshot(context_id, seq_num);
        if (!facts.found) {
            // Stores may enter Ruby only after retirement. Retain that exact
            // identity so their later response can release the marker record.
            if (pending_store) noteCommitted(context_id, seq_num);
            return;
        }
        if (facts.resolved()) erase(context_id, seq_num);
        else noteCommitted(context_id, seq_num);
    }

    static void
    noteBypassAccepted(const RequestPtr &req)
    {
        uint32_t context = 0;
        uint64_t sequence = 0;
        if (!identity(req, context, sequence)) return;
        auto extension = ensureExtension(req);
        const auto observed = snapshot(context, sequence);
        const bool invalid = extension->bypassAccepted != 0 ||
            extension->bypassResponses != 0 || extension->issuanceClosed ||
            observed.issuanceClosed;
        if (invalid) ++extension->bypassProtocolErrors;
        else {
            extension->bypassIdentityKnown = true;
            extension->bypassContext = context;
            extension->bypassSequence = sequence;
            ++extension->bypassAccepted;
        }
        taoTraceNativeRefreshBypassTerminal(*extension);
        if (!shouldAdmit(context, sequence)) return;
        auto &facts = records_[context][sequence];
        facts.found = true;
        if (invalid) ++facts.bypassProtocolErrors;
        else ++facts.bypassAccepted;
        taoTraceNativeRefreshBypassTerminal(facts);
    }

    static void
    noteBypassResponse(const RequestPtr &req)
    {
        uint32_t context = 0;
        uint64_t sequence = 0;
        if (!identity(req, context, sequence)) return;
        auto extension = ensureExtension(req);
        const bool invalid = !extension->bypassIdentityKnown ||
            extension->bypassContext != context ||
            extension->bypassSequence != sequence ||
            extension->bypassAccepted != 1 || extension->bypassResponses != 0;
        if (invalid) ++extension->bypassProtocolErrors;
        else {
            ++extension->bypassResponses;
            spr_retired::Result result;
            result.service = spr_retired::Service::Device;
            result = spr_retired::convert(result);
            extension->sprMiss.add(result);
            mutate(req, [result](Snapshot &facts) { facts.sprMiss.add(result); });
        }
        taoTraceNativeRefreshBypassTerminal(*extension);
        if (!shouldTrack(context, sequence)) return;
        auto &facts = records_[context][sequence];
        facts.found = true;
        if (invalid) ++facts.bypassProtocolErrors;
        else ++facts.bypassResponses;
        taoTraceNativeRefreshBypassTerminal(facts);
        if (facts.abandonedServiceDrained() ||
            (facts.committed && retirement_contexts_.count(context) &&
             !traceOwnsContext(context) && facts.resolved())) {
            erase(context, sequence);
        }
    }

    static void
    noteAdmission(const RequestPtr &req, bool aliased)
    {
        auto extension = ensureExtension(req);
        if (extension) extension->admit(aliased);
        uint32_t context_id = 0;
        uint64_t seq_num = 0;
        if (!identity(req, context_id, seq_num) ||
            !shouldAdmit(context_id, seq_num)) {
            return;
        }
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        // A new admission after a completed service may be a replay or a
        // delayed split fragment. The instruction-scoped trace cannot tell.
        if (facts.admissions)
            facts.serviceFactIdentityAmbiguous = true;
        const uint64_t tick = uint64_t(curTick());
        ++facts.admissions;
        if (facts.admissions>1) facts.sprMiss.estimated_mask |= 7;
        taoTraceNativeRefreshBypassTerminal(facts);
        if (aliased) ++facts.aliasedAdmissions;
        if (!facts.hasAdmissionTick) {
            facts.hasAdmissionTick = true;
            facts.firstAdmissionTick = tick;
            facts.lastAdmissionTick = tick;
        } else {
            facts.firstAdmissionTick = std::min(
                facts.firstAdmissionTick, tick);
            facts.lastAdmissionTick = std::max(
                facts.lastAdmissionTick, tick);
        }
    }

    static void
    noteResponse(const RequestPtr &req, bool external_hit,
                 bool was_coalesced, int machine)
    {
        auto extension = ensureExtension(req);
        if (extension) {
            extension->observe(external_hit, was_coalesced, machine);
        }
        uint32_t context_id = 0;
        uint64_t seq_num = 0;
        if (!identity(req, context_id, seq_num) ||
            !shouldTrack(context_id, seq_num)) {
            return;
        }
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        if (extension && extension->l1ObservationMissing)
            facts.l1ObservationMissing = true;
        if (extension) {
            facts.l1FinalMask |= extension->l1FinalMask;
            facts.serviceExpected += extension->serviceExpected;
        }
        const uint64_t tick = uint64_t(curTick());
        ++facts.responses;
        if (external_hit) {
            ++facts.externalMisses;
        } else {
            ++facts.externalHits;
        }
        if (was_coalesced) ++facts.coalesced;
        if (machine >= 0 && machine < 64) {
            facts.responderMachineMask |= uint64_t(1) << unsigned(machine);
        } else {
            ++facts.responderMachineUnknown;
        }
        facts.hasResponseTick = true;
        facts.lastResponseTick = std::max(facts.lastResponseTick, tick);
        if (facts.abandonedServiceDrained() ||
            (facts.committed && retirement_contexts_.count(context_id) &&
             !traceOwnsContext(context_id) && facts.resolved())) {
            erase(context_id, seq_num);
        }
    }

    static void
    noteServiceSource(const RequestPtr &req, unsigned source,
                      unsigned l2_outcome, unsigned l3_outcome,
                      bool follower = false)
    {
        // Invalid protocol metadata is conservative rather than being
        // interpreted as a new physical supplier or cache outcome.
        auto extension = ensureExtension(req);
        if (!extension) return;
        if (!extension->serviceFacts.empty()) {
            // A second completion on the same Request cannot be attributed
            // safely to the earlier service without an attempt identity.
            // Retain one explicit unknown rather than preserving stale data
            // or manufacturing another completed fragment.  The registry is
            // instruction-scoped, so conservatively invalidate its collected
            // service facts too; its fragment identity is not available here.
            auto &previous = extension->serviceFacts.front();
            previous = TaoTraceServiceFact{0, 0, 0, true, 1, 1};
            mutate(req, [](Snapshot &facts) {
                for (auto &observed : facts.serviceFacts) {
                    observed.source = 0;
                    observed.l2Outcome = 0;
                    observed.l3Outcome = 0;
                    observed.ownL2Mask = 1;
                    observed.ownL3Mask = 1;
                }
            });
            return;
        }
        const auto &hierarchy = extension->hierarchy;
        const auto ownOutcomes = [&hierarchy, &extension](unsigned level) -> uint8_t {
            if (hierarchy.accesses[level] < extension->admissionAccesses[level] ||
                hierarchy.hits[level] < extension->admissionHits[level] ||
                hierarchy.tagMisses[level] < extension->admissionMisses[level])
                return 1u;
            const auto accesses = hierarchy.accesses[level] - extension->admissionAccesses[level];
            const auto hits = hierarchy.hits[level] - extension->admissionHits[level];
            const auto misses = hierarchy.tagMisses[level] - extension->admissionMisses[level];
            if (!accesses) return 0;
            uint8_t mask = 0;
            if (hits) mask |= 1u << 1;
            if (misses) mask |= 1u << 2;
            if (accesses != hits + misses)
                mask |= 1u;
            return mask;
        };
        uint8_t own_l2 = extension->l1ObservationMissing ? 1u :
                         follower ? 1u << 3 : ownOutcomes(1);
        uint8_t own_l3 = extension->l1ObservationMissing ? 1u :
                         follower ? 1u << 3 : ownOutcomes(2);
        if (!own_l2) own_l2 = follower ? 1u << 3 : 1u;
        if (!own_l3) {
            own_l3 = follower || own_l2 == (1u << 1) ? 1u << 3 : 1u;
        }
        const bool ambiguous = extension->l1ObservationMissing;
        const TaoTraceServiceFact fact{
            uint8_t(!ambiguous && source <= 4 ? source : 0),
            uint8_t(!ambiguous && l2_outcome <= 3 ? l2_outcome : 0),
            uint8_t(!ambiguous && l3_outcome <= 3 ? l3_outcome : 0), follower,
            own_l2, own_l3};
        extension->serviceFacts.push_back(fact);
        mutate(req, [fact](Snapshot &facts) {
            facts.serviceFacts.push_back(fact);
        });
    }

    static void
    noteSequencerShared(const RequestPtr &req, const RequestPtr &parent,
                        bool write_callback)
    {
        uint16_t sources = write_callback ? SharedParentWriteCallback : 0;
        const auto ext = parent ?
            parent->getExtension<TaoTraceRubyResponseExtension>() : nullptr;
        if (parent && parent->isInstFetch())
            sources |= SharedParentIFetch;
        if (ext) {
            const auto &h = ext->hierarchy;
            if (h.hits[0]) sources |= SharedParentL1Hit;
            if (h.tagMisses[0]) sources |= SharedParentL1Miss;
            if (h.permissionUpgrades[0] || h.mergedMisses[0] || h.remoteSupplies[0]) sources |= SharedParentL1Other;
            if (h.hits[1]) sources |= SharedParentL2Hit;
            if (h.tagMisses[1]) sources |= SharedParentL2Miss;
            if (h.permissionUpgrades[1] || h.mergedMisses[1] || h.remoteSupplies[1]) sources |= SharedParentL2Other;
            if (h.hits[2]) sources |= SharedParentL3Hit;
            if (h.tagMisses[2]) sources |= SharedParentL3Miss;
            if (h.permissionUpgrades[2] || h.mergedMisses[2] || h.remoteSupplies[2]) sources |= SharedParentL3Other;
            if ((!h.accesses[0] && !(sources & SharedParentIFetch)) ||
                (h.tagMisses[0] && !h.accesses[1]) ||
                (h.tagMisses[1] && !h.accesses[2]))
                sources |= SharedParentMissing;
        } else if (!(sources & SharedParentIFetch)) {
            sources |= SharedParentMissing;
        }
        auto extension = ensureExtension(req);
        if (extension) {
            ++extension->sequencerShared;
            extension->sharedParentSources |= sources;
        }
        mutate(req, [sources](Snapshot &facts) {
            ++facts.sequencerShared;
            facts.sharedParentSources |= sources;
        });
    }

    static void
    noteHierarchyRequest(const RequestPtr &req)
    {
        auto extension = ensureExtension(req);
        if (extension) ++extension->hierarchyRequests;
        mutate(req, [](Snapshot &facts) { ++facts.hierarchyRequests; });
    }

    static void
    noteHierarchyAttempt(const RequestPtr &req, uint64_t attempt,
                         uint64_t replaced = 0)
    {
        uint8_t previous_miss = 0;
        if (replaced && replaced != attempt) {
            auto old = sprCurrent_.find(replaced);
            if (old != sprCurrent_.end()) {
                ++spr_retired::lifecycleDiagnostics().replaced;
                if (old->second.request.lock() == req && old->second.result)
                    previous_miss = old->second.result->sticky_miss |
                                    old->second.result->miss_mask;
                old->second.consumed = true;
                if (!old->second.retained) {
                    sprCurrent_.erase(old);
                    --spr_retired::lifecycleDiagnostics().live;
                }
            }
        }
        if (attempt && req && retirementObservationActive()) {
            if (!sprCurrent_.count(attempt) && sprCurrent_.size()>=65536)
                throw std::runtime_error("SPR current Request/TBE observer capacity exhausted");
            auto inserted = sprCurrent_.try_emplace(attempt);
            auto &current = inserted.first->second;
            if (inserted.second) {
                auto &d=spr_retired::lifecycleDiagnostics();
                ++d.created; ++d.live; d.peak=std::max(d.peak,d.live);
            }
            current.request = req;
            if (!current.result) current.result =
                std::make_shared<spr_retired::Result>();
            if (replaced) {
                current.result->sticky_miss |= previous_miss;
                current.result->retried = true;
            }
        }
        mutate(req, [attempt](Snapshot &facts) {
            if (auto *a = facts.attempt(attempt, true)) {
                if (!attempt) facts.attemptOverflow = true;
                (void)a;
            }
            if (facts.hierarchyAttemptCount < 2) ++facts.hierarchyAttemptCount;
            if (facts.hierarchyAttemptCount == 1) facts.hierarchyAttempt = attempt;
        });
    }

    static void
    noteHierarchyCompletion(const RequestPtr &req, uint64_t attempt,
                            bool coalesced = false, bool resident = false)
    {
        if (req && retirementObservationActive()) {
            auto extension = ensureExtension(req);
            // Idempotent current Request completion, without a history set.
            const bool repeated=extension->sprCompletionSeen &&
                extension->sprLastCompletion==attempt;
            if (!repeated) {
                extension->sprCompletionSeen=true;
                extension->sprLastCompletion=attempt;
                spr_retired::Result result;
                unsigned prepare_status=255;
                unsigned prepare_invalidation=0;
                auto entry = sprCurrent_.find(attempt);
                if (entry != sprCurrent_.end() && !entry->second.consumed) {
                    auto &current = entry->second;
                    result = *current.result;
                    if (current.lower) {
                        result.level[2] = current.lower->level[2];
                        result.multiple_consumers |= current.lower->multiple_consumers;
                    }
                    result.stale = extension->staleServiced != 0;
                    result = spr_retired::convert(result);
                    current.consumed = true;
                    ++spr_retired::lifecycleDiagnostics().completed;
                    if (!current.retained) {
                        sprCurrent_.erase(entry);
                        --spr_retired::lifecycleDiagnostics().live;
                    }
                } else if (!attempt && coalesced && extension->sprPrepareService) {
                    const auto &service=*extension->sprPrepareService;
                    prepare_status=(service.dataComplete ? 1u : 0u) |
                        (service.invalidated ? 2u : 0u) | (service.tbeLive ? 4u : 0u) |
                        (service.invalidatedBeforeData ? 8u : 0u) |
                        (service.invalidatedAfterData ? 16u : 0u) |
                        (service.replaced ? 32u : 0u);
                    prepare_invalidation=service.invalidationSources;
                    result = extension->sprPrepareService->completedResult(
                        extension->sprPrepareAdmissionValid,
                        extension->sprPrepareAdmissionNeedsFill);
                    extension->sprPrepareService.reset();
                } else if (!attempt && coalesced && extension->sprAliasObserved) {
                    result = spr_retired::convert(extension->sprAlias);
                } else {
                    // A shared response alone proves neither a resident
                    // admission nor a nonresident FB merge. Require the
                    // controller's actual admission-state observation.
                    result = spr_retired::convert(result);
                }
                result.stale |= extension->staleServiced != 0;
                // Full forwarding supersedes earlier discarded Ruby attempts,
                // which still close their observer service lifetime above.
                if (!(extension->terminalReasonMask & taoTraceNativeReasonMask(
                        TaoTraceNativeTerminalReason::StoreForward))) {
                    extension->sprMiss.add(result);
                    const unsigned alias_category=extension->sprAliasUnclassifiedCategory;
                    mutate(req, [result, attempt, req, alias_category, prepare_status, prepare_invalidation](Snapshot &facts) {
                        if (facts.terminalReasonMask & taoTraceNativeReasonMask(
                                TaoTraceNativeTerminalReason::StoreForward)) return;
                        facts.sprMiss.add(result);
                        if (facts.admissions>1) {
                            facts.sprMiss.estimated_mask |= 7;
                            facts.sprMiss.path_mask |= 1u << 13;
                        }
                        if (!result.ready) {
                            ++facts.sprIncompleteCompletions;
                            if (!facts.sprMissingLevels) {
                                facts.sprFailedRequest=attempt;
                                facts.sprFailedAddress=req->hasPaddr() ? req->getPaddr() : 0;
                                facts.sprMissingLevels=7 & ~result.known_mask;
                                facts.sprFailedLevels=result.level;
                                facts.sprFailedAliasCategory=alias_category;
                                facts.sprFailedPrepareStatus=prepare_status;
                                facts.sprFailedPrepareInvalidation=prepare_invalidation;
                            }
                        }
                    });
                }
            }
        }
        mutate(req, [attempt, coalesced, resident](Snapshot &facts) {
            auto *a = facts.attempt(attempt, coalesced && !attempt);
            if (a) {
                ++a->completions;
                a->coalesced = coalesced;
                a->resident = resident;
            } else facts.attemptOverflow = true;
            if (facts.hierarchyCompletionCount < 2) ++facts.hierarchyCompletionCount;
            if (facts.hierarchyCompletionCount == 1)
                facts.hierarchyCompletionAttempt = attempt;
        });
    }

    // Only live Request/TBE objects are indexed. No completed attempt history
    // is queried by the primary event path.
    static void sprLookup(uint64_t id, unsigned level, unsigned outcome)
    {
        auto found = sprCurrent_.find(id);
        if (found == sprCurrent_.end() || outcome > 4) {
            if (id && found == sprCurrent_.end()) ++spr_retired::lifecycleDiagnostics().late_lookup;
            return;
        }
        constexpr spr_retired::Level values[] = {
            spr_retired::Level::Resident, spr_retired::Level::Miss,
            spr_retired::Level::Permission, spr_retired::Level::PendingFill,
            spr_retired::Level::Remote};
        spr_retired::lookup(*found->second.result, level, values[outcome]);
    }

    static void sprAliasAdmission(const RequestPtr &req, unsigned outcome)
    {
        if (!req || !retirementObservationActive() || outcome > 2) return;
        auto extension = ensureExtension(req);
        extension->sprPrepareService.reset();
        extension->sprPrepareAdmissionValid=false;
        extension->sprPrepareAdmissionNeedsFill=false;
        extension->sprAliasObserved = true;
        extension->sprAlias.level[0] = outcome ?
            spr_retired::Level::PendingFill : spr_retired::Level::Resident;
        if (outcome) {
            extension->sprAlias.service = outcome == 2 ?
                spr_retired::Service::PrefetchFollower : spr_retired::Service::DemandFollower;
            extension->sprAlias.supply = spr_retired::Supply::FillBuffer;
        }
    }

    static void sprAliasUnclassified(const RequestPtr &req, unsigned category)
    {
        if (req && retirementObservationActive() && category < 8) {
            ++spr_retired::lifecycleDiagnostics().unclassified_alias[category];
            ensureExtension(req)->sprAliasUnclassifiedCategory=category;
        }
    }

    static void sprAliasRmwAdmission(const RequestPtr &req, unsigned outcome)
    {
        sprAliasAdmission(req,outcome);
        if (req && retirementObservationActive() && outcome==0)
            ensureExtension(req)->sprAlias.level[0]=spr_retired::Level::Permission;
    }

    static uint64_t sprPrepareBegin(int core,uint64_t line,bool pendingFill)
    {
        if (!retirementObservationActive()) return 0;
        return spr_retired::prepareServices().begin(core,line,pendingFill);
    }
    static void sprPrepareFollow(const RequestPtr &req,int core,uint64_t line)
    {
        if (!req || !retirementObservationActive()) return;
        auto extension=ensureExtension(req);
        extension->sprAliasObserved=false;
        extension->sprAlias={};
        extension->sprPrepareService = spr_retired::prepareServices().follow(core,line);
        const auto &service=extension->sprPrepareService;
        // SP_IE acknowledged an invalidation before this follower joined.
        // Only a later data write by this live generation can complete it.
        extension->sprPrepareAdmissionNeedsFill=service && !service->pendingFill &&
            service->invalidated && !service->dataComplete && !service->replaced &&
            service->invalidationSources==(1u<<1);
        extension->sprPrepareAdmissionValid=extension->sprPrepareService &&
            (extension->sprPrepareService->pendingFill ||
             !extension->sprPrepareService->invalidated ||
             extension->sprPrepareAdmissionNeedsFill);
    }
    static void sprPrepareData(uint64_t generation)
    { if (generation) spr_retired::prepareServices().data(generation); }
    static void sprPrepareClose(uint64_t generation)
    { if (generation) spr_retired::prepareServices().close(generation); }
    static void sprPrepareInvalidate(int core,uint64_t line,unsigned reason=0)
    { spr_retired::prepareServices().invalidate(core,line,reason); }
    static size_t sprPrepareLive()
    { return spr_retired::prepareServices().live(); }

    static void sprMarkPrepareLeader(uint64_t id,uint64_t generation)
    {
        auto leader=sprCurrent_.find(id);
        if (leader==sprCurrent_.end()) return;
        const auto service=spr_retired::prepareServices().tbe(generation);
        if (service && service->pendingFill) return;
        auto &result=*leader->second.result;
        result.preparePermissionGuard=true;
        result.level[0]=service && !service->invalidated ?
            spr_retired::Level::Permission : spr_retired::Level::Undecided;
    }

    // Borrow only the current live leader of this Sequencer line callback.
    // A shared pointer keeps its observed lookup alive across leader retirement.
    static std::shared_ptr<spr_retired::Result> sprBorrowService(uint64_t id)
    {
        const auto it=sprCurrent_.find(id);
        return it!=sprCurrent_.end() && !it->second.consumed ? it->second.result : nullptr;
    }
    static void sprRebindPrepareFollower(const RequestPtr &req,
        const std::shared_ptr<spr_retired::Result> &leader,bool dataWitness)
    {
        if (!req || !leader) return;
        auto extension=req->getExtension<TaoTraceRubyResponseExtension>();
        if (!extension || !extension->sprPrepareService ||
            extension->sprPrepareService->completedResult(
                extension->sprPrepareAdmissionValid,
                extension->sprPrepareAdmissionNeedsFill).ready) return;
        using namespace spr_retired;
        Result rebound;
        const auto level=leader->level[0];
        const bool dataMiss=level==Level::Miss || level==Level::PendingFill;
        if (dataMiss && dataWitness) {
            rebound.level[0]=Level::PendingFill;
            rebound.service=Service::DemandFollower;
        } else if (!dataMiss && (level==Level::Resident || level==Level::Permission)) {
            // Current line's observed resident/permission service, not the
            // abandoned prepare generation or the leader's lower-level bits.
            rebound.level[0]=Level::Permission;
        } else return;
        extension->sprPrepareService.reset();
        extension->sprPrepareAdmissionValid=false;
        extension->sprPrepareAdmissionNeedsFill=false;
        extension->sprAlias=rebound;
        extension->sprAliasObserved=true;
    }

    static void sprRetain(uint64_t id)
    {
        auto found = sprCurrent_.find(id);
        if (found != sprCurrent_.end() && !found->second.retained) {
            found->second.retained = true;
            ++spr_retired::lifecycleDiagnostics().retained;
        }
    }

    static void sprRelease(uint64_t id)
    {
        auto found = sprCurrent_.find(id);
        if (found == sprCurrent_.end()) return;
        if (found->second.retained) ++spr_retired::lifecycleDiagnostics().released;
        found->second.retained = false;
        if (found->second.consumed) {
            sprCurrent_.erase(found);
            --spr_retired::lifecycleDiagnostics().live;
        }
    }

    static void sprFollow(uint64_t id, uint64_t parent)
    {
        auto child = sprCurrent_.find(id), service = sprCurrent_.find(parent);
        if (child == sprCurrent_.end() || service == sprCurrent_.end()) return;
        child->second.lower = service->second.result;
        if (service->second.result.use_count()>2)
            service->second.result->multiple_consumers=true;
        child->second.result->service = spr_retired::Service::DemandFollower;
    }

    static void sprPrefetchBegin(uint64_t generation)
    {
        if (generation && !sprPrefetch_.count(generation) && sprPrefetch_.size()>=65536)
            throw std::runtime_error("SPR current prefetch TBE observer capacity exhausted");
        if (generation) sprPrefetch_[generation] =
            std::make_shared<spr_retired::Result>();
    }
    static void sprPrefetchLookup(uint64_t generation, unsigned outcome)
    {
        auto found = sprPrefetch_.find(generation);
        if (found == sprPrefetch_.end() || outcome > 4) return;
        constexpr spr_retired::Level values[] = {
            spr_retired::Level::Resident, spr_retired::Level::Miss,
            spr_retired::Level::Permission, spr_retired::Level::PendingFill,
            spr_retired::Level::Remote};
        spr_retired::lookup(*found->second, 2, values[outcome]);
    }
    static void sprPrefetchFollow(uint64_t id, uint64_t generation)
    {
        auto child = sprCurrent_.find(id);
        auto parent = sprPrefetch_.find(generation);
        if (child == sprCurrent_.end() || parent == sprPrefetch_.end()) return;
        child->second.lower = parent->second;
        if (parent->second.use_count()>2) parent->second->multiple_consumers=true;
        child->second.result->service = spr_retired::Service::PrefetchFollower;
    }
    static void sprPrefetchFinish(uint64_t generation)
    {
        sprPrefetch_.erase(generation);
    }

    static void
    noteHierarchy(uint32_t context_id, uint64_t seq_num, unsigned level,
                  TaoTraceNativeCacheOutcome outcome)
    {
        mutate(context_id, seq_num, [level, outcome](Snapshot &facts) {
            facts.hierarchy.observe(level, outcome);
        });
    }

    // Used when the Sequencer directly observes a request-local cache/FB
    // outcome. Do not transfer the initiating request's lower-level facts.
    static void
    noteHierarchy(const RequestPtr &req, unsigned level,
                  TaoTraceNativeCacheOutcome outcome)
    {
        auto extension = ensureExtension(req);
        if (extension) {
            extension->hierarchy.observe(level, outcome);
            if (level == 0 && outcome == TaoTraceNativeCacheOutcome::TagMiss &&
                !extension->hasL1MissTick) {
                extension->hasL1MissTick = true;
                extension->firstL1MissTick = curTick();
            }
        }
        mutate(req, [level, outcome](Snapshot &facts) {
            facts.hierarchy.observe(level, outcome);
        });
    }

    static void
    noteAttemptHierarchy(uint32_t context_id, uint64_t seq_num,
                         uint64_t attempt, unsigned level,
                         TaoTraceNativeCacheOutcome outcome)
    {
        mutate(context_id, seq_num, [attempt, level, outcome](Snapshot &facts) {
            if (auto *a = facts.attempt(attempt))
                a->hierarchy.observe(level, outcome);
            else facts.attemptOverflow = true;
        });
    }

    static void
    noteLatePrefetch(uint32_t context_id, uint64_t seq_num, unsigned level)
    {
        if (level >= TaoTraceNativeHierarchyFacts::Levels) return;
        mutate(context_id, seq_num, [level](Snapshot &facts) {
            ++facts.hierarchy.latePrefetches[level];
        });
    }

    // Data_Stale service: the demand completed but the delivered data was
    // not cached (line invalidated in flight).  Lifecycle stays resolved; the
    // membership engine is required to fail it closed.
    static void
    noteStaleServiced(const RequestPtr &req)
    {
        auto extension = ensureExtension(req);
        if (extension) ++extension->staleServiced;
        mutate(req, [](Snapshot &facts) { ++facts.staleServiced; });
    }

    // Oracle generation rejection for an attached demand follower.  chain:
    // 0 = L1D-begun (G0), 1 = private-L2-begun (G1).  The parent outcome may
    // never be inherited once any reject bit is set.
    static void
    notePrefetchParentRejected(uint32_t context_id, uint64_t seq_num,
                               unsigned chain, uint8_t mask)
    {
        if (!mask) return;
        mutate(context_id, seq_num, [chain, mask](Snapshot &facts) {
            if (chain == 0) facts.l1ParentRejectMask |= mask;
            else if (chain == 1) facts.l2ParentRejectMask |= mask;
        });
    }

    static void
    noteL2PrefetchParentLlc(uint32_t context_id, uint64_t seq_num,
                            uint64_t attempt, unsigned outcome,
                            int sender_kind, int sender_id, int response_kind)
    {
        if (!attempt || outcome >= 5) return;
        if (sender_kind < 1 || sender_kind > 2 || sender_id < 0) {
            sender_kind = 0;
            sender_id = -1;
        }
        if (response_kind < 1 || response_kind > 2) response_kind = 0;
        mutate(context_id, seq_num, [attempt, outcome, sender_kind, sender_id, response_kind](Snapshot &facts) {
            if (auto *a = facts.attempt(attempt)) {
                ++a->parentCount[1];
                a->parentOutcome[1] = sender_kind && response_kind ? int(outcome) : -1;
            } else facts.attemptOverflow = true;
            if (facts.l2ParentWitnessCount < 2) ++facts.l2ParentWitnessCount;
            if (facts.l2ParentWitnessCount == 1) {
                facts.l2ParentWitnessAttempt = attempt;
                facts.l2ParentSenderId = sender_id;
            }
            ++facts.hierarchy.l2PrefetchParentLlc[outcome];
            ++facts.hierarchy.l2PrefetchParentSender[unsigned(sender_kind)];
            ++facts.hierarchy.l2PrefetchParentResponse[unsigned(response_kind)];
        });
    }

    static void
    noteL1PrefetchParentLlc(uint32_t context_id, uint64_t seq_num,
                            uint64_t attempt, unsigned outcome,
                            int sender_kind, int sender_id, int response_kind)
    {
        if (!attempt || outcome >= 5) return;
        if (sender_kind < 1 || sender_kind > 2 || sender_id < 0) {
            sender_kind = 0;
            sender_id = -1;
        }
        if (response_kind < 1 || response_kind > 2) response_kind = 0;
        mutate(context_id, seq_num, [attempt, outcome, sender_kind, sender_id,
                                      response_kind](Snapshot &facts) {
            if (auto *a = facts.attempt(attempt)) {
                ++a->parentCount[0];
                a->parentOutcome[0] = sender_kind && response_kind ? int(outcome) : -1;
            } else facts.attemptOverflow = true;
            if (facts.l1ParentWitnessCount < 2) ++facts.l1ParentWitnessCount;
            if (facts.l1ParentWitnessCount == 1) {
                facts.l1ParentWitnessAttempt = attempt;
                facts.l1ParentSenderId = sender_id;
            }
            ++facts.hierarchy.l1PrefetchParentLlc[outcome];
            ++facts.hierarchy.l1PrefetchParentSender[unsigned(sender_kind)];
            ++facts.hierarchy.l1PrefetchParentResponse[unsigned(response_kind)];
        });
    }

    static void
    noteUniqueFill(uint32_t context_id, uint64_t seq_num)
    {
        mutate(context_id, seq_num, [](Snapshot &facts) {
            ++facts.hierarchy.uniqueFills;
        });
    }

    static void
    noteRubyMemoryFetch(uint32_t context_id, uint64_t seq_num)
    {
        mutate(context_id, seq_num, [](Snapshot &facts) {
            ++facts.hierarchy.rubyMemoryFetches;
        });
    }

    static void
    noteMemoryReadTransaction(uint32_t context_id, uint64_t seq_num)
    {
        mutate(context_id, seq_num, [](Snapshot &facts) {
            ++facts.hierarchy.memoryReadTransactions;
        });
    }

    static void
    noteIssuanceClosed(const RequestPtr &req)
    {
        auto extension = ensureExtension(req);
        if (extension) {
            extension->issuanceClosed = true;
            taoTraceNativeRefreshBypassTerminal(*extension);
        }
        mutate(req, [](Snapshot &facts) {
            facts.issuanceClosed = true;
            taoTraceNativeRefreshBypassTerminal(facts);
        });
    }

    static void
    noteTerminal(const RequestPtr &req, TaoTraceNativeTerminalReason reason)
    {
        auto extension = ensureExtension(req);
        if (reason == TaoTraceNativeTerminalReason::StoreForward) {
            spr_retired::Result result;
            result.service = spr_retired::Service::Forwarded;
            result = spr_retired::convert(result);
            if (extension) {
                extension->sprMiss={};
                extension->sprMiss.add(result);
            }
            mutate(req, [result](Snapshot &facts) {
                facts.sprMiss={};
                facts.sprMiss.add(result);
            });
        }
        if (extension) {
            extension->terminalNoRuby = true;
            extension->terminalReasonMask |= taoTraceNativeReasonMask(reason);
            taoTraceNativeRefreshBypassTerminal(*extension);
        }
        mutate(req, [reason](Snapshot &facts) {
            facts.terminalNoRuby = true;
            facts.terminalReasonMask |= taoTraceNativeReasonMask(reason);
            taoTraceNativeRefreshBypassTerminal(facts);
        });
    }

    static void
    noteCommitted(uint32_t context_id, uint64_t seq_num)
    {
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        facts.committed = true;
    }

    static void
    noteSquashed(uint32_t context_id, uint64_t seq_num)
    {
        auto context = records_.find(context_id);
        if (context == records_.end()) return;
        auto entry = context->second.find(seq_num);
        if (entry == context->second.end()) return;
        entry->second.squashed = true;
        if (entry->second.abandonedServiceDrained()) erase(context_id, seq_num);
    }

    static void
    noteObservedComplete(uint32_t context_id, uint64_t seq_num,
                         uint32_t hierarchy_requests, uint32_t responses,
                         uint32_t external_hits,
                         uint32_t external_misses, uint32_t coalesced,
                         uint64_t responder_machine_mask,
                         uint32_t responder_machine_unknown,
                         bool has_admission_tick,
                         uint64_t first_admission_tick,
                         uint64_t last_admission_tick,
                         bool has_response_tick,
                         uint64_t last_response_tick,
                         const TaoTraceNativeHierarchyFacts &hierarchy,
                         uint32_t stale_serviced = 0)
    {
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        // Stale markers come from the per-Request extension path; the
        // functional attr import carries none, so never erase an observed one.
        facts.staleServiced = std::max(facts.staleServiced, stale_serviced);
        // This path covers requests already in flight at the warmup/measure
        // boundary. Their Request extension proves that every physical
        // fragment completed even though registry tracking was not yet on.
        // Normal measured requests were already observed exactly once by the
        // registry.  Do not replace their instruction-wide hierarchy with the
        // extension carried by the last packet of a split access.  Import the
        // extension only for a request already in flight at measurement start.
        // Its admission can precede enableContext() while its response follows
        // it, so the registry may contain a response but no admission. Replace
        // (rather than merge) that partial snapshot with the Request
        // extension's complete lifecycle; merging would double-count the
        // post-boundary response.
        if (facts.admissions == 0) {
            facts.admissions = responses;
            facts.hierarchyRequests = hierarchy_requests;
            facts.responses = responses;
            facts.externalHits = external_hits;
            facts.externalMisses = external_misses;
            facts.coalesced = coalesced;
            facts.responderMachineMask = responder_machine_mask;
            facts.responderMachineUnknown = responder_machine_unknown;
            facts.hasAdmissionTick = has_admission_tick;
            facts.firstAdmissionTick = first_admission_tick;
            facts.lastAdmissionTick = last_admission_tick;
            facts.hasResponseTick = has_response_tick;
            facts.lastResponseTick = last_response_tick;
            facts.hierarchy = hierarchy;
        }
        facts.issuanceClosed = true;
    }

    static void
    noteObservedLifecycle(
        uint32_t context_id, uint64_t seq_num,
        const TaoTraceRubyResponseExtension &extension)
    {
        // A load may finish in Ruby (or through store forwarding/local
        // access) before the process-wide measurement marker, remain in a
        // different core's ROB, and retire just after the marker.  The
        // Request extension is the authoritative finite lifecycle ledger for
        // that exact boundary case. Import only a complete, identity-matched
        // extension to replace a missing/partial prefix. A resolved registry
        // is already authoritative and must not be counted twice.
        if (!extension.resolved() ||
            (extension.bypassAccepted &&
             (!extension.bypassIdentityKnown ||
              extension.bypassContext != context_id ||
              extension.bypassSequence != seq_num)) ||
            !shouldTrack(context_id, seq_num)) return;
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        // A measured prefix can already contain responses. Replace its
        // lifecycle, never add it again; keep independent SLICC observations.
        if (facts.resolved() || facts.bypassProtocolErrors ||
            facts.admissions > extension.admissions ||
            facts.responses > extension.responses ||
            facts.bypassAccepted > extension.bypassAccepted ||
            facts.bypassResponses > extension.bypassResponses) return;
        facts.bypassAccepted = extension.bypassAccepted;
        facts.bypassResponses = extension.bypassResponses;
        facts.bypassProtocolErrors = extension.bypassProtocolErrors;
        facts.admissions = extension.admissions;
        facts.aliasedAdmissions = extension.aliasedAdmissions;
        facts.hierarchyRequests = extension.hierarchyRequests;
        facts.responses = extension.responses;
        facts.externalHits = extension.externalHits;
        facts.externalMisses = extension.externalMisses;
        facts.coalesced = extension.coalesced;
        facts.sequencerShared = extension.sequencerShared;
        facts.sharedParentSources = extension.sharedParentSources;
        facts.serviceFacts = extension.serviceFacts;
        facts.serviceFactIdentityAmbiguous = extension.serviceFactIdentityAmbiguous;
        facts.l1ObservationMissing = extension.l1ObservationMissing ||
                                     extension.l1ObservationPending();
        facts.l1FinalMask = extension.l1FinalMask;
        facts.serviceExpected = extension.serviceExpected;
        facts.responderMachineMask = extension.responderMachineMask;
        facts.responderMachineUnknown = extension.responderMachineUnknown;
        facts.hasAdmissionTick = extension.hasAdmissionTick;
        facts.firstAdmissionTick = extension.firstAdmissionTick;
        facts.lastAdmissionTick = extension.lastAdmissionTick;
        facts.hasResponseTick = extension.hasResponseTick;
        facts.lastResponseTick = extension.lastResponseTick;
        facts.issuanceClosed = extension.issuanceClosed;
        facts.terminalNoRuby = extension.terminalNoRuby;
        facts.terminalReasonMask = extension.terminalReasonMask;
        facts.staleServiced = std::max(facts.staleServiced,
                                       extension.staleServiced);
        if (!facts.sprMiss.seen) facts.sprMiss = extension.sprMiss;
        if (facts.hierarchy.accesses == std::array<uint32_t, 3>{} &&
            !facts.hierarchy.uniqueFills && !facts.hierarchy.rubyMemoryFetches &&
            !facts.hierarchy.memoryReadTransactions)
            facts.hierarchy = extension.hierarchy;
    }

    static void
    noteTerminal(uint32_t context_id, uint64_t seq_num,
                 TaoTraceNativeTerminalReason reason)
    {
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        facts.terminalNoRuby = true;
        facts.terminalReasonMask |= taoTraceNativeReasonMask(reason);
        taoTraceNativeRefreshBypassTerminal(facts);
    }

    static Snapshot
    snapshot(uint32_t context_id, uint64_t seq_num)
    {
        const auto context = records_.find(context_id);
        if (context == records_.end()) return {};
        const auto entry = context->second.find(seq_num);
        return entry == context->second.end() ? Snapshot{} : entry->second;
    }

    static void
    erase(uint32_t context_id, uint64_t seq_num)
    {
        auto context = records_.find(context_id);
        if (context == records_.end()) return;
        context->second.erase(seq_num);
        if (context->second.empty() &&
            enabled_contexts_.count(context_id) == 0) {
            records_.erase(context);
        }
    }

  private:
    struct SprCurrent {
        std::weak_ptr<Request> request;
        std::shared_ptr<spr_retired::Result> result, lower;
        bool retained = false, consumed = false;
    };
    inline static std::unordered_map<uint64_t, SprCurrent> sprCurrent_;
    inline static std::unordered_map<uint64_t,
        std::shared_ptr<spr_retired::Result>> sprPrefetch_;
    using ContextRecords = std::unordered_map<uint64_t, Snapshot>;
    inline static std::unordered_map<uint32_t, ContextRecords> records_;
    inline static std::unordered_set<uint32_t> enabled_contexts_;
    inline static std::unordered_set<uint32_t> pretracking_contexts_;
    inline static std::unordered_set<uint32_t> retirement_contexts_;

    static bool traceOwnsContext(uint32_t context_id)
    {
        return enabled_contexts_.count(context_id) ||
            pretracking_contexts_.count(context_id);
    }

    static bool
    identity(const RequestPtr &req, uint32_t &context_id, uint64_t &seq_num)
    {
        if (!req || !req->hasContextId() || !req->hasInstSeqNum()) {
            return false;
        }
        context_id = uint32_t(req->contextId());
        seq_num = uint64_t(req->getReqInstSeqNum());
        return true;
    }

    static bool
    shouldAdmit(uint32_t context_id, uint64_t seq_num)
    {
        return enabled_contexts_.count(context_id) != 0 ||
            pretracking_contexts_.count(context_id) != 0 ||
            shouldTrack(context_id, seq_num);
    }

    static bool
    shouldTrack(uint32_t context_id, uint64_t seq_num)
    {
        if (enabled_contexts_.count(context_id) != 0 ||
            retirement_contexts_.count(context_id) != 0) return true;
        const auto context = records_.find(context_id);
        return context != records_.end() &&
            context->second.count(seq_num) != 0;
    }

    static std::shared_ptr<TaoTraceRubyResponseExtension>
    ensureExtension(const RequestPtr &req)
    {
        if (!req) return {};
        auto extension =
            req->getExtension<TaoTraceRubyResponseExtension>();
        if (!extension) {
            extension = std::make_shared<TaoTraceRubyResponseExtension>();
            req->setExtension(extension);
        }
        return extension;
    }

    template <class Callback>
    static void
    mutate(const RequestPtr &req, Callback callback)
    {
        uint32_t context_id = 0;
        uint64_t seq_num = 0;
        if (!identity(req, context_id, seq_num) ||
            !shouldTrack(context_id, seq_num)) {
            return;
        }
        mutate(context_id, seq_num, callback);
    }

    template <class Callback>
    static void
    mutate(uint32_t context_id, uint64_t seq_num, Callback callback)
    {
        if (!shouldTrack(context_id, seq_num)) return;
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        callback(facts);
        if (facts.abandonedServiceDrained()) {
            erase(context_id, seq_num);
        }
    }
};

/**
 * Exact CPL-window instruction-fetch ledger.
 *
 * Unlike gem5's reset/dump statistics, this registry is enabled and frozen by
 * each TaoTrace instance at its first CPL event and functional target.
 * Fetch supplies lifecycle events, so the accounting observes real O3 ITLB
 * and I-cache requests (including wrong-path requests) without changing the
 * functional trace or the timing state machine.
 */
class TaoTraceFrontendRegistry
{
  public:
    static constexpr unsigned StatusCount = 13;

    enum class Terminal : unsigned
    {
        TranslationSquash,
        TranslationFault,
        NoGoodAddress,
        RetryDiscard,
        Response,
        SquashedResponse,
        DsbValidation,
        Count
    };

    struct Snapshot
    {
        bool found = false;
        uint64_t inflightAtStart = 0;
        uint64_t requestsStarted = 0;
        uint64_t userModeRequestsStarted = 0;
        uint64_t kernelModeRequestsStarted = 0;
        uint64_t invalidSameBlockRefetches = 0;
        uint64_t invalidNewBlockRequests = 0;
        uint64_t validBlockChanges = 0;
        uint64_t translationsCompleted = 0;
        uint64_t sendAttempts = 0;
        uint64_t requestsSent = 0;
        uint64_t sendRejects = 0;
        std::array<uint64_t, static_cast<unsigned>(Terminal::Count)>
            terminals{};
        uint64_t inflightAtEnd = 0;
        uint64_t squashEvents = 0;
        uint64_t squashEventsWithOutstanding = 0;
        std::array<uint64_t, StatusCount> statusCycles{};
        uint64_t requestToResponseTicks = 0;
        uint64_t requestToResponseSamples = 0;

        uint64_t
        terminalCount() const
        {
            uint64_t total = 0;
            for (uint64_t value : terminals) total += value;
            return total;
        }

        uint64_t
        statusCycleCount() const
        {
            uint64_t total = 0;
            for (uint64_t value : statusCycles) total += value;
            return total;
        }

        bool
        requestPopulationConserved() const
        {
            return inflightAtStart + requestsStarted ==
                terminalCount() + inflightAtEnd;
        }

        bool
        requestModeConserved() const
        {
            return requestsStarted ==
                userModeRequestsStarted + kernelModeRequestsStarted;
        }

        bool
        requestReasonConserved() const
        {
            return requestsStarted == invalidSameBlockRefetches +
                invalidNewBlockRequests + validBlockChanges;
        }

        bool
        sendAccountingConserved() const
        {
            return sendAttempts == requestsSent + sendRejects;
        }
    };

    static void
    prepareContext(uint32_t context_id)
    {
        auto &context = contexts_[context_id];
        context = ContextState{};
        context.pretracking = true;
    }

    static void
    enableContext(uint32_t context_id)
    {
        auto &context = contexts_[context_id];
        context.snapshot = Snapshot{};
        context.snapshot.found = true;
        context.pretracking = false;
        context.enabled = true;
        for (auto &entry : context.requests) {
            entry.second.counted = true;
            entry.second.startedInWindow = false;
            ++context.snapshot.inflightAtStart;
        }
    }

    static void
    disableContext(uint32_t context_id)
    {
        auto found = contexts_.find(context_id);
        if (found == contexts_.end()) return;
        auto &context = found->second;
        if (!context.enabled) return;
        context.snapshot.inflightAtEnd = 0;
        for (auto &entry : context.requests) {
            if (entry.second.counted) {
                ++context.snapshot.inflightAtEnd;
                entry.second.counted = false;
            }
        }
        context.enabled = false;
        context.pretracking = false;
    }

    static void
    noteRequestStart(const RequestPtr &req, bool in_user_mode,
                     bool buffer_valid, bool same_buffer_block,
                     uint64_t tick)
    {
        uint32_t context_id = 0;
        if (!identity(req, context_id)) return;
        auto found = contexts_.find(context_id);
        if (found == contexts_.end() ||
            (!found->second.pretracking && !found->second.enabled)) {
            return;
        }
        auto &context = found->second;
        RequestState request;
        request.startTick = tick;
        request.counted = context.enabled;
        request.startedInWindow = context.enabled;
        context.requests[req.get()] = request;
        if (!context.enabled) return;

        auto &snapshot = context.snapshot;
        ++snapshot.requestsStarted;
        if (in_user_mode) ++snapshot.userModeRequestsStarted;
        else ++snapshot.kernelModeRequestsStarted;
        if (!buffer_valid && same_buffer_block) {
            ++snapshot.invalidSameBlockRefetches;
        } else if (!buffer_valid) {
            ++snapshot.invalidNewBlockRequests;
        } else {
            ++snapshot.validBlockChanges;
        }
    }

    static void
    noteTranslationComplete(const RequestPtr &req)
    {
        mutateCounted(req, [](Snapshot &snapshot, RequestState &request) {
            if (request.translationCompleted) return;
            request.translationCompleted = true;
            ++snapshot.translationsCompleted;
        });
    }

    static void
    noteSendAttempt(const RequestPtr &req, bool accepted)
    {
        mutateCounted(req, [accepted](Snapshot &snapshot, RequestState &) {
            ++snapshot.sendAttempts;
            if (accepted) ++snapshot.requestsSent;
            else ++snapshot.sendRejects;
        });
    }

    static void
    noteSquash(uint32_t context_id, bool had_outstanding)
    {
        auto found = contexts_.find(context_id);
        if (found == contexts_.end() || !found->second.enabled) return;
        ++found->second.snapshot.squashEvents;
        if (had_outstanding) {
            ++found->second.snapshot.squashEventsWithOutstanding;
        }
    }

    static void
    noteStatusCycle(uint32_t context_id, unsigned status)
    {
        auto found = contexts_.find(context_id);
        if (found == contexts_.end() || !found->second.enabled ||
            status >= StatusCount) {
            return;
        }
        ++found->second.snapshot.statusCycles[status];
    }

    static void
    noteTerminal(const RequestPtr &req, Terminal terminal, uint64_t tick)
    {
        uint32_t context_id = 0;
        if (!identity(req, context_id)) return;
        auto context_it = contexts_.find(context_id);
        if (context_it == contexts_.end()) return;
        auto &context = context_it->second;
        auto request_it = context.requests.find(req.get());
        if (request_it == context.requests.end()) return;
        const RequestState request = request_it->second;
        if (context.enabled && request.counted) {
            ++context.snapshot.terminals[static_cast<unsigned>(terminal)];
            if (request.startedInWindow &&
                (terminal == Terminal::Response ||
                 terminal == Terminal::SquashedResponse) &&
                tick >= request.startTick) {
                context.snapshot.requestToResponseTicks +=
                    tick - request.startTick;
                ++context.snapshot.requestToResponseSamples;
            }
        }
        context.requests.erase(request_it);
    }

    static Snapshot
    snapshot(uint32_t context_id)
    {
        const auto found = contexts_.find(context_id);
        return found == contexts_.end() ? Snapshot{} : found->second.snapshot;
    }

  private:
    struct RequestState
    {
        uint64_t startTick = 0;
        bool counted = false;
        bool startedInWindow = false;
        bool translationCompleted = false;
    };

    struct ContextState
    {
        bool pretracking = false;
        bool enabled = false;
        Snapshot snapshot;
        std::unordered_map<const Request *, RequestState> requests;
    };

    inline static std::unordered_map<uint32_t, ContextState> contexts_;

    static bool
    identity(const RequestPtr &req, uint32_t &context_id)
    {
        if (!req || !req->hasContextId()) return false;
        context_id = uint32_t(req->contextId());
        return true;
    }

    template <class Callback>
    static void
    mutateCounted(const RequestPtr &req, Callback callback)
    {
        uint32_t context_id = 0;
        if (!identity(req, context_id)) return;
        auto context_it = contexts_.find(context_id);
        if (context_it == contexts_.end() || !context_it->second.enabled) {
            return;
        }
        auto &context = context_it->second;
        auto request_it = context.requests.find(req.get());
        if (request_it == context.requests.end() ||
            !request_it->second.counted) {
            return;
        }
        callback(context.snapshot, request_it->second);
    }
};

} // namespace gem5

#endif // __MEM_TAOTRACE_RESPONSE_HH__
