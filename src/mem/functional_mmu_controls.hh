#pragma once
// Observer only. This header deliberately has no CPU, x86, tick, or TLB-entry
// dependency. Endpoint pointers, owner IDs and effect tickets never enter wire.
#include "fst_mmu_controls.hh"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
namespace gem5::functional_mmu {
using namespace fastsim::fst::mmuctl;
class Observer;
using OwnerKey = std::pair<std::uint32_t, std::uint64_t>;
inline std::uint32_t generation(const void *p);
struct Endpoint {
    Observer *observer = nullptr;
    Domain domain = Domain::Unknown;
};
inline std::map<const void *, Endpoint> &registry() {
    static std::map<const void *, Endpoint> r;
    return r;
}
inline Endpoint endpoint(const void *p) {
    auto i = registry().find(p);
    return i == registry().end() ? Endpoint{} : i->second;
}
// Only scalar evidence is retained for completed excluded groups. Each live
// candidate owns witnesses relative to its own first effect, not completion time.
struct ExcludedEvidence {
    std::uint64_t writes = 0, commands = 0, visits = 0;
    std::array<std::uint64_t, 16> reasons{};
};
struct Payload {
    std::uint64_t owner = 0, first = 0, last = 0, writes = 0, commands = 0, visits = 0;
    std::uint32_t generation = 0;
    std::array<unsigned char, 64> hot{};
    State pre{};
    ExcludedEvidence excluded;
    std::vector<Operation> ops;
    Member member;
    Reason cause = Reason::UnboundExternal;
    bool bound = false, sealed = false, unsupported = false, discarded = false, boundary = false;
};
// Intrusive links live on the existing native call stack. No event history or
// DynInst lifetime is retained merely to observe a scope crossing the cutoff.
struct ActiveScope {
    const std::shared_ptr<Payload> *payload = nullptr;
    ActiveScope *previous = nullptr;
};
struct OwnerScope {
    Observer *observer;
    std::uint64_t owner;
    std::uint32_t generation;
    Application application;
    OwnerScope *previous;
    inline static thread_local OwnerScope *active = nullptr;
    OwnerScope(const void *p, std::uint64_t id, Application a, std::uint32_t gen = UINT32_MAX)
        : observer(endpoint(p).observer), owner(id),
          generation(gen == UINT32_MAX ? functional_mmu::generation(p) : gen), application(a),
          previous(active) {
        active = this;
    }
    OwnerScope(const OwnerScope &) = delete;
    OwnerScope &operator=(const OwnerScope &) = delete;
    ~OwnerScope() { active = previous; }
};
struct CauseScope {
    Reason cause;
    CauseScope *previous;
    const void *actual_fault=nullptr,*thread_context=nullptr;
    inline static thread_local CauseScope *active = nullptr;
    explicit CauseScope(Reason r,const void* fault=nullptr,const void* tc=nullptr) : cause(r), previous(active),actual_fault(fault),thread_context(tc) { active = this; }
    CauseScope(const CauseScope &) = delete;
    CauseScope &operator=(const CauseScope &) = delete;
    ~CauseScope() { active = previous; }
};
// A capability is valid only for the synchronous accepted object and exact TC.
// It does not change default CauseScope precedence or grant generic PF ownership.
struct BoundaryScope {
 Observer* observer=nullptr;const void* fault=nullptr;const void* tc=nullptr;
 std::shared_ptr<Payload> payload;bool page_fault=false;BoundaryScope* previous=nullptr;
 inline static thread_local BoundaryScope* active=nullptr;
 BoundaryScope(Observer&,const void*,const void*,std::uint64_t,bool,bool=true);
 ~BoundaryScope();BoundaryScope(const BoundaryScope&)=delete;BoundaryScope& operator=(const BoundaryScope&)=delete;
};
inline bool causeBlocksOwnership(){const auto* c=CauseScope::active;if(!c)return false;const auto* b=BoundaryScope::active;return !(b && b->payload && !b->previous && b->page_fault && !c->previous && c->cause==Reason::UnboundPageFault && c->actual_fault==b->fault && c->thread_context==b->tc);}
struct BoundaryCallbacks {
 std::function<void(const Payload&)> retained;
 std::function<void(const Payload&,Reason)> diagnostic;
 std::function<void(const Footer&)> finished;
};
class Observer {
    std::uint32_t context_, generation_ = 0, max_owners_ = 4096, max_ops_ = 256, max_details_ = 256,
                            last_generation_ = 0;
    std::function<State()> read_;
    std::unique_ptr<Writer> writer_;
    std::unique_ptr<BoundaryCallbacks> boundary_callbacks_;
    std::shared_ptr<Payload> boundary_payload_;
    bool hold_boundary_close_=false,last_boundary_owner_=false;
    std::map<OwnerKey, std::shared_ptr<Payload>> owners_;
    std::map<std::uint64_t, std::shared_ptr<Payload>> pending_;
    std::set<std::pair<const void *, const void *>> edges_;
    Baseline baseline_;
    Footer footer_;
    State mirror_{};
    ActiveScope *active_scopes_ = nullptr;
    std::uint64_t ticket_ = 0, last_owner_ = 0, last_emitted_ticket_ = 0, next_emit_ = 0,
                  details_ = 0, abandoned_ = 0;
    bool have_last_ = false, resumed_ = false, lifecycle_pending_ = true, started_ = false,
         closing_ = false, finished_ = false;
    void add(std::uint64_t &v, std::uint64_t n = 1) {
        if (!saturating_add(v, n))
            footer_.reasons[unsigned(Reason::CounterOverflow)] = 1;
    }
    void emit() {
        while (!pending_.empty() && pending_.begin()->first == next_emit_ &&
               pending_.begin()->second->sealed) {
            auto p = pending_.begin()->second;
            if (p->first && p->first < last_emitted_ticket_) {
                fail(Reason::EffectOrderUnsupported);
                p->unsupported = true;
            }
            last_emitted_ticket_ = std::max(last_emitted_ticket_, p->last);
            p->member.status = Sealed | (p->unsupported ? Unsupported : 0);
            if (p->unsupported) {
                diagnostic(p, Reason::EffectOrderUnsupported);
                p->ops.clear();
            } else {
                add(footer_.visits, p->visits);
                add(footer_.retained_writes, p->writes);
                add(footer_.retained_commands, p->commands);
                add(footer_.total_writes, p->writes);
                add(footer_.total_commands, p->commands);
            }
            p->member.count = p->ops.size();
            if(boundary_callbacks_)boundary_callbacks_->retained(*p);
            if (writer_)
                writer_->member(p->member, p->ops);
            ++next_emit_;
            add(footer_.sealed);
            owners_.erase({p->generation, p->owner});
            pending_.erase(pending_.begin());
        }
        if (closing_ && pending_.empty() && !hold_boundary_close_)
            finish();
    }
    void abandonPending() {
        for (auto &kv : pending_) {
            auto p = kv.second;
            fail(Reason::UnsealedOwner);
            diagnostic(p, Reason::UnsealedOwner);
            p->member.status = Unsupported;
            p->member.count = 0;
            if(boundary_callbacks_)boundary_callbacks_->retained(*p);
            if (writer_)
                writer_->member(p->member, {});
            owners_.erase({p->generation, p->owner});
            ++next_emit_;
            add(abandoned_);
        }
        pending_.clear();
    }
    void detachEndpoints() {
        for (auto i = registry().begin(); i != registry().end();)
            if (i->second.observer == this)
                i = registry().erase(i);
            else
                ++i;
    }
    void discardCrossing(const std::shared_ptr<Payload> &p, Reason reason) {
        if (p->discarded)
            return;
        fail(Reason::EffectOrderUnsupported);
        if (p->writes || p->commands)
            diagnostic(p, reason);
        p->discarded = true;
    }
    void finish() {
        if (finished_)
            return;
        if (!started_) {
            baseline_.cut = 2;
            if (writer_)
                writer_->baseline(baseline_);
        }
        footer_.pending = pending_.size() + abandoned_;
        footer_.status = Finalized | (footer_.hot ? 0 : Empty);
        if (footer_.hot && resumed_ && baseline_.known == 15 && baseline_.cut == 1 &&
            footer_.sealed == footer_.hot && !footer_.pending &&
            std::all_of(footer_.reasons.begin(), footer_.reasons.end(), [](auto v) { return !v; }))
            footer_.status |= Complete;
        if(boundary_callbacks_)boundary_callbacks_->finished(footer_);
        if (writer_)
            writer_->finish(footer_);
        finished_ = true;
        detachEndpoints();
        // Active RAII scopes may still unwind, but they cannot sample state or
        // enter the now-terminal writer. Pending evidence was finalized above.
        for (auto *scope = active_scopes_; scope; scope = scope->previous)
            (*scope->payload)->discarded = true;
        owners_.clear();
        pending_.clear();
    }

