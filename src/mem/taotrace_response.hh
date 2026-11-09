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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/extensible.hh"
#include "mem/request.hh"
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
};

enum class TaoTraceNativeCacheOutcome : uint32_t
{
    Hit = 0,
    TagMiss = 1,
    PermissionUpgrade = 2,
    MergedMiss = 3,
    RemoteSupply = 4,
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

class TaoTraceRubyResponseExtension
    : public Extension<Request, TaoTraceRubyResponseExtension>
{
  public:
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
    TaoTraceNativeHierarchyFacts hierarchy;
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
        hierarchy.merge(other.hierarchy);
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
        bool found = false;
        bool committed = false;
        bool squashed = false;
        uint32_t admissions = 0;
        uint32_t aliasedAdmissions = 0;
        uint32_t hierarchyRequests = 0;
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
        TaoTraceNativeHierarchyFacts hierarchy;

        bool
        resolved() const
        {
            return found && responses == admissions &&
                (issuanceClosed || terminalNoRuby);
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
        if ((facts.squashed ||
             (facts.committed && retirement_contexts_.count(context_id) &&
              !traceOwnsContext(context_id))) && facts.resolved()) {
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
        if (extension) extension->issuanceClosed = true;
        mutate(req, [](Snapshot &facts) { facts.issuanceClosed = true; });
    }

    static void
    noteTerminal(const RequestPtr &req, TaoTraceNativeTerminalReason reason)
    {
        auto extension = ensureExtension(req);
        if (extension) {
            extension->terminalNoRuby = true;
            extension->terminalReasonMask |= taoTraceNativeReasonMask(reason);
        }
        mutate(req, [reason](Snapshot &facts) {
            facts.terminalNoRuby = true;
            facts.terminalReasonMask |= taoTraceNativeReasonMask(reason);
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
        if (entry->second.resolved()) erase(context_id, seq_num);
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
                         const TaoTraceNativeHierarchyFacts &hierarchy)
    {
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
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
        // that exact boundary case.  Import only a complete extension and
        // only when registry tracking has no admission, so normal measured
        // requests cannot be counted twice.
        const bool complete = extension.terminalNoRuby ||
            (extension.issuanceClosed &&
             extension.responses == extension.admissions &&
             extension.responses != 0);
        if (!complete) return;
        auto &facts = records_[context_id][seq_num];
        facts.found = true;
        if (facts.admissions != 0 || facts.responses != 0 ||
            facts.terminalNoRuby) {
            return;
        }
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
        if (facts.squashed && facts.resolved()) {
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
