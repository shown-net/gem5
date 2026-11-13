/* Oracle-only native transaction observer. Never serialized into FST input. */
#ifndef __MEM_PERF_CACHE_NATIVE_HH__
#define __MEM_PERF_CACHE_NATIVE_HH__
#include <array>
#include <cstdint>
#include <map>
#include <ostream>
#include <unordered_map>
#include <vector>

namespace gem5 { namespace perf_cache_native {
enum class Kind : unsigned {
    DataRead, Rfo, CodeRead, SoftwarePrefetch, HardwarePrefetch,
    HardwarePrefetchL1, HardwarePrefetchL2, HardwarePrefetchL3, Writeback, Unknown,
    Count
};
enum class Outcome : unsigned { Hit, TagMiss, PermissionUpgrade, MergedMiss,
                                RemoteSupply, Count };
inline constexpr unsigned Kinds=unsigned(Kind::Count), Outcomes=unsigned(Outcome::Count);
struct Counts {
    // Physical levels: 0=L1D, 1=private L2, 2=LLC. No eventual supplier folding.
    std::array<std::array<std::array<uint64_t,Outcomes>,Kinds>,3> levels{};
    uint64_t issued=0, merged=0, retries=0, writebacks=0, dirty_writebacks=0;
    uint64_t unknown_context=0, unknown_kind=0, ignored_hooks=0;
    uint64_t unknown_identity=0;
    uint64_t duplicate_edges=0, overflow=0;
    uint64_t inflight_begin=0, inflight_end=0, released=0, superseded=0;
    int64_t inflight_balance_residual=0;
    std::array<uint64_t,3> outgoing_writebacks{}, outgoing_dirty_writebacks{},
                           invalidations{};
    uint64_t accesses(unsigned level) const {
        uint64_t n=0;
        for (unsigned k=0;k<Kinds;++k) {
            if (k==unsigned(Kind::Writeback) || k==unsigned(Kind::Unknown) ||
                (level==2 && k==unsigned(Kind::HardwarePrefetchL3))) continue;
            for (unsigned o=0;o<Outcomes;++o)
                if (level==2 || o!=unsigned(Outcome::MergedMiss)) n+=levels[level][k][o];
        }
        return n;
    }
    uint64_t l2_accesses() const { return accesses(1); }
    uint64_t llc_accesses() const { return accesses(2); }
    uint64_t l2_demand_misses() const {
        uint64_t n=0;
        for (unsigned k=0;k<=unsigned(Kind::CodeRead);++k)
            n+=levels[1][k][unsigned(Outcome::TagMiss)];
        return n;
    }
    uint64_t llc_tag_misses() const {
        uint64_t n=0;
        for (unsigned k=0;k<Kinds;++k)
            if(k!=unsigned(Kind::Writeback) && k!=unsigned(Kind::Unknown) &&
               k!=unsigned(Kind::HardwarePrefetchL3))
                n+=levels[2][k][unsigned(Outcome::TagMiss)];
        return n;
    }
};
struct Sample {
    int origin_core=-1;
    Kind request_kind=Kind::Unknown;
    uint64_t logical_request_id=0,fragment_id=0,attempt_id=0,transaction_id=0;
    uint64_t parent_transaction_id=0,window_id=0,tick=0;
    unsigned cache_level=0;
    Outcome lookup_facts=Outcome::Hit;
    bool event_context_known=false,writeback=false;
};
class Observer {
    struct Window { bool open=false; uint64_t begin=0,end=0; Counts counts;
                    std::vector<Sample> samples; uint64_t omitted=0; };
    struct Transaction { int core=-1; Kind kind=Kind::Unknown;
                         unsigned levels=0; uint64_t packet=0; };
    uint64_t next_=1;
    uint64_t unassigned_origin_hooks_=0;
    std::map<int,Window> cores_;
    std::unordered_map<uint64_t,Transaction> live_;
    std::unordered_map<uint64_t,uint64_t> packets_;
    std::array<uint64_t,3> global_writebacks_{}, global_dirty_writebacks_{};
    static constexpr std::size_t MaxLive=65536;
    Counts *at(int core,uint64_t tick) {
        auto it=cores_.find(core);
        if (it==cores_.end() || !it->second.open || tick<it->second.begin) return nullptr;
        return &it->second.counts;
    }
    Counts *openWindow(int core) {
        auto it=cores_.find(core);
        return it==cores_.end() || !it->second.open ? nullptr : &it->second.counts;
    }
    void sample(int core,uint64_t id,uint64_t packet,Kind kind,unsigned level,
                Outcome outcome,uint64_t tick,bool writeback=false) {
        auto &w=cores_.at(core);
        if(w.samples.size()>=32) { ++w.omitted; return; }
        // Packet identifies one native line request. It is intentionally not
        // represented as an architectural macro or a split-load parent ID.
        Sample s; s.origin_core=core; s.request_kind=kind;
        s.logical_request_id=packet; s.transaction_id=id; s.attempt_id=id;
        s.window_id=w.begin; s.tick=tick; s.cache_level=level;
        s.lookup_facts=outcome; s.writeback=writeback;
        w.samples.push_back(s);
    }
  public:
    void prepare(int core) { cores_.try_emplace(core); }
    void open(int core,uint64_t tick) {
        auto &w=cores_[core]; w.open=true; w.begin=tick; w.end=0; w.counts=Counts{};
        w.samples.clear(); w.omitted=0;
        w.counts.inflight_begin=inflight(core);
    }
    void close(int core,uint64_t tick) {
        auto &w=cores_[core];
        if(w.open) w.counts=snapshot(core);
        w.open=false; w.end=tick;
    }
    uint64_t issue(int core,uint64_t packet,Kind kind,uint64_t tick) {
        if (!cores_.count(core)) return 0;
        auto old=packets_.find(packet);
        if (old!=packets_.end()) {
            const auto tx=live_.find(old->second);
            if (tx!=live_.end())
                if (auto *c=at(tx->second.core,tick)) ++c->superseded;
            live_.erase(old->second);
            packets_.erase(old);
        }
        if (live_.size()>=MaxLive || !next_) {
            if (auto *c=at(core,tick)) ++c->overflow;
            return 0;
        }
        if (unsigned(kind)>=Kinds) kind=Kind::Unknown;
        const uint64_t id=next_++;
        live_[id]={core,kind,0,packet}; packets_[packet]=id;
        if (auto *c=at(core,tick)) {
            ++c->issued;
            if (kind==Kind::Unknown) ++c->unknown_kind;
        }
        return id;
    }
    void lookup(uint64_t id,unsigned level,Outcome outcome,uint64_t tick,int origin_hint=-1) {
        auto it=live_.find(id);
        if (it==live_.end()) {
            if (auto *c=at(origin_hint,tick)) {
                ++c->ignored_hooks;
                unidentifiedLookup(origin_hint,Kind::Unknown,level,outcome,tick);
            } else if (origin_hint<0) unassignedOrigin();
            return;
        }
        auto &t=it->second;
        auto *c=at(t.core,tick);
        if (level>=3 || unsigned(outcome)>=Outcomes) {
            if (c) ++c->ignored_hooks;
            return;
        }
        if (t.levels&(1u<<level)) { if(c) ++c->duplicate_edges; return; }
        t.levels|=1u<<level;
        if(c) {
            ++c->levels[level][unsigned(t.kind)][unsigned(outcome)];
            // Native event-edge privilege/task context is deliberately absent.
            ++c->unknown_context;
            sample(t.core,id,t.packet,t.kind,level,outcome,tick);
        }
    }
    uint64_t attemptForPacket(uint64_t packet) const {
        const auto it=packets_.find(packet);
        if (it==packets_.end()) return 0;
        const auto tx=live_.find(it->second);
        return tx!=live_.end() && tx->second.packet==packet ? it->second : 0;
    }
    void finish(uint64_t packet) {
        const auto it=packets_.find(packet);
        if (it==packets_.end()) return;
        const auto tx=live_.find(it->second);
        if (tx!=live_.end()) {
            if (auto *c=openWindow(tx->second.core)) ++c->released;
        }
        live_.erase(it->second); packets_.erase(it);
    }
    void merged(int core,uint64_t tick) { if(auto *c=at(core,tick)) ++c->merged; }
    void retry(int core,uint64_t tick) { if(auto *c=at(core,tick)) ++c->retries; }
    void unknownHook(int core,uint64_t tick) {
        if(auto *c=at(core,tick)) ++c->ignored_hooks;
    }
    void unassignedOrigin() { ++unassigned_origin_hooks_; }
    uint64_t unassignedOrigins() const { return unassigned_origin_hooks_; }
    void unidentifiedLookup(int core,Kind kind,unsigned level,Outcome outcome,uint64_t tick) {
        if(auto *c=at(core,tick)) {
            ++c->unknown_identity; ++c->unknown_context;
            if(level>=3 || unsigned(outcome)>=Outcomes) { ++c->ignored_hooks; return; }
            if(unsigned(kind)>=Kinds) kind=Kind::Unknown;
            ++c->levels[level][unsigned(kind)][unsigned(outcome)];
            if(kind==Kind::Unknown) ++c->unknown_kind;
            sample(core,0,0,kind,level,outcome,tick);
        }
    }
    void writeback(int core,bool dirty,uint64_t tick) {
        writebackAt(core,1,dirty,tick);
    }
    void writebackAt(int core,unsigned level,bool dirty,uint64_t tick) {
        if(auto *c=at(core,tick)) {
            if(level>=3) { ++c->ignored_hooks; return; }
            ++c->outgoing_writebacks[level];
            c->outgoing_dirty_writebacks[level]+=dirty;
            ++c->unknown_context;
            // Legacy fields remain specifically private-L2 evictions.
            if(level==1) { ++c->writebacks; c->dirty_writebacks+=dirty; }
            sample(core,next_++,0,Kind::Writeback,level,Outcome::Hit,tick,true);
        }
    }
    void globalWriteback(unsigned level,bool dirty) {
        // Whole-run, unassigned LLC slice diagnostics; never core/ROI PMUs.
        if(level<3) {
            ++global_writebacks_[level];
            global_dirty_writebacks_[level]+=dirty;
        }
    }
    void invalidation(int core,unsigned level,uint64_t tick) {
        if(auto *c=at(core,tick)) {
            if(level<3) ++c->invalidations[level];
            else ++c->ignored_hooks;
        }
    }
    Counts snapshot(int core) const {
        const auto it=cores_.find(core);
        if(it==cores_.end()) return Counts{};
        auto c=it->second.counts;
        if(it->second.open || !it->second.end) {
            c.inflight_end=inflight(core);
            c.inflight_balance_residual=int64_t(c.inflight_begin)+int64_t(c.issued)
                -int64_t(c.released)-int64_t(c.superseded)-int64_t(c.inflight_end);
        }
        return c;
    }
    uint64_t inflight(int core) const {
        uint64_t n=0; for(const auto &entry:live_) n+=entry.second.core==core; return n;
    }
    uint64_t globalWritebacks(unsigned level) const {
        return level<3 ? global_writebacks_[level] : 0;
    }
    uint64_t globalDirtyWritebacks(unsigned level) const {
        return level<3 ? global_dirty_writebacks_[level] : 0;
    }
    const std::vector<Sample> &samples(int core) const { return cores_.at(core).samples; }
    void writeSamples(std::ostream &out,int core) const {
        out << '[';
        const auto it=cores_.find(core);
        if(it!=cores_.end()) {
            bool first=true;
            for(const auto &s:it->second.samples) {
                if(!first) out << ',';
                first=false;
                out << "{\"origin_core\":" << s.origin_core
                    << ",\"request_kind\":" << unsigned(s.request_kind)
                    << ",\"logical_request_id\":" << s.logical_request_id
                    << ",\"fragment_id\":" << s.fragment_id
                    << ",\"attempt_id\":" << s.attempt_id
                    << ",\"transaction_id\":" << s.transaction_id
                    << ",\"parent_transaction_id\":" << s.parent_transaction_id
                    << ",\"cache_level\":" << s.cache_level
                    << ",\"event_phase\":\"" << (s.writeback ? "outgoing-writeback":"controller-lookup")
                    << "\",\"event_context\":{\"known\":false},\"lookup_facts\":";
                if(s.writeback) out << "null";
                else out << unsigned(s.lookup_facts);
                out << ",\"window_id\":" << s.window_id << ",\"tick\":" << s.tick << '}';
            }
        }
        out << ']';
    }
    uint64_t samplesOmitted(int core) const {
        const auto it=cores_.find(core); return it==cores_.end() ? 0 : it->second.omitted;
    }
};
inline Observer &observer() { static Observer instance; return instance; }
inline void writeCounts(std::ostream &out,const Counts &c) {
    static const char *levels[]={"l1d","private_l2","llc"};
    static const char *kinds[]={"data_read","rfo","code_read","software_prefetch",
                              "hardware_prefetch","hardware_prefetch_l1","hardware_prefetch_l2",
                              "hardware_prefetch_l3","writeback","unknown"};
    static const char *outcomes[]={"hit","tag_miss","permission_upgrade",
                                 "merged_miss","remote_supply"};
    out << "{\"levels\":{";
    for(unsigned l=0;l<3;++l) {
        if(l) out << ',';
        out << '"' << levels[l] << "\":{";
        for(unsigned k=0;k<Kinds;++k) {
            if(k) out << ',';
            out << '"' << kinds[k] << "\":{";
            for(unsigned o=0;o<Outcomes;++o) {
                if(o) out << ',';
                out << '"' << outcomes[o] << "\":" << c.levels[l][k][o];
            }
            out << '}';
        }
        out << '}';
    }
    out << "},\"private_l2_outgoing_writebacks\":" << c.writebacks
        << ",\"private_l2_outgoing_dirty_writebacks\":" << c.dirty_writebacks
        << ",\"issued\":" << c.issued << ",\"merged\":" << c.merged
        << ",\"retries\":" << c.retries << ",\"unknown_context\":" << c.unknown_context
        << ",\"unknown_kind\":" << c.unknown_kind << ",\"ignored_hooks\":" << c.ignored_hooks
        << ",\"unknown_identity\":" << c.unknown_identity
        << ",\"duplicate_edges\":" << c.duplicate_edges << ",\"overflow\":" << c.overflow
        << ",\"per_level_diagnostics\":{";
    for(unsigned l=0;l<3;++l) {
        if(l) out << ',';
        out << '"' << levels[l] << "\":{\"outgoing_writebacks\":"
            << c.outgoing_writebacks[l] << ",\"outgoing_dirty_writebacks\":"
            << c.outgoing_dirty_writebacks[l] << ",\"invalidations\":"
            << c.invalidations[l] << '}';
    }
    out << "},\"transaction_balance\":{\"inflight_begin\":" << c.inflight_begin
        << ",\"inflight_end\":" << c.inflight_end
        << ",\"released\":" << c.released
        << ",\"superseded\":" << c.superseded
        << ",\"balance_residual\":" << c.inflight_balance_residual << "}}";
}
}} // gem5::perf_cache_native
#endif