  public:
    explicit Observer(std::uint32_t context, std::function<State()> read)
        : context_(context), read_(std::move(read)) {}
    ~Observer() { detachEndpoints(); }
    Observer(const Observer &) = delete;
    Observer &operator=(const Observer &) = delete;
    void limits(unsigned owners, unsigned ops, unsigned details) {
        check(!started_ && owners && owners <= 4096 && ops && ops <= 256 && details <= 256,
              "observer limits");
        max_owners_ = owners;
        max_ops_ = ops;
        max_details_ = details;
    }
    void attach(const std::string &path, Header h) {
        check(!writer_ && !started_, "observer attach order");
        h.context = context_;
        h.max_owners = max_owners_;
        h.max_ops = max_ops_;
        h.max_diagnostics = max_details_;
        writer_ = std::make_unique<Writer>(path, h);
    }
    void configureBoundaries(BoundaryCallbacks callbacks){check(!writer_&&!started_&&!boundary_callbacks_,"boundary controls configuration");boundary_callbacks_=std::make_unique<BoundaryCallbacks>(std::move(callbacks));hold_boundary_close_=true;}
    bool armBoundaryEpoch(const State& state){if(finished_||closing_||!boundary_callbacks_||started_)return false;bool clear=owners_.empty()&&!active_scopes_&&!boundary_payload_&&resumed_;started_=true;mirror_=state;baseline_.state=state;baseline_.known=15;baseline_.generation=generation_;baseline_.cut=clear?1:0;if(!clear)fail(Reason::InitialCutUnknown);return clear;}
    std::shared_ptr<Payload> beginBoundary(std::uint64_t event){if(finished_||closing_||!boundary_callbacks_||!started_)return {};if(boundary_payload_){fail(Reason::EffectOrderUnsupported);return {};}auto p=std::make_shared<Payload>();p->boundary=true;p->owner=event;p->generation=generation_;p->pre=mirror_;boundary_payload_=p;return p;}
    void endBoundary(const std::shared_ptr<Payload>& p){if(finished_||!p||boundary_payload_!=p)return;if(p->first && p->first<last_emitted_ticket_){p->unsupported=true;fail(Reason::EffectOrderUnsupported);}p->sealed=true;boundary_payload_.reset();}
    bool boundMetadataPending()const{return !pending_.empty();}
    void boundaryPlaced(std::uint64_t first,std::uint64_t last){if(finished_)return;if(first && first<last_emitted_ticket_)fail(Reason::EffectOrderUnsupported);last_emitted_ticket_=std::max(last_emitted_ticket_,last);}
    void releaseBoundaryClose(){if(finished_)return;hold_boundary_close_=false;if(closing_&&pending_.empty())finish();}
    void registerEndpoint(const void *p, Domain d) {
        if (finished_)
            return;
        auto old = endpoint(p);
        if (old.observer && old.observer != this) {
            fail(Reason::CrossContextEffect);
            old.observer->fail(Reason::CrossContextEffect);
            return;
        }
        registry()[p] = {this, d};
    }
    void unregisterEndpoint(const void *p) {
        auto i = registry().find(p);
        if (i != registry().end() && i->second.observer == this)
            registry().erase(i);
    }
    void edge(const void *from, const void *to) {
        if (!finished_)
            edges_.insert({from, to});
    }
    bool hasEdge(const void *from, const void *to) const { return edges_.count({from, to}); }
    void fail(Reason r) {
        if (!finished_)
            add(footer_.reasons[unsigned(r)]);
    }
    void lifecycle() {
        if (closing_ || finished_)
            return;
        lifecycle_pending_ = true;
        resumed_ = false;
        if (started_)
            fail(Reason::LifecycleInWindow);
    }
    void resume() {
        if (closing_ || finished_)
            return;
        if (!lifecycle_pending_)
            return;
        if (started_) {
            fail(Reason::LifecycleInWindow);
            return;
        }
        mirror_ = read_();
        if (generation_ == UINT32_MAX) {
            fail(Reason::CounterOverflow);
            return;
        }
        ++generation_;
        lifecycle_pending_ = false;
        resumed_ = true;
    }
    std::uint32_t generation() const { return generation_; }
    const Baseline &baseline() const { return baseline_; }
    const Footer &footer() const { return footer_; }
    bool ready() const { return finished_ && (footer_.status & Complete); }
    bool finished() const { return finished_; }
    bool acceptsEffects() const {
        if (finished_)
            return false;
        if (!closing_)
            return true;
        const auto *owner = OwnerScope::active;
        if (causeBlocksOwnership() || !owner || owner->observer != this)
            return false;
        const auto found = owners_.find({owner->generation, owner->owner});
        return found != owners_.end() && found->second->bound && !found->second->sealed &&
               !found->second->discarded;
    }
    void enterScope(ActiveScope &scope, const std::shared_ptr<Payload> &p) {
        scope.payload = &p;
        scope.previous = active_scopes_;
        active_scopes_ = &scope;
    }
    void leaveScope(ActiveScope &scope) {
        if (active_scopes_ == &scope)
            active_scopes_ = scope.previous;
    }
    void completeScope(const std::shared_ptr<Payload> &p) {
        if (!finished_ && p && !p->discarded)
            p->last = std::max(p->last, ticket_);
    }
    State snapshot() { return acceptsEffects() ? read_() : mirror_; }
    std::shared_ptr<Payload> payload() {
        if (!acceptsEffects())
            return {};
        const auto* boundary=BoundaryScope::active;
        if(boundary && boundary->observer==this && boundary->payload && !causeBlocksOwnership())return boundary->payload;
        auto s = boundary ? nullptr : OwnerScope::active;
        if (causeBlocksOwnership() || !s || s->observer != this) {
            if (s && s->observer && s->observer != this)
                fail(Reason::CrossContextEffect);
            auto p = std::make_shared<Payload>();
            p->cause = CauseScope::active ? CauseScope::active->cause : Reason::UnboundExternal;
            p->pre = mirror_;
            return p;
        }
        auto it = owners_.find({s->generation, s->owner});
        if (it != owners_.end())
            return it->second;
        if (owners_.size() >= max_owners_) {
            fail(Reason::OwnerOverflow);
            auto p = std::make_shared<Payload>();
            p->unsupported = true;
            p->pre = mirror_;
            return p;
        }
        auto p = std::make_shared<Payload>();
        p->owner = s->owner;
        p->generation = s->generation;
        p->pre = mirror_;
        owners_.emplace(OwnerKey{s->generation, s->owner}, p);
        return p;
    }
    bool owned(const std::shared_ptr<Payload> &p) const {
        if(p->boundary)return boundary_payload_==p;
        auto i = owners_.find({p->generation, p->owner});
        return i != owners_.end() && i->second == p;
    }
    std::uint32_t append(const std::shared_ptr<Payload> &p, Operation op) {
        if (finished_ || !p || p->discarded)
            return NoParent;
        if (closing_ && !p->bound) {
            fail(Reason::EffectOrderUnsupported);
            p->unsupported = true;
        }
        if (ticket_ == UINT64_MAX)
            fail(Reason::CounterOverflow);
        else
            ++ticket_;
        if (!p->first)
            p->first = ticket_;
        if (p->last && have_last_ &&
            (last_owner_ != p->owner || last_generation_ != p->generation || last_boundary_owner_!=p->boundary)) {
            fail(Reason::EffectOrderUnsupported);
            p->unsupported = true;
        }
        p->last = ticket_;
        last_boundary_owner_=p->boundary;
        last_owner_ = p->owner;
        last_generation_ = p->generation;
        have_last_ = true;
        if (std::holds_alternative<Write>(op))
            add(p->writes);
        else
            add(p->commands);
        if (p->ops.size() >= max_ops_) {
            fail(Reason::OperationOverflow);
            p->unsupported = true;
            return NoParent;
        }
        const auto index = std::uint32_t(p->ops.size());
        std::visit([&](auto &x) { x.index = index; }, op);
        p->ops.push_back(op);
        return index;
    }
    void updateMirror(const State &state) {
        if (!finished_)
            mirror_ = state;
    }
    void visit(const std::shared_ptr<Payload> &p) {
        if (!finished_ && p && !p->discarded)
            add(p->visits);
    }
    void diagnostic(const std::shared_ptr<Payload> &p, Reason reason) {
        if (finished_ || !p || p->discarded)
            return;
        if (!started_) {
            if (p->writes || p->commands)
                for (auto &entry : owners_) {
                    const auto &candidate = entry.second;
                    if (candidate != p && candidate->first && p->last >= candidate->first) {
                        auto &evidence = candidate->excluded;
                        add(evidence.writes, p->writes);
                        add(evidence.commands, p->commands);
                        add(evidence.visits, p->visits);
                        add(evidence.reasons[unsigned(reason)]);
                    }
                }
            return;
        }
        fail(reason);
        add(footer_.visits, p->visits);
        add(footer_.total_writes, p->writes);
        add(footer_.total_commands, p->commands);
        add(footer_.unbound_writes, p->writes);
        add(footer_.unbound_commands, p->commands);
        auto count = p->writes;
        if (!saturating_add(count, p->commands))
            fail(Reason::CounterOverflow);
        if (!count)
            return;
        if (p->ops.size() != count ||
            count > max_details_ - std::min<std::uint64_t>(max_details_, details_)) {
            add(footer_.suppressed, count);
            fail(Reason::DiagnosticTruncated);
            return;
        }
        Diagnostic d;
        d.context = context_;
        d.generation = generation_;
        d.reason = reason;
        d.count = p->ops.size();
        if(boundary_callbacks_)boundary_callbacks_->diagnostic(*p,reason);
        if (writer_)
            writer_->diagnostic(d, p->ops);
        add(footer_.groups);
        details_ += count;
    }
    void unbound(const std::shared_ptr<Payload> &p) {
        if (!owned(p))
            diagnostic(p, CauseScope::active ? CauseScope::active->cause : Reason::UnboundExternal);
    }
    void bind(std::uint64_t owner, std::uint64_t ordinal, const void *hot,
              std::uint32_t gen = UINT32_MAX) {
        if (closing_ || finished_)
            return;
        check(ordinal == footer_.hot, "hot ordinal");
        if (pending_.size() >= max_owners_) {
            fail(Reason::OwnerOverflow);
            abandonPending();
        }
        if (gen == UINT32_MAX)
            gen = generation_;
        auto i = owners_.find({gen, owner});
        std::shared_ptr<Payload> p;
        if (i != owners_.end())
            p = i->second;
        else {
            p = std::make_shared<Payload>();
            p->owner = owner;
            p->generation = gen;
            p->pre = read_();
            if (owners_.size() < max_owners_)
                owners_[{gen, owner}] = p;
            else {
                fail(Reason::OwnerOverflow);
                p->unsupported = true;
            }
        }
        if (p->bound) {
            fail(Reason::EffectOrderUnsupported);
            return;
        }
        if (started_ && p->generation != generation_) {
            fail(Reason::LifecycleInWindow);
            p->unsupported = true;
        }
        if (!started_) {
            if (p->generation != generation_) {
                fail(Reason::InitialCutUnknown);
                resumed_ = false;
            }
            baseline_.hash = row_hash(hot);
            baseline_.state = p->first ? p->pre : read_();
            baseline_.known = 15;
            baseline_.generation = generation_;
            baseline_.cut = resumed_ ? 1 : 0;
            if (!resumed_)
                fail(Reason::InitialCutUnknown);
            for (auto &kv : owners_)
                if (kv.second != p && kv.second->first) {
                    fail(Reason::InitialCutUnknown);
                    baseline_.cut = 0;
                }
            for (auto *scope = active_scopes_; scope; scope = scope->previous) {
                const auto &active = *scope->payload;
                if (active != p && active->first) {
                    fail(Reason::InitialCutUnknown);
                    baseline_.cut = 0;
                }
            }
            const auto &excluded = p->excluded;
            if (excluded.writes || excluded.commands) {
                fail(Reason::InitialCutUnknown);
                baseline_.cut = 0;
                add(footer_.total_writes, excluded.writes);
                add(footer_.unbound_writes, excluded.writes);
                add(footer_.total_commands, excluded.commands);
                add(footer_.unbound_commands, excluded.commands);
                add(footer_.visits, excluded.visits);
                // No unbounded prefix detail log: counts/reasons survive, and
                // absent details remain explicitly suppressed and uncertified.
                add(footer_.suppressed, excluded.writes);
                add(footer_.suppressed, excluded.commands);
                for (unsigned i = 0; i < 16; ++i)
                    add(footer_.reasons[i], excluded.reasons[i]);
                fail(Reason::DiagnosticTruncated);
            }
            started_ = true;
            if (writer_)
                writer_->baseline(baseline_);
        }
        p->bound = true;
        std::memcpy(p->hot.data(), hot, 64);
        p->member = {ordinal, row_hash(hot), context_, generation_, 0, 0};
        pending_[ordinal] = p;
        add(footer_.hot);
        footer_.hot_hash = hash_bytes(footer_.hot_hash, hot, 64);
        if (!owned(p))
            abandonPending();
    }
    void seal(std::uint64_t owner, std::uint32_t gen = UINT32_MAX) {
        if (finished_)
            return;
        if (gen == UINT32_MAX)
            gen = generation_;
        auto i = owners_.find({gen, owner});
        if (i == owners_.end())
            return;
        auto p = i->second;
        if (!p->bound) {
            if (p->writes || p->commands)
                diagnostic(p, Reason::FilteredInterior);
            owners_.erase(i);
            return;
        }
        if (p->member.hash != row_hash(p->hot.data()) || p->owner != owner) {
            fail(Reason::EffectOrderUnsupported);
            p->unsupported = true;
        }
        p->sealed = true;
        emit();
    }
    void destroy(std::uint64_t owner, std::uint32_t gen = UINT32_MAX) {
        if (finished_)
            return;
        if (gen == UINT32_MAX)
            gen = generation_;
        auto i = owners_.find({gen, owner});
        if (i == owners_.end())
            return;
        auto p = i->second;
        if (p->bound) {
            fail(Reason::UnsealedOwner);
            return;
        }
        if (p->writes || p->commands)
            diagnostic(p, Reason::EffectfulSquash);
        owners_.erase(i);
    }
    void close(std::uint64_t hot) {
        if (closing_)
            return;
        check(hot == footer_.hot, "close hot population");
        closing_ = true;
        if(boundary_payload_ && !boundary_payload_->discarded)discardCrossing(boundary_payload_,Reason::EffectOrderUnsupported);
        for (auto i = owners_.begin(); i != owners_.end();) {
            if (!i->second->bound) {
                if (i->second->writes || i->second->commands)
                    discardCrossing(i->second, Reason::EffectOrderUnsupported);
                i = owners_.erase(i);
            } else
                ++i;
        }
        for (auto *scope = active_scopes_; scope; scope = scope->previous) {
            const auto &p = *scope->payload;
            if (!p->bound)
                discardCrossing(p, p->cause);
        }
        emit();
    }
    void forceClose() {
        if (!closing_)
            close(footer_.hot);
        if (finished_)
            return;
        closing_ = true;
        for (auto &kv : pending_) {
            auto p = kv.second;
            fail(Reason::UnsealedOwner);
            diagnostic(p, Reason::UnsealedOwner);
            p->member.status = Unsupported;
            p->member.count = 0;
            if(boundary_callbacks_)boundary_callbacks_->retained(*p);
            if (writer_)
                writer_->member(p->member, {});
        }
        finish();
    }
};
inline BoundaryScope::BoundaryScope(Observer& o,const void* accepted_fault,const void* endpoint,std::uint64_t event,bool pf,bool enabled):observer(&o),fault(accepted_fault),tc(endpoint),page_fault(pf),previous(active){if(!previous&&enabled)payload=o.beginBoundary(event);else if(previous)o.fail(Reason::EffectOrderUnsupported);active=this;}
inline BoundaryScope::~BoundaryScope(){active=previous;if(observer&&payload)observer->endBoundary(payload);}
inline std::uint32_t generation(const void *p) {
    auto o = endpoint(p).observer;
    return o ? o->generation() : 0;
}
inline void unregisterEndpoint(const void *p) { registry().erase(p); }
inline void lifecycle(const void *p) {
    if (auto o = endpoint(p).observer)
        o->lifecycle();
}
inline void resume(const void *p) {
    if (auto o = endpoint(p).observer)
        o->resume();
}
inline void destroy(const void *p, std::uint64_t id, std::uint32_t gen = UINT32_MAX) {
    if (auto o = endpoint(p).observer)
        o->destroy(id, gen);
}
class WriteScope;
struct StoragePermit {
    const void *endpoint;
    Register reg;
    bool consumed = false;
    StoragePermit *previous;
    inline static thread_local StoragePermit *active = nullptr;
    StoragePermit(const void *p, Register r) : endpoint(p), reg(r), previous(active) {
        active = this;
    }
    StoragePermit(const StoragePermit &) = delete;
    StoragePermit &operator=(const StoragePermit &) = delete;
    ~StoragePermit() { active = previous; }
};
class WriteScope {
    Observer *observer_ = nullptr;
    std::shared_ptr<Payload> payload_;
    std::function<State()> read_;
    std::uint32_t index_ = NoParent;
    WriteScope *previous_ = nullptr;
    ActiveScope scope_;

