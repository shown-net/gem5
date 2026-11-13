/* Oracle-only architectural retirement accounting. No timing/FST input. */
#ifndef __CPU_O3_PROBE_PERF_RETIREMENT_HH__
#define __CPU_O3_PROBE_PERF_RETIREMENT_HH__
#define TAOTRACE_RETIREMENT_ROM_BOUNDARIES 1
#define TAOTRACE_RETIREMENT_INDEPENDENT_EVENTS 1

#include <array>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mem/prefetch_lineage_oracle.hh"
#include "mem/retired_cache_membership.hh"
#include "mem/spr_retired_load_miss.hh"
#include "mem/spr_prepare_service.hh"

namespace gem5 { namespace o3 { namespace retirement {

enum Source : unsigned {
    Unknown=1, L1=2, Miss=4, FillBuffer=8, Forwarded=16,
    L2Hit=32, L2Miss=64, L3Hit=128, L3Miss=256, L2Unknown=512, L3Unknown=1024,
    SequencerShared=2048, OtherTerminal=4096,
    PermissionUpgrade=8192, RemoteSupply=16384
};

inline bool knownRomEntry(uint64_t micro_pc, uint64_t rom_bit,
                          std::initializer_list<uint64_t> entries)
{
    if (!(micro_pc & rom_bit)) return false;
    const auto offset=micro_pc & ~rom_bit;
    for (const auto entry : entries) if (entry==offset) return true;
    return false;
}

struct NativeMemberDiagnostic {
    uint64_t sequence=0, pc=0, micro_pc=0, next_micro_pc=0, macroop_identity=0;
    bool first=false, last=false, micro=false, macroop_present=false;
    std::string name, macroop_name, disasm;
};

inline void writeJsonString(std::ostream &out, const std::string &value)
{
    static constexpr const char digits[]="0123456789abcdef";
    out << '"';
    for (unsigned char c : value) {
        if (c=='"' || c=='\\') out << '\\' << c;
        else if (c<32) out << "\\u00" << digits[c>>4] << digits[c&15];
        else out << c;
    }
    out << '"';
}

struct DebugPrefixMember {
    bool micro=false, first=false, debug_control=false;
};

inline bool debugControlName(const std::string &name)
{
    return name=="warn" || name=="warn_once" || name=="hack" ||
           name=="hack_once" || name=="inform" || name=="inform_once";
}

// Only simulator debug controls may be absent before the first committed
// member. Never infer a start from an empty ledger or a mnemonic alone.
template<class Fetch>
inline bool debugOnlyPrefix(uint64_t micro_pc, Fetch fetch)
{
    if (!micro_pc || micro_pc>64) return false;
    for (uint64_t i=0;i<micro_pc;++i) {
        const auto prefix=fetch(i);
        if (!prefix.micro || !prefix.debug_control || (i==0 && !prefix.first))
            return false;
    }
    return true;
}

// Complete sparse functional facts; bounded human-readable samples elsewhere
// are diagnostics only. No clocks, cache outcomes or PMU values are emitted.
class BoundaryStream {
    std::ofstream out_;
    std::string directory_;
    unsigned core_=0;
    uint64_t count_=0, previous_=0;
  public:
    bool opened() const { return out_.is_open(); }
    void open(const std::string &directory,unsigned core) {
        if (opened()) return;
        directory_=directory;core_=core;
        out_.open(directory+"/macro-boundary-core"+std::to_string(core)+".jsonl");
        if (!out_) throw std::runtime_error("cannot open functional macro boundaries");
    }
    void record(uint64_t ordinal,uint32_t context,uint64_t pc,bool starts,
                bool rom,bool debug,bool had_open) {
        if (!opened() || (count_ && ordinal<=previous_))
            throw std::runtime_error("unordered functional macro boundary");
        out_ << "{\"schema\":\"fastsim-functional-macro-boundary-v1\",\"core_id\":" << core_
             << ",\"record_ordinal\":" << ordinal << ",\"context_id\":" << context
             << ",\"pc\":" << pc << ",\"starts_macro\":" << (starts?"true":"false")
             << ",\"rom_entry\":" << (rom?"true":"false")
             << ",\"debug_prefix\":" << (debug?"true":"false")
             << ",\"had_open_macro\":" << (had_open?"true":"false") << "}\n";
        if (!out_) throw std::runtime_error("failed functional macro boundary write");
        previous_=ordinal;++count_;
    }
    void finish(const std::string &source,uint64_t records,uint64_t begin,bool complete) {
        if (!opened()) throw std::runtime_error("functional macro boundary stream missing");
        out_.flush();
        if (!out_) throw std::runtime_error("failed functional macro boundary flush");
        out_.close();
        std::ofstream meta(directory_+"/macro-boundary-metadata-core"+std::to_string(core_)+".json");
        meta << "{\"schema\":\"fastsim-functional-macro-boundary-metadata-v1\",\"core_id\":" << core_
             << ",\"source_file\":";writeJsonString(meta,source);
        meta << ",\"source_record_count\":" << records << ",\"measurement_begin_ordinal\":" << begin
             << ",\"event_count\":" << count_ << ",\"dropped_events\":0,\"complete\":"
             << (complete?"true":"false")
             << ",\"incomplete_scope\":\"measured-functional-members-v2\","
                "\"event_file\":\"macro-boundary-core" << core_ << ".jsonl\"}\n";
        meta.close();
        if (!meta) throw std::runtime_error("failed functional macro boundary metadata write");
    }
};

inline void writeNativeMemberDiagnostic(std::ostream &out,
                                         const NativeMemberDiagnostic &m)
{
    out << "{\"inst_seq_num\":" << m.sequence << ",\"pc\":" << m.pc
        << ",\"micro_pc\":" << m.micro_pc << ",\"next_micro_pc\":" << m.next_micro_pc
        << ",\"first_micro\":" << (m.first ? "true" : "false")
        << ",\"last_micro\":" << (m.last ? "true" : "false")
        << ",\"is_microop\":" << (m.micro ? "true" : "false")
        << ",\"macroop_present\":" << (m.macroop_present ? "true" : "false")
        << ",\"macroop_identity\":" << m.macroop_identity << ",\"name\":";
    writeJsonString(out,m.name);
    out << ",\"macroop_name\":"; writeJsonString(out,m.macroop_name);
    out << ",\"disasm\":"; writeJsonString(out,m.disasm);
    out << '}';
}

// Exact independent registry facts, never the functional path-class or MSHR
// predictor. A terminal reason mask does not identify partial forwarding bytes.
struct NativeFacts {
    spr_retired::Aggregate sprMiss;
    uint64_t spr_incomplete_completions=0, spr_failed_request=0, spr_failed_address=0;
    unsigned spr_missing_levels=0;
    unsigned spr_failed_alias_category=255;
    unsigned spr_failed_prepare_status=255;
    unsigned spr_failed_prepare_invalidation=0;
    std::array<spr_retired::Level,3> spr_failed_levels{};
    std::vector<retired_membership::FragmentEvidence> membership_fragments;
    bool resolved=false, store_forward=false, other_terminal=false;
    uint64_t admissions=0, responses=0, coalesced=0, hierarchy_requests=0;
    uint64_t sequencer_shared=0;
    uint64_t external_hits=0, external_misses=0;
    uint64_t accesses=0, hits=0, tag_misses=0;
    uint64_t permission_upgrades=0, merged_misses=0, remote_supplies=0;
    uint64_t l2_accesses=0,l2_hits=0,l2_tag_misses=0,l2_other=0;
    uint64_t l3_accesses=0,l3_hits=0,l3_tag_misses=0,l3_other=0;
    uint64_t l3_merged_misses=0,l3_remote_supplies=0,terminal_reason_mask=0;
    uint64_t l2_permission_upgrades=0,l3_permission_upgrades=0;
    uint64_t l1_late_prefetches=0,l2_late_prefetches=0;
    std::array<uint64_t,5> l2_prefetch_parent_llc{};
    std::array<uint64_t,3> l2_prefetch_parent_sender{},l2_prefetch_parent_response{};
    int l2_prefetch_parent_sender_id=-1;
    // Independent L1D-begun prefetch chain; populated only behind the same
    // attempt-level lifecycle gate in tao_trace.cc.
    std::array<uint64_t,5> l1_prefetch_parent_llc{};
    std::array<uint64_t,3> l1_prefetch_parent_sender{},l1_prefetch_parent_response{};
    int l1_prefetch_parent_sender_id=-1;
    // Task 4 diagnosed lifecycle facts (observer diagnostics; the Task 5
    // membership engine consumes them). Reject bit 1 = uniqueness failure
    // (multi follower/lookup or repeated response), bit 2 = stale/invalidate.
    bool stale_serviced=false, repeated_response=false;
    uint64_t hierarchy_attempts=0, hierarchy_completions=0;
    uint64_t hierarchy_attempt_id=0, hierarchy_completion_attempt_id=0;
    uint8_t l1_parent_reject_mask=0, l2_parent_reject_mask=0;
    // Each mask is reduced from completed physical-fragment facts, not from
    // the shared fill supplier. Bits: unknown, hit, miss, not accessed.
    uint8_t own_l2_mask=0, own_l3_mask=0;
    uint8_t l1_final_mask=0;
    uint64_t service_fact_count=0;
    uint64_t service_expected=0;
    bool service_identity_ambiguous=false;
};

template<class Facts>
inline void addServiceFacts(NativeFacts &native, const Facts &facts)
{
    for (const auto &fact : facts) {
        native.own_l2_mask |= fact.ownL2Mask;
        native.own_l3_mask |= fact.ownL3Mask;
        ++native.service_fact_count;
    }
}

inline unsigned classify(const NativeFacts &n)
{
    const unsigned forwarding=n.store_forward ? unsigned(Forwarded) : 0u;
    if (!n.resolved || n.other_terminal || n.store_forward ||
        !n.responses || n.responses!=n.admissions ||
        n.responses!=n.external_hits+n.external_misses ||
        n.responses!=n.hierarchy_requests+n.coalesced ||
        n.accesses!=n.hierarchy_requests ||
        n.accesses!=n.hits+n.tag_misses || n.permission_upgrades ||
        n.merged_misses || n.remote_supplies ||
        n.external_hits<n.hits || n.external_misses<n.tag_misses)
        return Unknown|forwarding;
    // A coalesced response can share a resident L1 parent. Only followers of
    // a native controller miss are fill followers. This is a service mapping,
    // not certification of a hardware FB_HIT event.
    unsigned source=0;
    if (n.hits || n.external_hits>n.hits) source|=L1;
    if (n.tag_misses) source|=Miss;
    if (n.external_misses>n.tag_misses) source|=FillBuffer;
    return source ? source : Unknown;
}

// Full-address forwarding is recorded only by LSQUnit's full coverage path.
// Older issued/discarded packets remain request events but do not describe the
// data which completed this retired load. Hardware event mapping stays unverified.
inline unsigned classifyIndependent(const NativeFacts &n)
{
    // Candidate SPR L1_HIT membership for the LSQ full-coverage terminal.
    // This mapping is subject to instruction-form controlled validation.
    if (n.resolved && n.store_forward && !n.other_terminal)
        return L1|Forwarded;
    if (n.service_identity_ambiguous)
        return Unknown | (n.store_forward ? unsigned(Forwarded) : 0u);
    // No independent L1 lookup occurred for these followers. Their service
    // path is known, but the leader's cache outcome must not be inherited.
    if (n.resolved && n.other_terminal && !n.responses)
        return OtherTerminal | (n.store_forward ? unsigned(Forwarded) : 0u);
    // Reduce final physical-fragment results. Positive observations survive
    // another fragment's unknown result; discarded attempts are excluded.
    if (n.l1_final_mask) {
        unsigned source = 0;
        if (n.l1_final_mask & 1u) source |= Unknown;
        if (n.l1_final_mask & (1u<<1)) source |= L1;
        if (n.l1_final_mask & (1u<<2)) source |= Miss;
        if (n.l1_final_mask & (1u<<3)) source |= FillBuffer;
        if (n.sequencer_shared) source |= SequencerShared;
        if (n.l1_final_mask & (1u<<4)) source |= PermissionUpgrade;
        if (n.l1_final_mask & (1u<<5)) source |= RemoteSupply;
        if (n.other_terminal) source |= OtherTerminal;
        if (!n.resolved || n.responses != n.admissions) source |= Unknown;
        return source ? source : Unknown;
    }
    return Unknown | (n.store_forward ? unsigned(Forwarded) : 0u);
}

// Candidate retired-load events use this request's own lower-level lookups.
// A follower's shared supplier is a separate model fact, not another lookup.
inline unsigned classifyRetiredCache(const NativeFacts &n) {
    if (n.resolved && n.store_forward && !n.other_terminal) return 0;
    const auto upper=classifyIndependent(n);
    if (n.service_identity_ambiguous) return L2Unknown|L3Unknown;
    unsigned result=0;
    if (n.own_l2_mask & (1u<<1)) result|=L2Hit;
    if (n.own_l2_mask & (1u<<2)) result|=L2Miss;
    if (n.own_l3_mask & (1u<<1)) result|=L3Hit;
    if (n.own_l3_mask & (1u<<2)) result|=L3Miss;
    if (n.own_l2_mask & 1u) result|=L2Unknown|L3Unknown;
    if (n.own_l3_mask & 1u) result|=L3Unknown;

    // A service fact belongs to one physical fragment. Preserve positive
    // observations if another fragment or its response is unresolved.
    const bool incomplete = !n.resolved || n.other_terminal ||
        !n.responses || n.responses!=n.admissions ||
        (upper & (Unknown|SequencerShared|PermissionUpgrade|RemoteSupply));
    const bool missing_fact = n.service_fact_count < n.service_expected;
    if (incomplete || missing_fact) result|=L2Unknown|L3Unknown;
    return result;
}

#define TAOTRACE_RETIREMENT_COUNTS(X) \
    X(retired_macro_instructions) X(retired_uops) X(native_retired_uops) \
    X(perf_retired_loads) X(perf_retired_stores) X(perf_l1d_load_hits) \
    X(perf_l1d_load_misses) X(perf_l1d_load_fb_hits) \
    X(independent_l1_hits) X(independent_l1_misses) X(independent_fb_hits) \
    X(independent_l2_hits) X(independent_l2_misses) X(independent_l3_hits) X(independent_l3_misses)
#define TAOTRACE_RETIREMENT_COVERAGE(X) \
    X(classified_loads) X(unclassified_loads) X(incomplete_macros) \
    X(incomplete_uops) X(mixed_source_loads) X(forwarded_loads) \
    X(scope_crossing_macros) X(warmup_spanning_macros) \
    X(leading_incomplete_macros) X(unresolved_source_uops) X(unsupported_loads) \
    X(rom_entry_macros) X(rom_microcode_macros) X(boundary_discontinuities) \
    X(excluded_hardware_entry_macros) X(precise_fault_abandoned_macros) \
    X(independent_l1_hits_unknown) X(independent_l1_misses_unknown) \
    X(independent_fb_hits_unknown) X(independent_retired_loads_unknown) \
    X(independent_retired_stores_unknown) \
    X(all_history_incomplete_macros) X(all_history_incomplete_uops) \
    X(independent_l2_hits_unknown) X(independent_l2_misses_unknown) \
    X(independent_l3_hits_unknown) X(independent_l3_misses_unknown)

struct Counts {
    struct EventCounts {
        uint64_t count=0, unknown=0, nonmember=0;
        uint64_t exact_count=0, estimated_count=0;
        uint64_t exact_nonmember=0, estimated_nonmember=0;
        std::array<uint64_t,24> reasons{};
        void add(const retired_membership::Label &l) {
            using namespace retired_membership;
            if (l.membership==Membership::Unknown) ++unknown;
            else if (l.membership==Membership::Member) {
                ++count;
                (l.certainty==Certainty::Exact ? exact_count : estimated_count)++;
            } else {
                ++nonmember;
                (l.certainty==Certainty::Exact ? exact_nonmember : estimated_nonmember)++;
            }
            for (unsigned i=0;i<24;++i)
                if (l.reason && std::string(l.reason)==allReasons[i]) {
                    ++reasons[i];return;
                }
            throw std::runtime_error("retired cache reason absent");
        }
        EventCounts &operator+=(const EventCounts &o) {
            count+=o.count;unknown+=o.unknown;nonmember+=o.nonmember;
            exact_count+=o.exact_count;estimated_count+=o.estimated_count;
            exact_nonmember+=o.exact_nonmember;estimated_nonmember+=o.estimated_nonmember;
            for(unsigned i=0;i<24;++i) reasons[i]+=o.reasons[i];
            return *this;
        }
    };
    std::array<EventCounts,retired_membership::Events> membership{};
    std::array<EventCounts,3> spr_miss{};
    std::array<uint64_t,15> spr_paths{};
    std::array<uint64_t,6> membership_failures{};
#define DECLARE(name) uint64_t name=0;
    TAOTRACE_RETIREMENT_COUNTS(DECLARE)
    TAOTRACE_RETIREMENT_COVERAGE(DECLARE)
#undef DECLARE
    Counts &operator+=(const Counts &other) {
        for(unsigned i=0;i<3;++i) spr_miss[i]+=other.spr_miss[i];
        for(unsigned i=0;i<15;++i) spr_paths[i]+=other.spr_paths[i];
        for(unsigned i=0;i<retired_membership::Events;++i)
            membership[i]+=other.membership[i];
        for(unsigned i=0;i<6;++i) membership_failures[i]+=other.membership_failures[i];
#define ADD(name) name+=other.name;
        TAOTRACE_RETIREMENT_COUNTS(ADD)
        TAOTRACE_RETIREMENT_COVERAGE(ADD)
#undef ADD
        return *this;
    }
};

struct Scopes {
    Counts physical, user, kernel, workload_task, other_task;
};

inline void writeMembershipEvents(std::ostream &out,const Counts &counts)
{
    static const char *names[]={"retired_l1_load_hits","retired_l1_load_misses",
        "retired_load_fb_hits","retired_l2_load_hits","retired_l2_load_misses",
        "retired_l3_load_hits","retired_l3_load_misses"};
    for(unsigned i=0;i<retired_membership::Events;++i) {
        const auto &c=i==1 ? counts.spr_miss[0] :
            (i==4 ? counts.spr_miss[1] : (i==6 ? counts.spr_miss[2] :
             counts.membership[i]));
        out << ",\"" << names[i] << "\":{\"count\":" << c.count
            << ",\"unknown\":" << c.unknown << ",\"nonmember\":" << c.nonmember
            << ",\"exact_count\":" << c.exact_count
            << ",\"estimated_count\":" << c.estimated_count
            << ",\"exact_nonmember\":" << c.exact_nonmember
            << ",\"estimated_nonmember\":" << c.estimated_nonmember
            << ",\"reasons\":{";
        bool first=true;
        for(unsigned j=0;j<24;++j) if(c.reasons[j]) {
            if(!first) out << ',';
            first=false;
            out << '"' << retired_membership::allReasons[j] << "\":" << c.reasons[j];
        }
        out << "}}";
    }
}

inline void writeCounts(std::ostream &out, const Counts &c, bool available)
{
    out << "{\"available\":" << (available ? "true" : "false");
#define WRITE(name) out << ",\"" #name "\":"; \
    if (available) out << c.name; else out << "null";
    TAOTRACE_RETIREMENT_COUNTS(WRITE)
    out << ",\"coverage\":{\"unit_contract\":\"native-ISA-membership-not-hardware-certification\"";
    TAOTRACE_RETIREMENT_COVERAGE(WRITE)
#undef WRITE
    out << ",\"spr_service_paths\":{\"unit\":\"retired-load-macro-containing-path\","
        "\"policy\":\"" << spr_retired::Policy << "\",\"counts\":{";
    for(unsigned i=0;i<15;++i) {
        if(i) out << ',';
        out << '"' << spr_retired::Aggregate::Paths[i] << "\":";
        if(available) out << c.spr_paths[i]; else out << "null";
    }
    out << "}}";
    out << "}}";
}

#undef TAOTRACE_RETIREMENT_COUNTS
#undef TAOTRACE_RETIREMENT_COVERAGE

struct Member {
    uint32_t context=0;
    uint64_t sequence=0, owner=0, pc=0;
    bool first=true, last=true, read=false, write=false, user=true;
    bool measured=false, syscall=false, unsupported=false, software_prefetch=false;
    bool rom=false, rom_entry=false, hardware_entry=false;
};

class Ledger {
    struct Macro {
        uint64_t owner=0, pc=0, uops=0, unresolved=0, measured_functional_uops=0;
        bool user=true, read=false, write=false, warmup=false;
        bool leading=false, crossing=false, unsupported=false;
        bool rom_entry=false, rom=false, measured=false, hardware_entry=false, precise_fault=false;
        unsigned source=0;
        unsigned independent_source=0;
        retired_membership::MacroResult membership;
        spr_retired::Aggregate sprMiss;
        unsigned read_members=0;
        bool membership_set=false;
        std::vector<std::pair<uint32_t,uint64_t>> identities;
    };
    using Identity=std::pair<uint32_t,uint64_t>;
    std::map<uint32_t,std::shared_ptr<Macro>> open_;
    std::map<Identity,std::shared_ptr<Macro>> waiting_;
    std::vector<std::shared_ptr<Macro>> completed_loads_;
    std::vector<std::shared_ptr<Macro>> abandoned_;
    std::map<uint64_t,Scopes> owners_;
    bool last_hardware_entry_=false,last_precise_fault_abandonment_=false;
    struct NativeSample {
        uint32_t context=0; uint64_t sequence=0,owner=0,pc=0;
        unsigned membership=0; NativeFacts facts;
    };
    std::map<unsigned,uint64_t> native_unknown_masks_;
    uint64_t native_unknown_l1_late_prefetch_=0,native_unknown_l2_late_prefetch_=0;
    std::array<uint64_t,5> native_unknown_l2_prefetch_parent_llc_{};
    std::array<uint64_t,3> native_unknown_l2_prefetch_parent_sender_{},
                           native_unknown_l2_prefetch_parent_response_{};
    std::array<uint64_t,5> native_unknown_l1_prefetch_parent_llc_{};
    std::array<uint64_t,3> native_unknown_l1_prefetch_parent_sender_{},
                           native_unknown_l1_prefetch_parent_response_{};
    // Explicit fail-closed visibility (TR-4.2): uops kept unknown by each
    // diagnosed failure class. Saturated uniqueness/stale reject bits match
    // TaoTraceParentReject (1=uniqueness, 2=stale/invalidate).
    uint64_t native_unknown_stale_serviced_=0;
    uint64_t spr_incomplete_completions_=0;
    uint64_t native_unknown_repeated_response_=0;
    uint64_t native_unknown_lifecycle_incomplete_=0;
    uint64_t native_unknown_l1_parent_reject_uniqueness_=0;
    uint64_t native_unknown_l1_parent_reject_stale_=0;
    uint64_t native_unknown_l2_parent_reject_uniqueness_=0;
    uint64_t native_unknown_l2_parent_reject_stale_=0;
    std::vector<NativeSample> native_unknown_samples_;
    static void add(Scopes &s, const Counts &c, bool user) {
        s.physical+=c;
        (user ? s.user : s.kernel)+=c;
    }
    static void mergeMembership(Macro &m,
                                const retired_membership::MacroResult &next) {
        using namespace retired_membership;
        if (!m.membership_set) {
            m.membership=next;m.membership_set=true;return;
        }
        auto &r=m.membership;
        r.fail_incomplete+=next.fail_incomplete;r.fail_stale+=next.fail_stale;
        r.fail_multi_follower+=next.fail_multi_follower;
        r.fail_mixed_ownership+=next.fail_mixed_ownership;
        for(unsigned e=0;e<Events;++e) {
            auto &a=r.labels[e];const auto &b=next.labels[e];
            if (a.membership==Membership::Member && a.certainty==Certainty::Exact)
                continue;
            if (b.membership==Membership::Member &&
                (a.membership!=Membership::Member || b.certainty==Certainty::Exact)) {
                a=b;continue;
            }
            if (a.membership==Membership::Member) continue;
            if (a.membership==Membership::Unknown || b.membership==Membership::Unknown) {
                const auto reason=detail::strongerFailure(
                    a.membership==Membership::Unknown ? a.reason : nullptr,
                    b.membership==Membership::Unknown ? b.reason : nullptr);
                detail::setLabel(a,Membership::Unknown,Certainty::Unclassified,reason);
            } else if (a.reason!=b.reason || b.certainty==Certainty::Estimated) {
                detail::setLabel(a,Membership::Nonmember,Certainty::Estimated,
                                 Reasons::kMixedFragment);
            }
        }
        for(auto e : {retired_membership::L2Hit,retired_membership::L2Miss}) {
            auto &label=r.labels[e];
            if (label.membership==Membership::Member &&
                label.certainty==Certainty::Exact) {
                label.certainty=Certainty::Estimated;
                label.reason=Reasons::kMixedFragment;
            }
        }
    }
    static Counts sourceCounts(const Macro &m) {
        Counts c;
        using namespace retired_membership;
        auto result=m.membership;
        if (m.unsupported || m.leading || m.crossing || m.warmup) {
            CaseInput input;
            input.unsupported=m.unsupported;
            input.boundary=m.leading || m.crossing || m.warmup;
            result=evaluate(input);
        } else if (m.unresolved || !m.membership_set) {
            const auto unknown=evaluate(CaseInput{});
            if (!m.membership_set) result=unknown;
            else {
                for(unsigned e=0;e<Events;++e)
                    if(result.labels[e].membership!=Membership::Member)
                        result.labels[e]=unknown.labels[e];
                ++result.fail_incomplete;
            }
        }
        for(unsigned i=0;i<Events;++i) c.membership[i].add(result.labels[i]);
        const bool invalid=m.unsupported || m.leading || m.crossing || m.warmup;
        for(unsigned i=0;i<15;++i)
            c.spr_paths[i]=bool(m.sprMiss.path_mask&(1u<<i));
        const char *positive[]={Reasons::kDirectL1Miss,Reasons::kDirectL2Miss,
                                Reasons::kDirectL3Miss};
        const char *negative[]={Reasons::kDirectL1Hit,Reasons::kDirectL2Hit,
                                Reasons::kDirectL3Hit};
        for(unsigned l=0;l<3;++l) {
            const unsigned bit=1u<<l;
            const bool member=!invalid && (m.sprMiss.miss_mask&bit);
            const bool unknown=invalid ||
                (!member && (m.unresolved || (m.sprMiss.unknown()&bit)));
            Label label;
            label.membership=unknown ? Membership::Unknown :
                (member ? Membership::Member : Membership::Nonmember);
            label.certainty=unknown ? Certainty::Unclassified :
                ((m.sprMiss.estimated_mask&bit) ? Certainty::Estimated : Certainty::Exact);
            label.reason=unknown ? (m.unsupported ? Reasons::kUnsupportedIsa :
                (invalid ? Reasons::kBoundary : Reasons::kIncompleteLifecycle)) :
                ((m.sprMiss.estimated_mask&bit) ? Reasons::kMixedFragment :
                 (member ? positive[l] : negative[l]));
            c.spr_miss[l].add(label);
        }
        c.membership_failures={result.fail_incomplete,result.fail_stale,
            result.fail_multi_follower,result.fail_mixed_ownership,
            result.fail_boundary,result.fail_unsupported};
        const unsigned classes=m.source&(L1|Miss|FillBuffer);
        c.forwarded_loads=bool(m.source&Forwarded);
        c.mixed_source_loads=classes && (classes&(classes-1));
        c.unresolved_source_uops=m.unresolved;
        c.unsupported_loads=m.unsupported;
        const bool boundary_unknown=m.unsupported || m.leading || m.crossing;
        // A model follower's path is known, but without its own lookup its
        // membership in a hardware-named event is not established.
        const bool unknown=boundary_unknown || m.unresolved ||
            (m.independent_source & (Unknown|SequencerShared|
                                     PermissionUpgrade|RemoteSupply));
        // Positive membership can be proven even when another member's source
        // is unresolved. Each event keeps its own unknown population.
        c.independent_l1_hits=!boundary_unknown &&
            bool(m.independent_source&L1);
        c.independent_l1_misses=!boundary_unknown && bool(m.independent_source&Miss);
        c.independent_fb_hits=!boundary_unknown && bool(m.independent_source&FillBuffer);
        c.independent_l1_hits_unknown=unknown && !c.independent_l1_hits;
        c.independent_l1_misses_unknown=unknown && !c.independent_l1_misses;
        c.independent_fb_hits_unknown=unknown && !c.independent_fb_hits;
        c.independent_l2_hits=!boundary_unknown && bool(m.independent_source&L2Hit);
        c.independent_l2_hits_unknown=(boundary_unknown || m.unresolved || (m.independent_source&(L2Unknown|SequencerShared))) && !c.independent_l2_hits;
        c.independent_l2_misses=!boundary_unknown && bool(m.independent_source&L2Miss);
        c.independent_l2_misses_unknown=(boundary_unknown || m.unresolved || (m.independent_source&(L2Unknown|SequencerShared))) && !c.independent_l2_misses;
        c.independent_l3_hits=!boundary_unknown && bool(m.independent_source&L3Hit);
        c.independent_l3_hits_unknown=(boundary_unknown || m.unresolved || (m.independent_source&(L3Unknown|SequencerShared))) && !c.independent_l3_hits;
        c.independent_l3_misses=!boundary_unknown && bool(m.independent_source&L3Miss);
        c.independent_l3_misses_unknown=(boundary_unknown || m.unresolved || (m.independent_source&(L3Unknown|SequencerShared))) && !c.independent_l3_misses;
        if (m.unresolved || (m.source&Unknown) || !classes ||
            m.unsupported || m.leading) c.unclassified_loads=1;
        else {
            c.classified_loads=1;
            if (m.source&Miss) c.perf_l1d_load_misses=1;
            else if (m.source&FillBuffer) c.perf_l1d_load_fb_hits=1;
            else c.perf_l1d_load_hits=1;
        }
        return c;
    }
  public:
    bool hasOpen(uint32_t context) const {
        const auto found=open_.find(context);
        return found!=open_.end() && bool(found->second);
    }
    bool lastHardwareEntry() const { return last_hardware_entry_; }
    bool lastPreciseFaultAbandonment() const { return last_precise_fault_abandonment_; }
    bool notePreciseFault(uint32_t context,uint64_t pc) {
        const auto found=open_.find(context);
        if (found==open_.end() || !found->second || found->second->pc!=pc) return false;
        found->second->precise_fault=true;return true;
    }
    void commit(const Member &member) {
        last_precise_fault_abandonment_=false;
        auto &pending=open_[member.context];
        if ((member.first || member.rom_entry) && pending) {
            // Only an independently accepted precise fault at the same PC
            // proves why this incomplete prefix did not retire. Other starts
            // still fail the discontinuity gate, including unproven IRQ gaps.
            const bool precise=pending->precise_fault && member.rom_entry && member.hardware_entry;
            last_precise_fault_abandonment_=precise;
            if (member.measured) {
                Counts c;
                if (precise) {
                    c.precise_fault_abandoned_macros=bool(pending->measured_functional_uops);
                    add(owners_[pending->owner],c,pending->user);
                } else {
                    c.boundary_discontinuities=1;add(owners_[member.owner],c,member.user);
                }
            }
            abandoned_.push_back(pending);
            pending.reset();
        }
        if (!pending) {
            pending=std::make_shared<Macro>();
            pending->leading=!member.first && !member.rom_entry;
            pending->rom_entry=member.rom_entry;
            pending->hardware_entry=member.rom_entry && member.rom && member.hardware_entry;
            pending->owner=member.owner; pending->pc=member.pc; pending->user=member.user;
        }
        auto &m=*pending;
        last_hardware_entry_=m.hardware_entry;
        m.crossing|=m.owner!=member.owner || m.user!=member.user;
        m.owner=member.owner; m.user=member.user;
        ++m.uops;
        if (member.measured && (!member.syscall || member.first))
            ++m.measured_functional_uops;
        m.warmup|=!member.measured;
        m.measured|=member.measured;
        m.read|=!m.hardware_entry && (member.read || member.software_prefetch);
        m.write|=!m.hardware_entry && member.write;
        // SPR counts retired software prefetch hints in ALL_LOADS and L1_HIT,
        // independently of their asynchronous cache-fill request. They have
        // known negative membership in the three retired-load miss events.
        if (member.software_prefetch) {
            spr_retired::Result hint;
            hint.service=spr_retired::Service::SoftwarePrefetch;
            m.sprMiss.add(spr_retired::convert(hint));
            m.source|=L1;
            m.independent_source|=L1;
            retired_membership::CaseInput input;
            input.fragments.emplace_back();
            input.fragments.back().software_prefetch=true;
            mergeMembership(m,retired_membership::evaluate(input));
        }
        m.unsupported|=!m.hardware_entry && (member.unsupported || member.rom);
        m.rom|=member.rom;
        if (member.read && !m.hardware_entry) {
            if (++m.read_members>1) m.sprMiss.estimated_mask|=2;
            ++m.unresolved;
            const Identity identity{member.context,member.sequence};
            waiting_[identity]=pending;
            m.identities.push_back(identity);
        }
        if (member.measured) {
            Counts c;
            c.native_retired_uops=1;
            c.retired_uops=!member.syscall || member.first;
            add(owners_[member.owner],c,member.user);
        }
        if (!member.last) return;
        if (member.measured) {
            Counts c;
            c.retired_macro_instructions=1;
            c.perf_retired_loads=m.read; c.perf_retired_stores=m.write;
            c.scope_crossing_macros=m.crossing;
            c.warmup_spanning_macros=m.warmup;
            c.leading_incomplete_macros=m.leading;
            c.rom_entry_macros=m.rom_entry; c.rom_microcode_macros=m.rom;
            c.excluded_hardware_entry_macros=m.hardware_entry;
            c.independent_retired_loads_unknown=m.read &&
                (m.unsupported || m.leading || m.crossing);
            c.independent_retired_stores_unknown=m.write &&
                (m.unsupported || m.leading || m.crossing);
            add(owners_[m.owner],c,m.user);
            if (m.read) completed_loads_.push_back(pending);
        } else {
            // Completed warmup macros cannot contribute to a measured event.
            for (const auto &identity : m.identities) waiting_.erase(identity);
        }
        pending.reset();
    }
    void resolve(uint32_t context, uint64_t sequence, unsigned source,
                 unsigned independent_source=0) {
        const auto found=waiting_.find({context,sequence});
        if (found==waiting_.end()) return;
        found->second->source|=source ? source : Unknown;
        found->second->independent_source|=independent_source ? independent_source :
            (source ? source : Unknown);
        --found->second->unresolved;
        waiting_.erase(found);
    }
    void resolveNative(uint32_t context,uint64_t sequence,const NativeFacts &facts) {
        const unsigned membership=classifyIndependent(facts)|classifyRetiredCache(facts);
        const unsigned unknown=membership&(Unknown|L2Unknown|L3Unknown);
        const auto waiting=waiting_.find({context,sequence});
        if (waiting!=waiting_.end()) {
            waiting->second->sprMiss.add(facts.sprMiss);
            spr_incomplete_completions_+=facts.spr_incomplete_completions;
            retired_membership::CaseInput input;
            input.fragments=facts.membership_fragments;
            input.physical_split=input.fragments.size()>1;
            mergeMembership(*waiting->second,retired_membership::evaluate(input));
        }
        // Diagnostic samples never become a reference fallback. Count every
        // measured unresolved-class observation; retain at most eight examples
        // per mask, so a noisy early class cannot hide another class.
        if ((unknown || facts.sprMiss.unknown()) &&
            waiting!=waiting_.end() && waiting->second->measured) {
            const auto &macro=*waiting->second;
            const auto diagnostic_mask=unknown | (unsigned(facts.sprMiss.unknown())<<16);
            const auto count=++native_unknown_masks_[diagnostic_mask];
            native_unknown_l1_late_prefetch_+=bool(facts.l1_late_prefetches);
            native_unknown_l2_late_prefetch_+=bool(facts.l2_late_prefetches);
            for (size_t i=0; i<native_unknown_l2_prefetch_parent_llc_.size(); ++i)
                native_unknown_l2_prefetch_parent_llc_[i] += bool(facts.l2_prefetch_parent_llc[i]);
            for (size_t i=0; i<native_unknown_l2_prefetch_parent_sender_.size(); ++i) {
                native_unknown_l2_prefetch_parent_sender_[i] += bool(facts.l2_prefetch_parent_sender[i]);
                native_unknown_l2_prefetch_parent_response_[i] += bool(facts.l2_prefetch_parent_response[i]);
            }
            for (size_t i=0; i<native_unknown_l1_prefetch_parent_llc_.size(); ++i)
                native_unknown_l1_prefetch_parent_llc_[i] += bool(facts.l1_prefetch_parent_llc[i]);
            for (size_t i=0; i<native_unknown_l1_prefetch_parent_sender_.size(); ++i) {
                native_unknown_l1_prefetch_parent_sender_[i] += bool(facts.l1_prefetch_parent_sender[i]);
                native_unknown_l1_prefetch_parent_response_[i] += bool(facts.l1_prefetch_parent_response[i]);
            }
            native_unknown_stale_serviced_ += facts.stale_serviced;
            native_unknown_repeated_response_ += facts.repeated_response;
            const bool lifecycle_incomplete = !facts.resolved || facts.other_terminal ||
                !facts.responses || facts.responses!=facts.admissions ||
                facts.repeated_response || facts.hierarchy_attempts!=1 ||
                facts.hierarchy_completions!=1 ||
                facts.hierarchy_completion_attempt_id!=facts.hierarchy_attempt_id;
            native_unknown_lifecycle_incomplete_ += lifecycle_incomplete;
            native_unknown_l1_parent_reject_uniqueness_ += bool(facts.l1_parent_reject_mask & 1u);
            native_unknown_l1_parent_reject_stale_ += bool(facts.l1_parent_reject_mask & 2u);
            native_unknown_l2_parent_reject_uniqueness_ += bool(facts.l2_parent_reject_mask & 1u);
            native_unknown_l2_parent_reject_stale_ += bool(facts.l2_parent_reject_mask & 2u);
            if (count<=8) native_unknown_samples_.push_back(
                {context,sequence,macro.owner,macro.pc,membership,facts});
        }
        resolve(context,sequence,classify(facts),membership);
    }
    void writeNativeDiagnostics(std::ostream &out) const {
        out << "{\"diagnostic_only\":true,\"spr_policy\":\"" << spr_retired::Policy
            << "\",\"spr_incomplete_completions\":" << spr_incomplete_completions_
            << ",\"unit\":\"measured-read-uop-service-observation\",\"unknown_masks\":{";
        bool first=true;
        for (const auto &entry:native_unknown_masks_) {
            if (!first) out << ',';
            first=false;
            out << '\"' << entry.first << "\":" << entry.second;
        }
        out << "},\"l1_late_prefetch_unknown_uops\":" << native_unknown_l1_late_prefetch_
            << ",\"l2_late_prefetch_unknown_uops\":" << native_unknown_l2_late_prefetch_
            << ",\"l2_prefetch_parent_llc_unknown_uops\":[";
        for (size_t i=0; i<native_unknown_l2_prefetch_parent_llc_.size(); ++i) {
            if (i) out << ',';
            out << native_unknown_l2_prefetch_parent_llc_[i];
        }
        out << "],\"l2_prefetch_parent_sender_unknown_uops\":[";
        for (size_t i=0; i<native_unknown_l2_prefetch_parent_sender_.size(); ++i) {
            if (i) out << ',';
            out << native_unknown_l2_prefetch_parent_sender_[i];
        }
        out << "],\"l2_prefetch_parent_response_unknown_uops\":[";
        for (size_t i=0; i<native_unknown_l2_prefetch_parent_response_.size(); ++i) {
            if (i) out << ',';
            out << native_unknown_l2_prefetch_parent_response_[i];
        }
        out << "],\"l1_prefetch_parent_llc_unknown_uops\":[";
        for (size_t i=0; i<native_unknown_l1_prefetch_parent_llc_.size(); ++i) {
            if (i) out << ',';
            out << native_unknown_l1_prefetch_parent_llc_[i];
        }
        out << "],\"l1_prefetch_parent_sender_unknown_uops\":[";
        for (size_t i=0; i<native_unknown_l1_prefetch_parent_sender_.size(); ++i) {
            if (i) out << ',';
            out << native_unknown_l1_prefetch_parent_sender_[i];
        }
        out << "],\"l1_prefetch_parent_response_unknown_uops\":[";
        for (size_t i=0; i<native_unknown_l1_prefetch_parent_response_.size(); ++i) {
            if (i) out << ',';
            out << native_unknown_l1_prefetch_parent_response_[i];
        }
        out << "],\"stale_serviced_unknown_uops\":" << native_unknown_stale_serviced_
            << ",\"repeated_response_unknown_uops\":" << native_unknown_repeated_response_
            << ",\"lifecycle_incomplete_unknown_uops\":" << native_unknown_lifecycle_incomplete_
            << ",\"l1_parent_reject_uniqueness_unknown_uops\":" << native_unknown_l1_parent_reject_uniqueness_
            << ",\"l1_parent_reject_stale_unknown_uops\":" << native_unknown_l1_parent_reject_stale_
            << ",\"l2_parent_reject_uniqueness_unknown_uops\":" << native_unknown_l2_parent_reject_uniqueness_
            << ",\"l2_parent_reject_stale_unknown_uops\":" << native_unknown_l2_parent_reject_stale_
            << ",\"prefetch_lineage_tracking\":{\"scope\":\"sim-global-cumulative-at-core-finalization\",\"capacity_overflows\":"
            << prefetch_lineage::globalOracle().overflows()
            << ",\"live\":" << prefetch_lineage::globalOracle().live()
            << "},\"spr_lifecycle\":{\"scope\":\"sim-global-cumulative-at-core-finalization\"";
        const auto &lifecycle=spr_retired::lifecycleDiagnostics();
        out << ",\"created\":" << lifecycle.created
            << ",\"completed\":" << lifecycle.completed
            << ",\"replaced\":" << lifecycle.replaced
            << ",\"canceled\":" << lifecycle.canceled
            << ",\"retained\":" << lifecycle.retained
            << ",\"released\":" << lifecycle.released
            << ",\"live\":" << lifecycle.live
            << ",\"peak\":" << lifecycle.peak
            << ",\"lookup_without_live_identity\":" << lifecycle.late_lookup
            << ",\"unclassified_alias_states\":{\"SP_IE\":" << lifecycle.unclassified_alias[0]
            << ",\"Inst_IS\":" << lifecycle.unclassified_alias[1]
            << ",\"PF_Inst_IS\":" << lifecycle.unclassified_alias[2]
            << ",\"I_F\":" << lifecycle.unclassified_alias[3]
            << ",\"V_F\":" << lifecycle.unclassified_alias[4]
            << ",\"other\":" << lifecycle.unclassified_alias[5]
            << ",\"SP_IE_fill_credit\":" << lifecycle.unclassified_alias[6]
            << ",\"SP_IE_no_fill_credit\":" << lifecycle.unclassified_alias[7] << "}"
            << ",\"prepare_service\":{";
        const auto &prepare=spr_retired::prepareServices();
        const auto &prepare_counts=prepare.counts();
        out << "\"begun\":" << prepare_counts.begun
            << ",\"linked\":" << prepare_counts.linked
            << ",\"data_completed\":" << prepare_counts.data
            << ",\"invalidated\":" << prepare_counts.invalidated
            << ",\"replaced\":" << prepare_counts.replaced
            << ",\"tbe_closed\":" << prepare_counts.closed
            << ",\"destroyed\":" << prepare_counts.destroyed
            << ",\"live\":" << prepare.live()
            << ",\"live_tbes\":" << prepare.tbes()
            << ",\"peak\":" << prepare_counts.peak << "}"
            << "},\"samples\":[";first=true;
        for (const auto &sample:native_unknown_samples_) {
            if (!first) out << ',';
            first=false;
            out << "{\"context\":" << sample.context << ",\"sequence\":" << sample.sequence
                << ",\"owner\":" << sample.owner << ",\"pc\":" << sample.pc
                << ",\"membership\":" << sample.membership << ",\"facts\":{";
            const auto &f=sample.facts;
            out << "\"resolved\":" << (f.resolved?"true":"false");
            out << ",\"spr_miss_mask\":" << unsigned(f.sprMiss.miss_mask)
                << ",\"spr_unknown_mask\":" << unsigned(f.sprMiss.unknown())
                << ",\"spr_failure_callback\":\""
                << (f.spr_incomplete_completions ? "noteHierarchyCompletion" : "not-observed")
                << '"'
                << ",\"spr_missing_levels\":" << f.spr_missing_levels
                << ",\"spr_failed_alias_category\":" << f.spr_failed_alias_category
                << ",\"spr_failed_prepare_status\":" << f.spr_failed_prepare_status
                << ",\"spr_failed_prepare_invalidation\":" << f.spr_failed_prepare_invalidation
                << ",\"spr_failed_request\":" << f.spr_failed_request
                << ",\"spr_failed_address\":" << f.spr_failed_address
                << ",\"spr_completion_callback\":\"Sequencer::hitCallback\""
                << ",\"spr_failed_level_results\":[";
            for(unsigned l=0;l<3;++l) {
                if(l) out << ',';
                out << unsigned(f.spr_failed_levels[l]);
            }
            out << "]";
#define WRITE_FACT(name) out << ",\"" #name "\":" << f.name;
            WRITE_FACT(store_forward) WRITE_FACT(other_terminal) WRITE_FACT(terminal_reason_mask)
            WRITE_FACT(admissions) WRITE_FACT(responses) WRITE_FACT(coalesced) WRITE_FACT(hierarchy_requests)
            WRITE_FACT(external_hits) WRITE_FACT(external_misses) WRITE_FACT(accesses) WRITE_FACT(hits)
            WRITE_FACT(tag_misses) WRITE_FACT(permission_upgrades) WRITE_FACT(merged_misses) WRITE_FACT(remote_supplies)
            WRITE_FACT(l2_accesses) WRITE_FACT(l2_hits) WRITE_FACT(l2_tag_misses) WRITE_FACT(l2_other)
            WRITE_FACT(l3_accesses) WRITE_FACT(l3_hits) WRITE_FACT(l3_tag_misses) WRITE_FACT(l3_other)
            WRITE_FACT(l3_merged_misses) WRITE_FACT(l3_remote_supplies)
            WRITE_FACT(l2_permission_upgrades) WRITE_FACT(l3_permission_upgrades)
            WRITE_FACT(l1_late_prefetches) WRITE_FACT(l2_late_prefetches)
#undef WRITE_FACT
            out << ",\"l2_prefetch_parent_llc\":[";
            for (size_t i=0; i<f.l2_prefetch_parent_llc.size(); ++i) {
                if (i) out << ',';
                out << f.l2_prefetch_parent_llc[i];
            }
            out << "],\"l2_prefetch_parent_sender\":[";
            for (size_t i=0; i<f.l2_prefetch_parent_sender.size(); ++i) {
                if (i) out << ',';
                out << f.l2_prefetch_parent_sender[i];
            }
            out << "],\"l2_prefetch_parent_response\":[";
            for (size_t i=0; i<f.l2_prefetch_parent_response.size(); ++i) {
                if (i) out << ',';
                out << f.l2_prefetch_parent_response[i];
            }
            out << "],\"l2_prefetch_parent_sender_id\":" << f.l2_prefetch_parent_sender_id;
            out << ",\"stale_serviced\":" << (f.stale_serviced?"true":"false")
                << ",\"repeated_response\":" << (f.repeated_response?"true":"false")
                << ",\"hierarchy_attempts\":" << f.hierarchy_attempts
                << ",\"hierarchy_completions\":" << f.hierarchy_completions
                << ",\"hierarchy_attempt_id\":" << f.hierarchy_attempt_id
                << ",\"hierarchy_completion_attempt_id\":" << f.hierarchy_completion_attempt_id
                << ",\"l1_parent_reject_mask\":" << unsigned(f.l1_parent_reject_mask)
                << ",\"l2_parent_reject_mask\":" << unsigned(f.l2_parent_reject_mask);
            out << ",\"l1_prefetch_parent_llc\":[";
            for (size_t i=0; i<f.l1_prefetch_parent_llc.size(); ++i) {
                if (i) out << ',';
                out << f.l1_prefetch_parent_llc[i];
            }
            out << "],\"l1_prefetch_parent_sender\":[";
            for (size_t i=0; i<f.l1_prefetch_parent_sender.size(); ++i) {
                if (i) out << ',';
                out << f.l1_prefetch_parent_sender[i];
            }
            out << "],\"l1_prefetch_parent_response\":[";
            for (size_t i=0; i<f.l1_prefetch_parent_response.size(); ++i) {
                if (i) out << ',';
                out << f.l1_prefetch_parent_response[i];
            }
            out << "],\"l1_prefetch_parent_sender_id\":" << f.l1_prefetch_parent_sender_id;
            out << "}}";
        }
        out << "]}";
    }
    std::map<uint64_t,Scopes> owners() const {
        auto result=owners_;
        for (const auto &m : completed_loads_)
            add(result[m->owner],sourceCounts(*m),m->user);
        const auto tail=[&](const std::shared_ptr<Macro> &m) {
            if (!m) return;
            Counts c;
            c.all_history_incomplete_macros=1; c.all_history_incomplete_uops=m->uops;
            c.incomplete_macros=bool(m->measured_functional_uops);
            c.incomplete_uops=m->measured_functional_uops;
            c.leading_incomplete_macros=m->leading && m->measured;
            add(result[m->owner],c,m->user);
        };
        for (const auto &entry : open_) tail(entry.second);
        for (const auto &m : abandoned_) tail(m);
        return result;
    }
    Scopes snapshot(const std::set<uint64_t> &workload) const {
        Scopes result;
        for (const auto &entry : owners()) {
            result.physical+=entry.second.physical;
            result.user+=entry.second.user;
            result.kernel+=entry.second.kernel;
            if (entry.first)
                (workload.count(entry.first) ? result.workload_task : result.other_task)
                    +=entry.second.physical;
        }
        return result;
    }
};

}}} // namespace gem5::o3::retirement
#endif
