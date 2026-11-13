/*
 * Retired-load cache-event membership engine (observer-only rule evaluator).
 *
 * Pure C++17, no gem5/simulator includes: it evaluates per-retired-macro
 * L1/L2/FB/L3 hit/miss membership from already-closed native facts.  Reason
 * ids and certainty levels are the machine-readable contract from
 * configs/retired-load-cache-membership-v1.json. Behavior not specified by Intel documentation is
 * estimated at most, never exact; every ambiguous path fails closed.  A
 * coalesced/see-parent follower never inherits a parent miss without the
 * strict uniqueness and proven-consumption gates encoded by the caller's
 * evidence flags.
 */
#ifndef __MEM_RETIRED_CACHE_MEMBERSHIP_HH__
#define __MEM_RETIRED_CACHE_MEMBERSHIP_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gem5
{
namespace retired_membership
{

enum class Certainty : unsigned char
{
    Unclassified = 0,
    Exact = 1,
    Estimated = 2,
};

enum class Membership : unsigned char
{
    Unknown = 0,
    Member = 1,
    Nonmember = 2,
};

// Retired PMU events, in fixed array order.
enum Event : unsigned
{
    L1Hit = 0,
    L1Miss = 1,
    FbHit = 2,
    L2Hit = 3,
    L2Miss = 4,
    L3Hit = 5,
    L3Miss = 6,
    Events = 7,
};

struct Reasons
{
    static constexpr const char *kDirectL1Hit = "E-DIRECT-L1-HIT";
    static constexpr const char *kDirectL1Miss = "E-DIRECT-L1-MISS";
    static constexpr const char *kDirectL2Hit = "E-DIRECT-L2-HIT";
    static constexpr const char *kDirectL2Miss = "E-DIRECT-L2-MISS";
    static constexpr const char *kDirectL3Hit = "E-DIRECT-L3-HIT";
    static constexpr const char *kDirectL3Miss = "E-DIRECT-L3-MISS";
    static constexpr const char *kCompleteL1HitLowerNeg =
        "E-COMPLETE-L1HIT-LOWER-NEG";
    static constexpr const char *kStoreForwardNeg = "R-STORE-FORWARD-NEG";
    static constexpr const char *kPermissionUpgradeNeg =
        "R-PERMISSION-UPGRADE-NEG";
    static constexpr const char *kSwPrefetchPop = "E-SW-PREFETCH-POP";
    static constexpr const char *kFbCoalesced = "R-FB-COALESCED";
    static constexpr const char *kLateL2PrefetchFollower =
        "R-LATE-L2-PREFETCH-FOLLOWER";
    static constexpr const char *kLateL1PrefetchFollower =
        "R-LATE-L1-PREFETCH-FOLLOWER";
    static constexpr const char *kRemoteSupply = "R-REMOTE-SUPPLY";
    static constexpr const char *kMergedMiss = "R-MERGED-MISS";
    static constexpr const char *kMixedFragment = "R-MIXED-FRAGMENT";
    static constexpr const char *kDeviceBypassNeg = "R-DEVICE-BYPASS-NEG";
    static constexpr const char *kSwPrefetchL2L3Neg =
        "R-SW-PREFETCH-L2L3-NEG";
    static constexpr const char *kIncompleteLifecycle =
        "U-INCOMPLETE-LIFECYCLE";
    static constexpr const char *kStaleInvalidated = "U-STALE-INVALIDATED";
    static constexpr const char *kMultiFollowerAmbiguous =
        "U-MULTI-FOLLOWER-AMBIGUOUS";
    static constexpr const char *kMixedOwnershipUnresolved =
        "U-MIXED-OWNERSHIP-UNRESOLVED";
    static constexpr const char *kBoundary = "U-BOUNDARY";
    static constexpr const char *kUnsupportedIsa = "U-UNSUPPORTED-ISA";
};

// Stable ordered list for contract cross-checks (Python test verifies this is
// exactly the reasons[] set of retired-load-cache-membership-v1.json).
inline constexpr const char *const allReasons[24] = {
    Reasons::kDirectL1Hit,
    Reasons::kDirectL1Miss,
    Reasons::kDirectL2Hit,
    Reasons::kDirectL2Miss,
    Reasons::kDirectL3Hit,
    Reasons::kDirectL3Miss,
    Reasons::kCompleteL1HitLowerNeg,
    Reasons::kStoreForwardNeg,
    Reasons::kPermissionUpgradeNeg,
    Reasons::kSwPrefetchPop,
    Reasons::kFbCoalesced,
    Reasons::kLateL2PrefetchFollower,
    Reasons::kLateL1PrefetchFollower,
    Reasons::kRemoteSupply,
    Reasons::kMergedMiss,
    Reasons::kMixedFragment,
    Reasons::kDeviceBypassNeg,
    Reasons::kSwPrefetchL2L3Neg,
    Reasons::kIncompleteLifecycle,
    Reasons::kStaleInvalidated,
    Reasons::kMultiFollowerAmbiguous,
    Reasons::kMixedOwnershipUnresolved,
    Reasons::kBoundary,
    Reasons::kUnsupportedIsa,
};

struct Label
{
    Membership membership = Membership::Unknown;
    Certainty certainty = Certainty::Unclassified;
    const char *reason = nullptr;
};

// Evidence for one retired load uop / one physical request fragment. Counts
// come from the closed native registry; the strict parent-witness gate in the
// trace copy layer is the only producer of late_*_parent evidence.
struct FragmentEvidence
{
    // Lifecycle ledger.
    bool resolved = true;
    bool identity_known = true;
    bool issuance_closed = true;
    uint32_t admissions = 1;
    uint32_t responses = 1;
    bool repeated_response = false;
    uint32_t attempts = 1;
    uint32_t completions = 1;
    uint32_t hierarchy_requests = 0;

    // Terminals.
    bool store_forward_full = false;
    bool store_forward_partial = false;
    bool device_bypass = false;
    bool bypass_identity_known = true;
    bool bypass_closed = true;
    bool software_prefetch = false;

    // Per-level (0=L1D, 1=private L2, 2=LLC) own-request outcomes.
    std::array<uint32_t, 3> accesses{};
    std::array<uint32_t, 3> hits{};
    std::array<uint32_t, 3> tag_misses{};
    std::array<uint32_t, 3> permission_upgrades{};
    std::array<uint32_t, 3> merged_misses{};
    std::array<uint32_t, 3> remote_supplies{};

    // Sequencer-coalesced follower (no own hierarchy lookup at any level).
    bool coalesced_follower = false;
    bool coalesced_resident = false;
    uint32_t coalesced = 0;

    // Data_Stale service or explicit invalidation observed for this service.
    bool stale = false;

    // Accepted unique late-prefetch parent witness (registry gate passed).
    bool late_l2_parent = false;
    bool late_l1_parent = false;
    int parent_llc_outcome = -1; // TaoTraceNativeCacheOutcome 0..4
    bool parent_response_ambiguous = false;

    // Rejected oracle generations: see-parent estimation is forbidden.
    bool parent_reject_uniqueness = false;
    bool parent_reject_stale = false;
};

struct FragmentResult
{
    std::array<Label, Events> labels{};
    // Explicit failure visibility (TR-4.2), even when macro OR later closes.
    uint32_t fail_incomplete = 0;
    uint32_t fail_stale = 0;
    uint32_t fail_multi_follower = 0;
    uint32_t fail_mixed_ownership = 0;
};

struct CaseInput
{
    bool physical_split = false;
    // Macro-level exclusions handled by the functional ledger.
    bool unsupported = false; // gather/scatter/maskmov/rep/predicated off
    bool boundary = false;    // leading/crossing/warmup/scope boundary
    std::vector<FragmentEvidence> fragments;
};

struct MacroResult
{
    std::array<Label, Events> labels{};
    uint32_t fail_incomplete = 0;
    uint32_t fail_stale = 0;
    uint32_t fail_multi_follower = 0;
    uint32_t fail_mixed_ownership = 0;
    uint32_t fail_boundary = 0;
    uint32_t fail_unsupported = 0;
};

namespace detail
{

inline void
setLabel(Label &label, Membership membership, Certainty certainty,
         const char *reason)
{
    label.membership = membership;
    label.certainty = certainty;
    label.reason = reason;
}

inline void
setAll(std::array<Label, Events> &labels, Membership membership,
       Certainty certainty, const char *reason)
{
    for (Label &label : labels) setLabel(label, membership, certainty, reason);
}

enum class LevelKind
{
    None,
    Hit,
    TagMiss,
    Upgrade,
    MixedOwnership,
    Conflict,
};

inline LevelKind
classifyLevel(const FragmentEvidence &f, unsigned level)
{
    const uint32_t h = f.hits[level];
    const uint32_t m = f.tag_misses[level];
    const uint32_t u = f.permission_upgrades[level];
    const uint32_t x = f.merged_misses[level];
    const uint32_t r = f.remote_supplies[level];
    // Access conservation must be closed per fragment; LLCC-only outcome
    // types at private levels are conflicts as well.
    if (f.accesses[level] != h + m + u + x + r)
        return LevelKind::Conflict;
    if (level < 2 && (x || r)) return LevelKind::Conflict;
    if (!f.accesses[level]) return LevelKind::None;
    if (m && u) return LevelKind::MixedOwnership;
    if (h + m + u + x + r > 1) return LevelKind::Conflict;
    if (h) return LevelKind::Hit;
    if (m) return LevelKind::TagMiss;
    if (u) return LevelKind::Upgrade;
    // A single merged/remote outcome is legal only at LLC.
    return level == 2 ? LevelKind::TagMiss : LevelKind::Conflict;
}

// Parent-rule failure category returned to the fragment evaluator.
enum class ParentReject
{
    None,
    Uniqueness,
    Stale,
    Ownership,
};

// Resolves L2/L3 events (and FB for the L1D chain) from a unique parent.
inline ParentReject
applyParent(std::array<Label, Events> &labels, const FragmentEvidence &f,
            const char *reason)
{
    const bool l1_chain = f.late_l1_parent;
    if (f.parent_reject_uniqueness || f.parent_response_ambiguous) {
        setLabel(labels[L2Hit], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kMultiFollowerAmbiguous);
        setLabel(labels[L2Miss], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kMultiFollowerAmbiguous);
        setLabel(labels[L3Hit], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kMultiFollowerAmbiguous);
        setLabel(labels[L3Miss], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kMultiFollowerAmbiguous);
        if (l1_chain)
            setLabel(labels[FbHit], Membership::Unknown,
                     Certainty::Unclassified, Reasons::kMultiFollowerAmbiguous);
        return ParentReject::Uniqueness;
    }
    if (f.parent_reject_stale) {
        setLabel(labels[L2Hit], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kStaleInvalidated);
        setLabel(labels[L2Miss], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kStaleInvalidated);
        setLabel(labels[L3Hit], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kStaleInvalidated);
        setLabel(labels[L3Miss], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kStaleInvalidated);
        if (l1_chain)
            setLabel(labels[FbHit], Membership::Unknown,
                     Certainty::Unclassified, Reasons::kStaleInvalidated);
        return ParentReject::Stale;
    }
    // see-parent: estimated membership derived from the unique LLC outcome.
    bool l2_miss_member = false;
    bool l3_miss_member = false;
    bool unresolved = false;
    switch (f.parent_llc_outcome) {
      case 0: // LLC Hit
      case 4: // remote supply, treated as L3 hit by service mapping
        l2_miss_member = true;
        break;
      case 1: // LLC tag miss
      case 3: // merged miss, no resident LLC data
        l2_miss_member = true;
        l3_miss_member = true;
        break;
      default: // ownership-only outcome, no per-fragment attribution
        unresolved = true;
        break;
    }
    if (unresolved) {
        for (Event e : {L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Unknown,
                     Certainty::Unclassified,
                     Reasons::kMixedOwnershipUnresolved);
        if (l1_chain)
            setLabel(labels[FbHit], Membership::Unknown,
                     Certainty::Unclassified,
                     Reasons::kMixedOwnershipUnresolved);
        return ParentReject::Ownership;
    }
    setLabel(labels[L2Hit], Membership::Nonmember, Certainty::Estimated,
             reason);
    setLabel(labels[L2Miss], l2_miss_member ? Membership::Member
                                            : Membership::Nonmember,
             Certainty::Estimated, reason);
    setLabel(labels[L3Hit], l3_miss_member ? Membership::Nonmember
                                           : Membership::Member,
             Certainty::Estimated, reason);
    setLabel(labels[L3Miss], l3_miss_member ? Membership::Member
                                            : Membership::Nonmember,
             Certainty::Estimated, reason);
    if (l1_chain)
        setLabel(labels[FbHit], Membership::Member, Certainty::Estimated,
                 reason);
    return ParentReject::None;
}

inline FragmentResult
evaluateFragment(const FragmentEvidence &f)
{
    FragmentResult result;
    auto &labels = result.labels;

    if (f.software_prefetch) {
        setLabel(labels[L1Hit], Membership::Member, Certainty::Exact,
                 Reasons::kSwPrefetchPop);
        setLabel(labels[L1Miss], Membership::Nonmember, Certainty::Exact,
                 Reasons::kSwPrefetchPop);
        for (Event e : {FbHit, L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Nonmember, Certainty::Estimated,
                     Reasons::kSwPrefetchL2L3Neg);
        return result;
    }
    if (f.store_forward_full) {
        setAll(labels, Membership::Nonmember, Certainty::Estimated,
               Reasons::kStoreForwardNeg);
        return result;
    }
    if (f.store_forward_partial) {
        setAll(labels, Membership::Unknown, Certainty::Unclassified,
               Reasons::kIncompleteLifecycle);
        ++result.fail_incomplete;
        return result;
    }
    if (f.device_bypass) {
        if (f.resolved && f.identity_known && f.bypass_identity_known &&
            f.bypass_closed) {
            setAll(labels, Membership::Nonmember, Certainty::Estimated,
                   Reasons::kDeviceBypassNeg);
        } else {
            setAll(labels, Membership::Unknown, Certainty::Unclassified,
                   Reasons::kIncompleteLifecycle);
            ++result.fail_incomplete;
        }
        return result;
    }
    if (!f.resolved || !f.identity_known || !f.issuance_closed ||
        f.responses != f.admissions || f.responses == 0 ||
        f.repeated_response ||
        (f.coalesced_follower ? (f.attempts != 0 || f.completions != 1) :
                               (f.attempts != 1 || f.completions != 1))) {
        setAll(labels, Membership::Unknown, Certainty::Unclassified,
               Reasons::kIncompleteLifecycle);
        ++result.fail_incomplete;
        return result;
    }
    if (f.stale) {
        setAll(labels, Membership::Unknown, Certainty::Unclassified,
               Reasons::kStaleInvalidated);
        ++result.fail_stale;
        return result;
    }

    // Sequencer-coalesced follower: no own hierarchy lookup. FB membership is
    // estimated; no parent miss is inherited from the initiating request.
    if (f.coalesced_follower || (f.coalesced && f.hierarchy_requests == 0)) {
        setLabel(labels[L1Hit], f.coalesced_resident ? Membership::Member :
                                                    Membership::Nonmember,
                 Certainty::Estimated,
                 Reasons::kFbCoalesced);
        setLabel(labels[L1Miss], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kFbCoalesced);
        setLabel(labels[FbHit], f.coalesced_resident ? Membership::Nonmember :
                                                    Membership::Member,
                 Certainty::Estimated,
                 Reasons::kFbCoalesced);
        for (Event e : {L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Nonmember, Certainty::Estimated,
                     Reasons::kFbCoalesced);
        return result;
    }

    const LevelKind l0 = classifyLevel(f, 0);
    const bool late_parent = f.late_l2_parent || f.late_l1_parent;
    const char *parent_reason = f.late_l1_parent
        ? Reasons::kLateL1PrefetchFollower
        : Reasons::kLateL2PrefetchFollower;

    // L1D level.
    switch (l0) {
      case LevelKind::Hit:
        setLabel(labels[L1Hit], Membership::Member, Certainty::Exact,
                 Reasons::kDirectL1Hit);
        setLabel(labels[L1Miss], Membership::Nonmember, Certainty::Exact,
                 Reasons::kDirectL1Hit);
        setLabel(labels[FbHit], Membership::Nonmember, Certainty::Exact,
                 Reasons::kDirectL1Hit);
        break;
      case LevelKind::TagMiss:
        setLabel(labels[L1Hit], Membership::Nonmember, Certainty::Exact,
                 Reasons::kDirectL1Miss);
        setLabel(labels[L1Miss], Membership::Member, Certainty::Exact,
                 Reasons::kDirectL1Miss);
        // Own fill initiator at L1D unless a pending L1D prefetch fill is
        // proven by the G0 chain.
        setLabel(labels[FbHit],
                 f.late_l1_parent ? Membership::Unknown
                                  : Membership::Nonmember,
                 f.late_l1_parent ? Certainty::Unclassified : Certainty::Exact,
                 f.late_l1_parent ? Reasons::kIncompleteLifecycle :
                                    Reasons::kDirectL1Miss);
        break;
      case LevelKind::Upgrade:
        // Resident data with an ownership change is not a data miss. The
        // resident-data L1 hit reading is the same observation; load identity
        // upgrades are lock RMW reads that L1_HIT includes regardless.
        setLabel(labels[L1Hit], Membership::Member, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        setLabel(labels[L1Miss], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        setLabel(labels[FbHit], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        // Lower permission traffic does not change this load's resident
        // L1 data service. Keep those lookups in request diagnostics only.
        for (Event e : {L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Nonmember, Certainty::Estimated,
                     Reasons::kPermissionUpgradeNeg);
        return result;
      case LevelKind::MixedOwnership:
        for (Event e : {L1Hit, L1Miss, FbHit, L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Unknown,
                     Certainty::Unclassified,
                     Reasons::kMixedOwnershipUnresolved);
        ++result.fail_mixed_ownership;
        return result;
      default:
        for (Event e : {L1Hit, L1Miss, FbHit, L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Unknown,
                     Certainty::Unclassified,
                     Reasons::kIncompleteLifecycle);
        ++result.fail_incomplete;
        return result;
    }

    // Complete resident L1 hit with zero lower traffic: all lower events are
    // exact negatives.
    if ((l0 == LevelKind::Hit || l0 == LevelKind::Upgrade) &&
        f.accesses[1] == 0 && f.accesses[2] == 0 && !late_parent) {
        for (Event e : {L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Nonmember,
                     l0 == LevelKind::Hit ? Certainty::Exact : Certainty::Estimated,
                     l0 == LevelKind::Hit ? Reasons::kCompleteL1HitLowerNeg :
                                           Reasons::kPermissionUpgradeNeg);
        return result;
    }

    // Lower levels.
    const LevelKind l1 = classifyLevel(f, 1);
    if (l1 == LevelKind::Conflict ||
        (l1 == LevelKind::MixedOwnership)) {
        for (Event e : {L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Unknown, Certainty::Unclassified,
                     l1 == LevelKind::MixedOwnership
                         ? Reasons::kMixedOwnershipUnresolved
                         : Reasons::kIncompleteLifecycle);
        if (l1 == LevelKind::MixedOwnership) ++result.fail_mixed_ownership;
        else ++result.fail_incomplete;
        if (f.late_l1_parent)
            setLabel(labels[FbHit], Membership::Unknown,
                     Certainty::Unclassified,
                     l1 == LevelKind::MixedOwnership
                         ? Reasons::kMixedOwnershipUnresolved
                         : Reasons::kIncompleteLifecycle);
        return result;
    }
    if (l1 == LevelKind::None) {
        if (late_parent) {
            // The G1 chain leaves FB a direct L1 negative; G0 fills it from
            // the parent rule.
            if (f.late_l2_parent)
                setLabel(labels[FbHit], Membership::Nonmember,
                         Certainty::Exact, Reasons::kDirectL1Miss);
            switch (applyParent(labels, f, parent_reason)) {
              case ParentReject::Uniqueness:
                ++result.fail_multi_follower;
                break;
              case ParentReject::Stale:
                ++result.fail_stale;
                break;
              case ParentReject::Ownership:
                ++result.fail_mixed_ownership;
                break;
              case ParentReject::None:
                break;
            }
            return result;
        }
        // A resolved L1 miss with no observable L2 service and no proven
        // parent cannot be classified: fail closed.
        for (Event e : {L2Hit, L2Miss, L3Hit, L3Miss})
            setLabel(labels[e], Membership::Unknown,
                     Certainty::Unclassified, Reasons::kIncompleteLifecycle);
        ++result.fail_incomplete;
        if (f.late_l1_parent)
            setLabel(labels[FbHit], Membership::Unknown,
                     Certainty::Unclassified, Reasons::kIncompleteLifecycle);
        return result;
    }

    // Own L2 outcome observed.
    if (l1 == LevelKind::Hit) {
        setLabel(labels[L2Hit], Membership::Member, Certainty::Exact,
                 Reasons::kDirectL2Hit);
        setLabel(labels[L2Miss], Membership::Nonmember, Certainty::Exact,
                 Reasons::kDirectL2Hit);
        if (f.accesses[2] == 0) {
            // Closed complement: resident L2 data, no LLC lookup.
            setLabel(labels[L3Hit], Membership::Nonmember, Certainty::Exact,
                     Reasons::kDirectL2Hit);
            setLabel(labels[L3Miss], Membership::Nonmember, Certainty::Exact,
                     Reasons::kDirectL2Hit);
            return result;
        }
    } else if (l1 == LevelKind::TagMiss) {
        setLabel(labels[L2Hit], Membership::Nonmember, Certainty::Exact,
                 Reasons::kDirectL2Miss);
        setLabel(labels[L2Miss], Membership::Member, Certainty::Exact,
                 Reasons::kDirectL2Miss);
    } else { // Upgrade: resident data, ownership is not a data miss.
        setLabel(labels[L2Hit], Membership::Member, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        setLabel(labels[L2Miss], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        setLabel(labels[L3Hit], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        setLabel(labels[L3Miss], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        return result;
    }

    const LevelKind l2 = classifyLevel(f, 2);
    if (l2 == LevelKind::None && late_parent && l1 == LevelKind::TagMiss) {
        const auto own_hit=labels[L2Hit], own_miss=labels[L2Miss];
        const auto reject=applyParent(labels,f,parent_reason);
        labels[L2Hit]=own_hit;labels[L2Miss]=own_miss;
        result.fail_multi_follower+=reject==ParentReject::Uniqueness;
        result.fail_stale+=reject==ParentReject::Stale;
        result.fail_mixed_ownership+=reject==ParentReject::Ownership;
        return result;
    }
    if (l2 == LevelKind::Hit) {
        setLabel(labels[L3Hit], Membership::Member, Certainty::Exact,
                 Reasons::kDirectL3Hit);
        setLabel(labels[L3Miss], Membership::Nonmember, Certainty::Exact,
                 Reasons::kDirectL3Hit);
    } else if (l2 == LevelKind::TagMiss) {
        if (f.remote_supplies[2] && !f.tag_misses[2]) {
            setLabel(labels[L3Hit], Membership::Member, Certainty::Estimated,
                     Reasons::kRemoteSupply);
            setLabel(labels[L3Miss], Membership::Nonmember,
                     Certainty::Estimated, Reasons::kRemoteSupply);
        } else if (f.merged_misses[2] && !f.tag_misses[2]) {
            setLabel(labels[L3Hit], Membership::Nonmember,
                     Certainty::Estimated, Reasons::kMergedMiss);
            setLabel(labels[L3Miss], Membership::Member,
                     Certainty::Estimated, Reasons::kMergedMiss);
        } else if (f.tag_misses[2]) {
            setLabel(labels[L3Hit], Membership::Nonmember, Certainty::Exact,
                     Reasons::kDirectL3Miss);
            setLabel(labels[L3Miss], Membership::Member, Certainty::Exact,
                     Reasons::kDirectL3Miss);
        } else {
            setLabel(labels[L3Hit], Membership::Unknown,
                     Certainty::Unclassified,
                     Reasons::kMixedOwnershipUnresolved);
            setLabel(labels[L3Miss], Membership::Unknown,
                     Certainty::Unclassified,
                     Reasons::kMixedOwnershipUnresolved);
            ++result.fail_mixed_ownership;
        }
    } else if (l2 == LevelKind::Upgrade) {
        // Ownership traffic to the LLC carries resident data; no LLC data
        // miss for this load.
        setLabel(labels[L3Hit], Membership::Member, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
        setLabel(labels[L3Miss], Membership::Nonmember, Certainty::Estimated,
                 Reasons::kPermissionUpgradeNeg);
    } else if (l2 == LevelKind::MixedOwnership) {
        setLabel(labels[L3Hit], Membership::Unknown,
                 Certainty::Unclassified,
                 Reasons::kMixedOwnershipUnresolved);
        setLabel(labels[L3Miss], Membership::Unknown,
                 Certainty::Unclassified,
                 Reasons::kMixedOwnershipUnresolved);
        ++result.fail_mixed_ownership;
    } else {
        setLabel(labels[L3Hit], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kIncompleteLifecycle);
        setLabel(labels[L3Miss], Membership::Unknown,
                 Certainty::Unclassified, Reasons::kIncompleteLifecycle);
        ++result.fail_incomplete;
    }
    return result;
}

// Failure precedence when several fragments stay unclassified for an event.
inline unsigned
failureRank(const char *reason)
{
    if (!reason) return 0;
    if (reason == Reasons::kStaleInvalidated) return 5;
    if (reason == Reasons::kMultiFollowerAmbiguous) return 4;
    if (reason == Reasons::kMixedOwnershipUnresolved) return 3;
    if (reason == Reasons::kBoundary) return 2;
    if (reason == Reasons::kUnsupportedIsa) return 2;
    if (reason == Reasons::kIncompleteLifecycle) return 1;
    return 0;
}

inline const char *
strongerFailure(const char *a, const char *b)
{
    return failureRank(b) > failureRank(a) ? b : a;
}

} // namespace detail

inline MacroResult
evaluate(const CaseInput &input)
{
    MacroResult macro;
    if (input.unsupported) {
        for (Label &label : macro.labels)
            detail::setLabel(label, Membership::Unknown,
                             Certainty::Unclassified, Reasons::kUnsupportedIsa);
        macro.fail_unsupported = 1;
        return macro;
    }
    if (input.boundary) {
        for (Label &label : macro.labels)
            detail::setLabel(label, Membership::Unknown,
                             Certainty::Unclassified, Reasons::kBoundary);
        macro.fail_boundary = 1;
        return macro;
    }
    if (input.fragments.empty()) {
        for (Label &label : macro.labels)
            detail::setLabel(label, Membership::Unknown,
                             Certainty::Unclassified,
                             Reasons::kIncompleteLifecycle);
        macro.fail_incomplete = 1;
        return macro;
    }

    std::vector<FragmentResult> fragments;
    fragments.reserve(input.fragments.size());
    for (const FragmentEvidence &f : input.fragments) {
        fragments.push_back(detail::evaluateFragment(f));
        const FragmentResult &fr = fragments.back();
        macro.fail_incomplete += fr.fail_incomplete;
        macro.fail_stale += fr.fail_stale;
        macro.fail_multi_follower += fr.fail_multi_follower;
        macro.fail_mixed_ownership += fr.fail_mixed_ownership;
    }

    for (unsigned e = 0; e < Events; ++e) {
        const Label *exact_member = nullptr;
        const Label *estimated_member = nullptr;
        const Label *unknown = nullptr;
        const Label *first_nonmember = nullptr;
        bool exact_nonmember = true;
        bool same_reason = true;
        for (const FragmentResult &fr : fragments) {
            const Label &l = fr.labels[e];
            switch (l.membership) {
              case Membership::Member:
                if (l.certainty == Certainty::Exact) exact_member = &l;
                else estimated_member = &l;
                break;
              case Membership::Unknown:
                unknown = &l;
                break;
              case Membership::Nonmember:
                if (!first_nonmember) first_nonmember = &l;
                if (l.certainty != Certainty::Exact) exact_nonmember = false;
                break;
            }
            if (first_nonmember && l.membership == Membership::Nonmember &&
                l.reason != first_nonmember->reason)
                same_reason = false;
        }
        Label &out = macro.labels[e];
        if (exact_member) {
            out = *exact_member;
        } else if (estimated_member) {
            out = *estimated_member;
        } else if (unknown) {
            // Highest-precedence explicit failure wins.
            const char *reason = unknown->reason;
            for (const FragmentResult &fr : fragments) {
                const Label &l = fr.labels[e];
                if (l.membership == Membership::Unknown)
                    reason = detail::strongerFailure(reason, l.reason);
            }
            detail::setLabel(out, Membership::Unknown,
                             Certainty::Unclassified, reason);
        } else if (first_nonmember) {
            const bool mixed = fragments.size() > 1 && !same_reason;
            // R-MIXED-FRAGMENT is an estimated reason in the membership
            // contract: mixed per-fragment reasons weaken otherwise exact
            // negatives to the weaker certainty.
            const char *reason = mixed
                ? Reasons::kMixedFragment
                : first_nonmember->reason;
            detail::setLabel(out, Membership::Nonmember,
                             (!mixed && exact_nonmember) ? Certainty::Exact
                                                         : Certainty::Estimated,
                             reason);
        }
        // Intel's descriptions do not define physical split-fragment
        // aggregation; L2 also lacks the explicit any-uop wording.
        if (out.membership==Membership::Member &&
            out.certainty==Certainty::Exact && input.fragments.size()>1 &&
            (input.physical_split || e==L2Hit || e==L2Miss)) {
            out.certainty=Certainty::Estimated;
            out.reason=Reasons::kMixedFragment;
        }
    }
    return macro;
}

} // namespace retired_membership
} // namespace gem5

#endif