  public:
    inline static thread_local WriteScope *active = nullptr;
    WriteScope(const void *endpoint_, Register reg, std::uint64_t requested, Mode mode,
               std::function<State()> read)
        : observer_(endpoint(endpoint_).observer), read_(std::move(read)) {
        if (!observer_ || !observer_->acceptsEffects()) {
            observer_ = nullptr;
            return;
        }
        if (mode == Mode::NoEffect && StoragePermit::active &&
            StoragePermit::active->endpoint == endpoint_ && StoragePermit::active->reg == reg &&
            !StoragePermit::active->consumed) {
            StoragePermit::active->consumed = true;
            observer_ = nullptr;
            return;
        }
        auto state = read_();
        observer_->updateMirror(state);
        payload_ = observer_->payload();
        Write w;
        w.reg = reg;
        w.requested = requested;
        w.mode = mode;
        w.before = state;
        w.before_known = 15;
        w.application =
            !BoundaryScope::active && OwnerScope::active && OwnerScope::active->observer == observer_ && !causeBlocksOwnership()
                ? OwnerScope::active->application
                : (BoundaryScope::active && BoundaryScope::active->observer==observer_ && BoundaryScope::active->payload && !causeBlocksOwnership() ? Application::Immediate : Application::External);
        previous_ = active;
        if (previous_) {
            observer_->fail(Reason::EffectOrderUnsupported);
            payload_->unsupported = true;
        }
        index_ = observer_->append(payload_, w);
        observer_->enterScope(scope_, payload_);
        active = this;
    }
    WriteScope(const WriteScope &) = delete;
    WriteScope &operator=(const WriteScope &) = delete;
    ~WriteScope() {
        if (!observer_)
            return;
        active = previous_;
        if (observer_->finished() || payload_->discarded) {
            observer_->leaveScope(scope_);
            return;
        }
        observer_->completeScope(payload_);
        auto after = read_();
        observer_->updateMirror(after);
        if (index_ != NoParent) {
            auto &w = std::get<Write>(payload_->ops[index_]);
            w.after = after;
            w.after_known = 15;
        }
        observer_->unbound(payload_);
        observer_->leaveScope(scope_);
    }
    Observer *observer() const { return observer_; }
    const std::shared_ptr<Payload> &payload() const { return payload_; }
    std::uint32_t index() const { return index_; }
};
class CommandScope;
struct EdgeScope {
    Observer *observer = nullptr;
    const void *parent = nullptr;
    const void *child = nullptr;
    CommandKind kind = CommandKind::FlushAll;
    std::uint64_t va = 0, asn = 0;
    bool consumed = false;
    CommandScope *command = nullptr;
    EdgeScope *previous;
    inline static thread_local EdgeScope *active = nullptr;
    EdgeScope(CommandScope &, const void *, const void *);
    EdgeScope(const EdgeScope &) = delete;
    EdgeScope &operator=(const EdgeScope &) = delete;
    ~EdgeScope() { active = previous; }
};
class CommandScope {
    Observer *observer_ = nullptr;
    std::shared_ptr<Payload> payload_;
    bool origin_ = false, unbound_ = false, depth_counted_ = false;
    inline static thread_local unsigned depth_ = 0;
    ActiveScope scope_;

