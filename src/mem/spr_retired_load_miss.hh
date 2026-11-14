// Shared, observer-only SPR interpretation. No simulator clocks or trace input.
#ifndef SPR_RETIRED_LOAD_MISS_HH
#define SPR_RETIRED_LOAD_MISS_HH
#include <array>
#include <cstdint>

namespace spr_retired {
inline constexpr const char *Policy = "spr-retired-load-miss-v2";
struct LifecycleDiagnostics {
    uint64_t created=0, completed=0, replaced=0, canceled=0;
    uint64_t retained=0, released=0, live=0, peak=0, late_lookup=0;
    std::array<uint64_t,8> unclassified_alias{};
};
inline LifecycleDiagnostics &lifecycleDiagnostics() {
    static LifecycleDiagnostics diagnostics;
    return diagnostics;
}
enum class Level : uint8_t {
    Undecided, Resident, Miss, Permission, PendingFill, Excluded, Remote
};
enum class Service : uint8_t {
    Data, Forwarded, PartialForward, DemandFollower, PrefetchFollower,
    Device, SoftwarePrefetch
};
enum class Supply : uint8_t {
    Undecided, L1, L2, Llc, FillBuffer, Dram, Remote, Device, Forwarded
};
// A fragment is updated at lookup/merge edges, finalized at data completion.
struct Result {
    std::array<Level, 3> level{};
    Service service = Service::Data;
    Supply supply = Supply::Undecided;
    uint8_t miss_mask = 0, estimated_mask = 0, known_mask = 0;
    uint8_t sticky_miss = 0;
    bool stale = false, ready = false, split = false, retried = false;
    bool multiple_consumers = false;
    bool preparePermissionGuard = false;
};
static_assert(sizeof(Result) <= 16, "fragment result must remain fixed and small");

inline void lookup(Result &r, unsigned level, Level value) {
    if (level >= 3 || r.ready) return;
    if (level == 0 && r.preparePermissionGuard && value == Level::PendingFill)
        return;
    if (value == Level::Miss || value == Level::PendingFill)
        r.sticky_miss |= 1u << level;
    r.level[level] = value;
}

inline Result convert(Result r) {
    r.miss_mask = r.known_mask = r.estimated_mask = 0;
    if (r.service == Service::Forwarded || r.service == Service::Device ||
        r.service == Service::SoftwarePrefetch) {
        r.known_mask = r.estimated_mask = 7;
        r.ready = true;
        return r;
    }
    // A resident upper service or an L1 FB excludes unrelated lower traffic.
    bool lower_excluded = false, estimated_exclusion = false;
    for (unsigned l = 0; l < 3; ++l) {
        const uint8_t bit = 1u << l;
        const auto value = r.level[l];
        if (lower_excluded) {
            r.known_mask |= bit;
            if (estimated_exclusion) r.estimated_mask |= bit;
            continue;
        }
        switch (value) {
          case Level::Resident:
            r.known_mask |= bit;
            lower_excluded = true;
            break;
          case Level::Permission:
            r.known_mask |= bit;
            r.estimated_mask |= bit;
            lower_excluded = estimated_exclusion = true;
            break;
          case Level::Miss:
            r.known_mask |= bit; r.miss_mask |= bit;
            break;
          case Level::PendingFill:
            r.known_mask |= bit; r.miss_mask |= bit; r.estimated_mask |= bit;
            if (l == 0) lower_excluded = estimated_exclusion = true;
            break;
          case Level::Remote:
            r.known_mask |= bit; r.estimated_mask |= bit;
            lower_excluded = estimated_exclusion = true;
            break;
          case Level::Excluded:
            r.known_mask |= bit; r.estimated_mask |= bit;
            break;
          case Level::Undecided:
            break; // Missing callbacks remain unknown, never implicit hits.
        }
    }
    r.miss_mask |= r.sticky_miss;
    r.known_mask |= r.sticky_miss;
    if (r.split) r.estimated_mask |= r.known_mask;
    if (r.retried) r.estimated_mask |= r.known_mask;
    if (r.level[1] == Level::PendingFill ||
        r.service == Service::PrefetchFollower ||
        r.service == Service::DemandFollower)
        r.estimated_mask |= r.known_mask;
    r.ready = r.known_mask == 7;
    return r;
}

// Constant-size OR accumulator: positive evidence survives missing siblings.
struct Aggregate {
    uint8_t miss_mask = 0, estimated_mask = 0, unknown_mask = 0;
    uint16_t path_mask = 0;
    inline static constexpr const char *Paths[] = {
        "data", "forwarded", "partial_forward", "demand_follower",
        "prefetch_follower", "device", "software_prefetch",
        "l1_pending_fill", "l2_pending_fill", "llc_pending_fill",
        "permission", "remote", "stale", "split_or_retry", "multiple_consumers"
    };
    bool seen = false;
    void add(const Result &r) {
        seen = true;
        path_mask |= uint16_t(1u << unsigned(r.service));
        for (unsigned l=0; l<3; ++l) {
            if (r.level[l]==Level::PendingFill) path_mask |= 1u << (7+l);
            if (r.level[l]==Level::Permission) path_mask |= 1u << 10;
            if (r.level[l]==Level::Remote) path_mask |= 1u << 11;
        }
        if (r.stale) path_mask |= 1u << 12;
        if (r.split || r.retried) path_mask |= 1u << 13;
        if (r.multiple_consumers) path_mask |= 1u << 14;
        miss_mask |= r.miss_mask;
        estimated_mask |= r.estimated_mask;
        unknown_mask |= uint8_t(7 & ~r.known_mask);
    }
    void add(const Aggregate &r) {
        if (!r.seen) return;
        seen = true; miss_mask |= r.miss_mask; path_mask |= r.path_mask;
        estimated_mask |= r.estimated_mask; unknown_mask |= r.unknown_mask;
    }
    uint8_t unknown() const {
        return seen ? uint8_t(unknown_mask & ~miss_mask) : 7;
    }
};

inline Result ordinary(unsigned misses) {
    Result r;
    for (unsigned l = 0; l < 3; ++l)
        r.level[l] = (misses & (1u << l)) ? Level::Miss : Level::Resident;
    return r;
}
}
#endif