  public:
    CommandKind kind;
    std::uint64_t va, asn;
    CommandScope(const void *p, CommandKind k, std::uint64_t v = 0, std::uint64_t a = 0,
                 bool tlb = false)
        : observer_(endpoint(p).observer), kind(k), va(v), asn(a) {
        auto e = EdgeScope::active;
        const bool unknown_endpoint = !observer_ && e;
        if (unknown_endpoint)
            observer_ = e->observer;
        if (!observer_ || !observer_->acceptsEffects()) {
            observer_ = nullptr;
            return;
        }
        if (unknown_endpoint)
            observer_->fail(Reason::PropagationUnknown);
        depth_counted_ = true;
        if (++depth_ > 64)
            observer_->fail(Reason::PropagationUnknown);
        if (tlb && e && !e->consumed && e->observer == observer_ && e->child == p && e->kind == k &&
            e->va == v && e->asn == a && observer_->hasEdge(e->parent, p)) {
            e->consumed = true;
            payload_ = e->command->payload_;
            observer_->visit(payload_);
            observer_->enterScope(scope_, payload_);
            return;
        }
        if (tlb && e && !e->consumed)
            observer_->fail(Reason::PropagationUnknown);
        origin_ = true;
        auto w = WriteScope::active;
        if (w && w->observer() == observer_ && !causeBlocksOwnership()) {
            payload_ = w->payload();
        } else {
            payload_ = observer_->payload();
            unbound_ = true;
        }
        Command c;
        c.kind = k;
        c.va = v;
        c.asn = a;
        c.domain = tlb ? endpoint(p).domain : Domain::ContextAllTranslations;
        if (w && w->observer() == observer_ && payload_ == w->payload())
            c.parent = w->index();
        if (tlb) {
            observer_->fail(Reason::DomainUnsupported);
            payload_->unsupported = true;
        }
        observer_->append(payload_, c);
        observer_->enterScope(scope_, payload_);
    }
    CommandScope(const CommandScope &) = delete;
    CommandScope &operator=(const CommandScope &) = delete;
    ~CommandScope() {
        if (depth_counted_)
            --depth_;
        if (observer_) {
            observer_->completeScope(payload_);
            if (origin_ && unbound_)
                observer_->unbound(payload_);
            observer_->leaveScope(scope_);
        }
    }
    Observer *observer() const { return observer_; }
    friend struct EdgeScope;
};
inline EdgeScope::EdgeScope(CommandScope &c, const void *p, const void *child_)
    : observer(c.observer()), parent(p), child(child_), kind(c.kind), va(c.va), asn(c.asn),
      command(&c), previous(active) {
    active = this;
    if (observer && !observer->acceptsEffects())
        observer = nullptr;
    if (observer && !observer->hasEdge(parent, child))
        observer->fail(Reason::PropagationUnknown);
}
} // namespace gem5::functional_mmu
