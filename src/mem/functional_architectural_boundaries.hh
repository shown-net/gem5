#ifndef GEM5_FUNCTIONAL_ARCHITECTURAL_BOUNDARIES_HH
#define GEM5_FUNCTIONAL_ARCHITECTURAL_BOUNDARIES_HH
// Pure functional observer. Native object identities are synchronous
// capability keys only; no native instruction, fault, timing or scheduling
// object is owned.
#include <functional>
#include <optional>
#include "fst_architectural_boundaries.hh"
#include "fst_mmu_controls_v2.hh"
#include "functional_mmu_controls.hh"
namespace gem5::functional_arch
{
namespace wire = fastsim::fst::archb;
namespace controls = fastsim::fst::mmuctl_v2;
using State = fastsim::fst::mmuctl::State;
class BoundaryToken
{
    std::uint32_t context_ = 0, generation_ = 0;
    std::uint64_t event_ = 0;

  public:
    BoundaryToken() = default;
    BoundaryToken(std::uint32_t c, std::uint32_t g, std::uint64_t e)
        : context_(c), generation_(g), event_(e)
    {}
    std::uint32_t
    context() const
    {
        return context_;
    }
    std::uint32_t
    generation() const
    {
        return generation_;
    }
    std::uint64_t
    event() const
    {
        return event_;
    }
    bool
    valid() const
    {
        return generation_ && event_;
    }
    bool
    operator==(const BoundaryToken &b) const
    {
        return context_ == b.context_ && generation_ == b.generation_ &&
               event_ == b.event_;
    }
    bool
    operator!=(const BoundaryToken &b) const
    {
        return !(*this == b);
    }
};
// This source identity never goes on wire. Restart copies it; a fresh dynamic
// expansion obtains a fresh identity even at the same architectural PC.
class MacroOrigin
{
    std::uint32_t context_ = 0, generation_ = 0;
    std::uint64_t serial_ = 0;

  public:
    MacroOrigin() = default;
    MacroOrigin(std::uint32_t c, std::uint32_t g, std::uint64_t n)
        : context_(c), generation_(g), serial_(n)
    {}
    bool
    valid() const
    {
        return generation_ && serial_;
    }
    bool
    operator==(const MacroOrigin &b) const
    {
        return context_ == b.context_ && generation_ == b.generation_ &&
               serial_ == b.serial_;
    }
};
struct AcceptedFacts
{
    wire::Cause cause = wire::Cause::OtherFault;
    std::uint16_t vector = 65535;
    std::uint8_t mode = 0, carrier = 0;
    std::uint64_t pc = 0, micro_pc = 0, fault_va = 0, error_code = 0;
    bool pc_known = false, exact_cause = false;
};
class Observer;
inline std::map<const void *, Observer *> &
registry()
{
    static std::map<const void *, Observer *> r;
    return r;
}
inline Observer *
observer(const void *tc)
{
    auto it = registry().find(tc);
    return it == registry().end() ? nullptr : it->second;
}
class AcceptedInvocationScope
{
    Observer *observer_ = nullptr;
    const void *fault_ = nullptr;
    const void *tc_ = nullptr;
    AcceptedInvocationScope *previous_ = nullptr;
    BoundaryToken token_;
    std::unique_ptr<functional_mmu::BoundaryScope> controls_;
    bool sealed_ = false;

  public:
    inline static thread_local AcceptedInvocationScope *active = nullptr;
    AcceptedInvocationScope() = default;
    AcceptedInvocationScope(Observer &, const void *, const void *,
                            BoundaryToken, bool);
    AcceptedInvocationScope(const AcceptedInvocationScope &) = delete;
    AcceptedInvocationScope &
    operator=(const AcceptedInvocationScope &) = delete;
    AcceptedInvocationScope(AcceptedInvocationScope &&) = delete;
    ~AcceptedInvocationScope();
    BoundaryToken
    token() const
    {
        return token_;
    }
    BoundaryToken redirect(const void *fault, const void *tc,
                           std::uint64_t micro_pc, std::uint16_t entry,
                           std::uint64_t vector, std::uint64_t ip,
                           std::uint64_t error);
    void seal();
};
class Observer
{
    friend class AcceptedInvocationScope;
    struct Event
    {
        wire::Accepted accepted;
        wire::InvocationSeal invocation;
        wire::Terminal terminal;
        bool invocation_sealed = false, invocation_returned = false,
             predecessor_sealed = false, span = false, exit = false,
             supported = false, prefix_span = true, advance_seen = false,
             head = false, span_complete = false;
        std::uint64_t predecessor_owner = 0, terminal_owner = 0,
                      expected_entry = 0, last_ordinal = wire::Absent;
        std::uint64_t hot_hash = wire::HashSeed;
        std::map<std::uint64_t, std::uint64_t> bound_members;
    };
    struct Retained
    {
        std::uint64_t owner = 0, ordinal = 0, hash = 0, macro_begin = 0;
        MacroOrigin origin;
        bool sealed = false, last = false;
    };
    std::uint32_t context_, generation_ = 0;
    const void *tc_;
    functional_mmu::Observer &mmu_;
    std::function<State()> read_;
    std::function<std::uint64_t()> cr2_;
    std::unique_ptr<wire::Writer> writer_;
    std::unique_ptr<controls::Writer> control_writer_;
    std::map<std::uint64_t, Event> events_;
    std::optional<Retained> predecessor_;
    struct Span
    {
        std::uint64_t begin = 0, end = 0;
        bool complete = false;
    };
    std::optional<Span> last_span_;
    wire::Epoch epoch_;
    wire::Footer footer_;
    controls::Footer control_footer_;
    struct PendingControl
    {
        controls::Owner owner;
        std::vector<fastsim::fst::mmuctl::Operation> operations;
        std::uint64_t predecessor = wire::Absent, first_ticket = 0,
                      last_ticket = 0;
    };
    std::map<std::uint64_t, PendingControl> pending_controls_;
    std::uint64_t next_control_event_ = 1, retained_emitted_ = 0;
    std::uint64_t next_event_ = 0, next_macro_ = 0, next_diagnostic_ = 0;
    void
    emitControls()
    {
        for (;;) {
            auto it = pending_controls_.find(next_control_event_);
            if (it == pending_controls_.end()) {
                return;
            }
            auto &p = it->second;
            if (p.predecessor != wire::Absent &&
                p.predecessor >= retained_emitted_) {
                return;
            }
            mmu_.boundaryPlaced(p.first_ticket, p.last_ticket);
            if (control_writer_) {
                control_writer_->owner(p.owner, p.operations);
            }
            pending_controls_.erase(it);
            ++next_control_event_;
        }
    }
    bool armed_ = false, completed_takeover_ = false, executed_ = false,
         closing_ = false, finished_ = false, lifecycle_bad_ = false,
         core_unknown_ = false;
    void
    fail(wire::Reason r)
    {
        if (!finished_) {
            fastsim::fst::mmuctl::saturating_add(footer_.reasons[unsigned(r)]);
        }
    }
    void
    count(std::uint64_t &value, std::uint64_t n = 1)
    {
        if (n > UINT64_MAX - value) {
            value = UINT64_MAX;
            fail(wire::Reason::CounterOverflow);
        } else {
            value += n;
        }
    }
    Event *
    find(BoundaryToken t)
    {
        if (!t.valid() || t.context() != context_ ||
            t.generation() != generation_) {
            return nullptr;
        }
        auto it = events_.find(t.event());
        return it == events_.end() ? nullptr : &it->second;
    }
    void
    writeRetained(const functional_mmu::Payload &p)
    {
        if (finished_) {
            return;
        }
        emitControls();
        controls::Owner o;
        o.owner_kind = controls::OwnerKind::RetiredInstruction;
        o.owner_id = p.member.ordinal;
        o.hot_hash = p.member.hash;
        o.context = context_;
        o.generation = generation_;
        o.status = p.member.status;
        const auto ops = p.member.count == p.ops.size()
                             ? p.ops
                             : std::vector<fastsim::fst::mmuctl::Operation>{};
        o.count = static_cast<std::uint32_t>(ops.size());
        o.op_hash = controls::operation_hash(ops);
        if (control_writer_) {
            control_writer_->owner(o, ops);
        }
        ++retained_emitted_;
        emitControls();
        count(control_footer_.retained_groups);
        count(control_footer_.sealed_retired,
              bool(o.status & controls::Sealed));
        count(control_footer_.unsupported_groups,
              bool(o.status & controls::Unsupported));
    }
    void
    writeDiagnostic(const functional_mmu::Payload &p,
                    fastsim::fst::mmuctl::Reason reason)
    {
        if (finished_) {
            return;
        }
        controls::Owner o;
        o.owner_kind = controls::OwnerKind::Unbound;
        o.owner_id = ++next_diagnostic_;
        o.context = context_;
        o.generation = generation_;
        o.status = controls::Unsupported;
        o.reason = unsigned(reason);
        o.count = static_cast<std::uint32_t>(p.ops.size());
        o.op_hash = controls::operation_hash(p.ops);
        if (control_writer_) {
            control_writer_->owner(o, p.ops);
        }
        count(control_footer_.unbound_groups);
        count(control_footer_.unsupported_groups);
    }
    void
    mmuFinished(const fastsim::fst::mmuctl::Footer &f)
    {
        if (finished_) {
            return;
        }
        control_footer_.hot = f.hot;
        control_footer_.hot_hash = f.hot_hash;
        control_footer_.total_writes =
            f.total_writes + control_footer_.boundary_writes;
        control_footer_.retained_writes = f.retained_writes;
        control_footer_.unbound_writes = f.unbound_writes;
        control_footer_.total_commands =
            f.total_commands + control_footer_.boundary_commands;
        control_footer_.retained_commands = f.retained_commands;
        control_footer_.unbound_commands = f.unbound_commands;
        count(control_footer_.visits, f.visits);
        control_footer_.suppressed_ops = f.suppressed;
        control_footer_.pending_owners = f.pending;
        control_footer_.reasons = f.reasons;
    }
    void
    emitTerminal(std::uint64_t id, bool forced)
    {
        auto it = events_.find(id);
        if (it == events_.end()) {
            return;
        }
        auto &e = it->second;
        auto &t = e.terminal;
        if (!forced &&
            !(e.invocation_sealed && e.predecessor_sealed && e.exit &&
              e.span && e.prefix_span && e.bound_members.empty())) {
            return;
        }
        t.event = id;
        t.context = context_;
        t.generation = generation_;
        t.predecessor_ordinal = e.accepted.predecessor_ordinal;
        t.predecessor_hash = e.accepted.predecessor_hash;
        t.member_hot_hash = e.hot_hash;
        t.seal_flags =
            (e.predecessor_sealed ? 1 : 0) | (e.invocation_sealed ? 2 : 0) |
            (e.head ? 4 : 0) | (e.exit ? 8 : 0) |
            (t.members == t.sealed_members ? 16 : 0) | (e.span ? 32 : 0) |
            ((e.accepted.prefix_flags == 3 && e.prefix_span) ? 64 : 0);
        t.reason_mask |= static_cast<std::uint32_t>(e.accepted.reason_mask);
        if (!e.prefix_span) {
            t.reason_mask |= wire::reason(wire::Reason::PrefixUnknown);
        }
        if (!e.invocation_sealed) {
            t.reason_mask |= wire::reason(wire::Reason::NoRedirect);
        }
        if (!e.predecessor_sealed) {
            t.reason_mask |= wire::reason(wire::Reason::PredecessorUnknown);
        }
        if (!t.members) {
            t.reason_mask |= wire::reason(wire::Reason::MissingEntryHead);
        }
        if (e.exit && !e.span_complete) {
            t.reason_mask |= wire::reason(wire::Reason::EntryDiscontinuity);
        }
        if (!e.exit) {
            t.reason_mask |= wire::reason(wire::Reason::MissingExit);
        }
        if (!e.bound_members.empty()) {
            t.reason_mask |= wire::reason(wire::Reason::LifecycleMismatch);
        }
        t.status = !t.reason_mask && (t.seal_flags & 63) == 63 ? 1
                   : e.supported                               ? 2
                                                               : 3;
        if (t.status == 1) {
            count(footer_.terminal_complete);
        } else if (t.status == 2) {
            count(footer_.terminal_partial);
        } else {
            count(footer_.terminal_unsupported);
        }
        for (unsigned i = 0; i < 16; ++i) {
            if (t.reason_mask & (1u << i)) {
                count(footer_.reasons[i]);
            }
        }
        if (!e.invocation_sealed && id >= next_control_event_ &&
            !pending_controls_.count(id)) {
            PendingControl p;
            p.owner.owner_kind = controls::OwnerKind::ArchitecturalBoundary;
            p.owner.owner_id = id;
            p.owner.context = context_;
            p.owner.generation = generation_;
            p.owner.status = controls::Partial;
            p.owner.op_hash = controls::operations_seed();
            p.predecessor = e.accepted.predecessor_ordinal;
            pending_controls_.emplace(id, std::move(p));
            count(control_footer_.boundary_groups);
            count(control_footer_.unsupported_groups);
            emitControls();
        }
        if (writer_) {
            writer_->terminal(t);
        }
        events_.erase(it);
    }
    void
    maybeFinish()
    {
        if (!closing_ || finished_ || mmu_.boundMetadataPending()) {
            return;
        }
        for (const auto &[id, e] : events_) {
            (void)id;
            if (!e.invocation_returned || !e.bound_members.empty() ||
                (e.terminal.members && !e.span)) {
                return;
            }
            if (e.terminal_owner && !e.advance_seen) {
                return;
            }
        }
        while (!events_.empty()) {
            emitTerminal(events_.begin()->first, true);
        }
        mmu_.releaseBoundaryClose();
        footer_.status =
            wire::Finalized |
            ((!footer_.hot && !footer_.accepted) ? wire::Empty : 0);
        bool complete = epoch_.proof_flags == 15 && !lifecycle_bad_ &&
                        !footer_.terminal_partial &&
                        !footer_.terminal_unsupported &&
                        !footer_.suppressed_events && !core_unknown_;
        for (auto n : footer_.reasons) {
            complete &= !n;
        }
        for (auto n : control_footer_.reasons) {
            complete &= !n;
        }
        if (complete) {
            footer_.status |= wire::Complete;
        }
        control_footer_.status = footer_.status;
        control_footer_.event_rows = footer_.event_rows;
        if (control_writer_) {
            control_writer_->finish(control_footer_);
        }
        if (writer_) {
            writer_->finish(footer_);
        }
        finished_ = true;
    }

  public:
    Observer(std::uint32_t context, const void *tc,
             functional_mmu::Observer &mmu, std::function<State()> read,
             std::function<std::uint64_t()> cr2)
        : context_(context),
          tc_(tc),
          mmu_(mmu),
          read_(std::move(read)),
          cr2_(std::move(cr2))
    {
        fastsim::fst::mmuctl::check(!registry().count(tc),
                                    "duplicate architectural observer");
        registry()[tc] = this;
        functional_mmu::BoundaryCallbacks callbacks;
        callbacks.retained = [this](const auto &p) { writeRetained(p); };
        callbacks.diagnostic = [this](const auto &p, auto r) {
            writeDiagnostic(p, r);
        };
        callbacks.finished = [this](const auto &f) { mmuFinished(f); };
        mmu_.configureBoundaries(std::move(callbacks));
    }
    ~Observer()
    {
        auto it = registry().find(tc_);
        if (it != registry().end() && it->second == this) {
            registry().erase(it);
        }
    }
    void
    attach(const std::string &hot, wire::Header h)
    {
        fastsim::fst::mmuctl::check(!armed_ && !writer_,
                                    "architectural attach order");
        h.context = context_;
        writer_ = std::make_unique<wire::Writer>(hot + ".archb.gz", h);
        controls::Header m;
        m.context = context_;
        m.policy = h.policy;
        m.coverage = h.coverage;
        control_writer_ =
            std::make_unique<controls::Writer>(hot + ".mmuctl.gz", m);
    }
    void
    completedTakeover()
    {
        if (finished_ || closing_) {
            return;
        }
        if (armed_) {
            lifecycle_bad_ = true;
            fail(wire::Reason::LifecycleMismatch);
            return;
        }
        completed_takeover_ = true;
        executed_ = false;
    }
    void
    executed()
    {
        if (!finished_ && !closing_) {
            executed_ = true;
        }
    }
    void
    lifecycle()
    {
        if (finished_ || closing_) {
            return;
        }
        completed_takeover_ = false;
        if (armed_) {
            lifecycle_bad_ = true;
            fail(wire::Reason::LifecycleMismatch);
        }
    }
    void
    arm(std::uint64_t pc, std::uint64_t micro_pc)
    {
        if (finished_ || closing_) {
            return;
        }
        if (armed_) {
            lifecycle_bad_ = true;
            fail(wire::Reason::LifecycleMismatch);
            return;
        }
        generation_ = mmu_.generation();
        if (!generation_) {
            generation_ = 1;
        }
        epoch_.context = context_;
        epoch_.generation = generation_;
        epoch_.state = read_();
        epoch_.state_known = 15;
        epoch_.pc = pc;
        epoch_.micro_pc = micro_pc;
        const bool mmu_clear = mmu_.armBoundaryEpoch(epoch_.state);
        if (completed_takeover_ && !executed_ && !predecessor_ &&
            !(micro_pc & 0x8000) && mmu_clear) {
            epoch_.proof_flags = 15;
        } else {
            fail(wire::Reason::InitialEpochUnknown);
        }
        armed_ = true;
        footer_.epoch_rows = 1;
        if (writer_) {
            writer_->epoch(epoch_);
        }
        controls::Baseline b;
        b.context = context_;
        b.generation = generation_;
        b.cut_kind = epoch_.proof_flags == 15 ? 1 : 0;
        b.known = 15;
        b.proof_flags = epoch_.proof_flags;
        b.state = epoch_.state;
        b.epoch_reference = generation_;
        if (control_writer_) {
            control_writer_->baseline(b);
        }
    }
    MacroOrigin
    newMacroOrigin()
    {
        if (finished_ || closing_ || !armed_) {
            return {};
        }
        if (next_macro_ == UINT64_MAX) {
            fail(wire::Reason::CounterOverflow);
            return {};
        }
        return MacroOrigin(context_, generation_, ++next_macro_);
    }
    AcceptedInvocationScope
    beginAccepted(const void *fault, const AcceptedFacts &facts,
                  MacroOrigin origin)
    {
        if (finished_ || closing_) {
            return {};
        }
        if (!armed_) {
            arm(facts.pc, facts.micro_pc);
        }
        count(footer_.accepted);
        if (facts.cause == wire::Cause::PageFault) {
            count(footer_.accepted_pf);
        } else if (facts.cause == wire::Cause::ExternalInterrupt) {
            count(footer_.accepted_irq);
        } else {
            count(footer_.accepted_other);
        }
        if (next_event_ == UINT64_MAX || events_.size() >= 32) {
            count(footer_.suppressed_events);
            fail(wire::Reason::Overflow);
            return AcceptedInvocationScope(*this, fault, tc_, {}, false);
        }
        const auto id = ++next_event_;
        Event e;
        auto &a = e.accepted;
        a.event = id;
        a.context = context_;
        a.generation = generation_;
        a.cause = facts.cause;
        a.vector = facts.vector;
        a.mode = facts.mode;
        a.carrier = facts.carrier;
        a.pc = facts.pc_known ? facts.pc : 0;
        a.micro_pc = facts.pc_known ? facts.micro_pc : 0;
        a.known =
            1 | (facts.pc_known ? 2 : 0) | (facts.exact_cause ? 4 : 0) | 8;
        a.epoch_generation = generation_;
        a.controls_pre = read_();
        e.supported = facts.exact_cause &&
                      (facts.cause == wire::Cause::PageFault ||
                       facts.cause == wire::Cause::ExternalInterrupt) &&
                      facts.mode == 1;
        if (!facts.exact_cause ||
            (facts.cause != wire::Cause::PageFault &&
             facts.cause != wire::Cause::ExternalInterrupt)) {
            a.reason_mask |= wire::reason(wire::Reason::UnsupportedCause);
        }
        if (facts.mode != 1) {
            a.reason_mask |= wire::reason(wire::Reason::UnsupportedMode);
        }
        if (!facts.pc_known) {
            a.reason_mask |= wire::reason(wire::Reason::LifecycleMismatch);
        }
        if (lifecycle_bad_) {
            a.reason_mask |= wire::reason(wire::Reason::PredecessorUnknown);
        }
        if (epoch_.proof_flags != 15) {
            a.reason_mask |= wire::reason(wire::Reason::InitialEpochUnknown);
        }
        if (!events_.empty()) {
            a.reason_mask |= wire::reason(wire::Reason::Nested);
            a.fact_flags |= 16;
            a.parent_event = events_.rbegin()->first;
            e.supported = false;
        }
        if (facts.cause == wire::Cause::PageFault) {
            a.fact_flags |= 7;
            a.fault_va = facts.fault_va;
            a.error_code = facts.error_code;
            a.cr2_before = cr2_();
        }
        if (predecessor_) {
            const auto &p = *predecessor_;
            a.predecessor_kind = 1;
            a.predecessor_ordinal = p.ordinal;
            a.predecessor_hash = p.hash;
            e.predecessor_owner = p.owner;
            e.predecessor_sealed = p.sealed;
            if (!p.sealed) {
                a.fact_flags |= 8;
            }
            if (!p.last) {
                a.prefix_flags = 1;
                a.prefix_begin = p.macro_begin;
                a.prefix_end = p.ordinal + 1;
                e.prefix_span = last_span_ && !last_span_->complete &&
                                last_span_->begin == a.prefix_begin &&
                                last_span_->end == a.prefix_end;
                if (origin.valid() && p.origin.valid() && origin == p.origin) {
                    a.prefix_flags |= 2;
                } else {
                    a.reason_mask |= wire::reason(wire::Reason::PrefixUnknown);
                }
            }
        } else {
            a.predecessor_kind =
                epoch_.proof_flags == 15 && !lifecycle_bad_ ? 2 : 0;
            e.predecessor_sealed = a.predecessor_kind == 2;
        }
        e.terminal.event = id;
        e.terminal.context = context_;
        e.terminal.generation = generation_;
        e.terminal.control_op_hash = controls::operations_seed();
        events_.emplace(id, e);
        count(footer_.event_rows);
        if (writer_) {
            writer_->accepted(a);
        }
        return AcceptedInvocationScope(
            *this, fault, tc_, BoundaryToken(context_, generation_, id),
            e.supported);
    }
    void
    successfulHot(std::uint64_t owner, const void *raw, std::uint64_t ordinal,
                  std::uint64_t macro_begin, MacroOrigin origin,
                  bool macro_last, BoundaryToken token, std::uint64_t micro_pc,
                  std::uint8_t rom_kind, bool actual_head,
                  bool hardware_rom = true)
    {
        if (finished_ || closing_) {
            return;
        }
        if (!armed_) {
            arm(0, 0);
        }
        if (ordinal != footer_.hot) {
            fail(wire::Reason::EntryDiscontinuity);
            return;
        }
        count(footer_.hot);
        footer_.hot_hash = wire::hash_bytes(footer_.hot_hash, raw, 64);
        predecessor_ =
            Retained{owner,  ordinal, wire::row_hash(raw), macro_begin,
                     origin, false,   macro_last};
        auto *e = find(token);
        if (!e) {
            return;
        }
        if (!hardware_rom) {
            e->supported = false;
            e->terminal.reason_mask |=
                wire::reason(wire::Reason::LifecycleMismatch);
            return;
        }
        if (!rom_kind || rom_kind == 5) {
            e->supported = false;
            e->terminal.reason_mask |=
                wire::reason(wire::Reason::UnsupportedCause);
        }
        wire::EntryMember m;
        m.event = token.event();
        m.context = context_;
        m.generation = generation_;
        m.ordinal = ordinal;
        m.hot_hash = wire::row_hash(raw);
        m.macro_begin = macro_begin;
        m.micro_pc = micro_pc;
        m.rom_kind = rom_kind;
        m.member_flags = (actual_head ? 1 : 0) | (rom_kind == 4 ? 2 : 0);
        if (!e->terminal.members) {
            e->head = actual_head && micro_pc == e->expected_entry;
            if (!actual_head || micro_pc != e->expected_entry) {
                e->terminal.reason_mask |=
                    wire::reason(wire::Reason::MissingEntryHead);
            }
            e->terminal.entry_begin = macro_begin;
            e->terminal.first_hash = m.hot_hash;
            count(footer_.entry_bound);
        } else if (ordinal != e->last_ordinal + 1) {
            e->terminal.reason_mask |=
                wire::reason(wire::Reason::EntryDiscontinuity);
        }
        e->last_ordinal = ordinal;
        e->terminal.entry_end = ordinal + 1;
        e->terminal.last_hash = m.hot_hash;
        ++e->terminal.members;
        e->hot_hash = wire::hash_bytes(e->hot_hash, raw, 64);
        if (e->bound_members.size() >= 4096) {
            e->terminal.reason_mask |= wire::reason(wire::Reason::Overflow);
            fail(wire::Reason::Overflow);
        } else {
            e->bound_members.emplace(owner, ordinal);
        }
        if (rom_kind == 4) {
            e->terminal_owner = owner;
        }
        count(footer_.member_rows);
        if (e->supported) {
            count(footer_.na_rom_rows);
        }
        if (writer_) {
            writer_->entry_member(m);
        }
    }
    void
    postUpdateSeal(std::uint64_t owner)
    {
        if (finished_) {
            return;
        }
        if (predecessor_ && predecessor_->owner == owner) {
            predecessor_->sealed = true;
        }
        std::vector<std::uint64_t> ids;
        for (auto &[id, e] : events_) {
            if (e.predecessor_owner == owner) {
                e.predecessor_sealed = true;
            }
            auto it = e.bound_members.find(owner);
            if (it != e.bound_members.end()) {
                e.bound_members.erase(it);
                count(e.terminal.sealed_members);
                count(footer_.sealed_members);
            }
            ids.push_back(id);
        }
        for (auto id : ids) {
            emitTerminal(id, false);
        }
        maybeFinish();
    }
    void
    postAdvance(std::uint64_t owner, std::uint64_t successor_micro_pc,
                BoundaryToken successor)
    {
        if (finished_) {
            return;
        }
        std::vector<std::uint64_t> ids;
        for (auto &[id, e] : events_) {
            if (e.terminal_owner == owner) {
                e.advance_seen = true;
                if (!(successor_micro_pc & 0x8000) && !successor.valid()) {
                    e.exit = true;
                } else {
                    e.terminal.reason_mask |=
                        wire::reason(wire::Reason::MissingExit);
                }
                ids.push_back(id);
            }
        }
        for (auto id : ids) {
            emitTerminal(id, false);
        }
        maybeFinish();
    }
    template <class Macro>
    void
    macroSpan(const Macro &m, bool complete = true)
    {
        if (finished_) {
            return;
        }
        last_span_ = Span{m.begin_record, m.end_record, complete};
        for (auto &[id, e] : events_) {
            (void)id;
            if ((e.accepted.prefix_flags & 1) && !complete &&
                m.begin_record == e.accepted.prefix_begin &&
                m.end_record == e.accepted.prefix_end) {
                e.prefix_span = true;
            }
            if (e.terminal.members &&
                e.terminal.entry_begin == m.begin_record) {
                e.span = e.terminal.entry_end == m.end_record;
                e.span_complete = complete;
                e.terminal.ffe_semantic_hash = m.semantic_identity;
                e.terminal.ffe_code_hash = m.code_identity;
                if (!e.span) {
                    e.terminal.reason_mask |=
                        wire::reason(wire::Reason::EntryDiscontinuity);
                }
            }
        }
        maybeFinish();
    }
    void
    coreFactsUnknown(bool unknown)
    {
        if (!finished_ && !closing_) {
            core_unknown_ |= unknown;
        }
    }
    void
    omitted()
    {
        if (!finished_ && !closing_) {
            lifecycle_bad_ = true;
            fail(wire::Reason::EntryDiscontinuity);
        }
    }
    bool
    bound(BoundaryToken t)
    {
        return find(t) != nullptr;
    }
    bool
    qualified(BoundaryToken t)
    {
        auto *e = find(t);
        return e && e->supported && e->expected_entry;
    }
    void
    closeAdmission()
    {
        if (finished_ || closing_) {
            return;
        }
        closing_ = true;
        mmu_.close(footer_.hot);
        maybeFinish();
    }
    void
    forceClose()
    {
        if (finished_) {
            return;
        }
        if (!armed_) {
            arm(0, 0);
        }
        closing_ = true;
        while (!events_.empty()) {
            emitTerminal(events_.begin()->first, true);
        }
        mmu_.forceClose();
        maybeFinish();
    }
    bool
    finished() const
    {
        return finished_;
    }
    bool
    accepting() const
    {
        return !finished_ && !closing_;
    }
    const wire::Footer &
    coverage() const
    {
        return footer_;
    }
};
class CommitClassification
{
    Observer *observer_;
    std::uint64_t before_;

  public:
    explicit CommitClassification(Observer *o)
        : observer_(o), before_(o && o->accepting() ? o->coverage().hot : 0)
    {}
    ~CommitClassification()
    {
        if (observer_ && observer_->accepting() &&
            observer_->coverage().hot == before_) {
            observer_->omitted();
        }
    }
};
inline AcceptedInvocationScope::AcceptedInvocationScope(Observer &observer,
                                                        const void *fault,
                                                        const void *tc,
                                                        BoundaryToken token,
                                                        bool supported)
    : observer_(&observer),
      fault_(fault),
      tc_(tc),
      previous_(active),
      token_(token)
{
    active = this;
    controls_ = std::make_unique<functional_mmu::BoundaryScope>(
        observer.mmu_, fault, tc, token.event(),
        token.valid() &&
            observer.find(token)->accepted.cause == wire::Cause::PageFault,
        supported);
}
inline AcceptedInvocationScope::~AcceptedInvocationScope()
{
    if (observer_ && !sealed_) {
        seal();
    }
    if (active == this) {
        active = previous_;
    }
}
inline BoundaryToken
AcceptedInvocationScope::redirect(const void *fault, const void *tc,
                                  std::uint64_t micro_pc, std::uint16_t entry,
                                  std::uint64_t vector, std::uint64_t ip,
                                  std::uint64_t error)
{
    if (!observer_ || sealed_ || observer_->finished_ || observer_->closing_ ||
        fault != fault_ || tc != tc_) {
        return {};
    }
    auto *e = observer_->find(token_);
    if (!e) {
        return {};
    }
    auto &s = e->invocation;
    s.event = token_.event();
    s.context = token_.context();
    s.generation = token_.generation();
    s.mode = e->accepted.mode;
    s.redirect_micro_pc = micro_pc;
    s.vector_arg = vector;
    s.ip_arg = ip;
    s.entry_kind = entry;
    s.known = 7;
    const bool pf = e->accepted.cause == wire::Cause::PageFault;
    if (pf) {
        s.error_arg = error;
        s.known |= 8;
    }
    if (!e->supported || (pf ? entry != 2 : entry != 1)) {
        e->supported = false;
        e->terminal.reason_mask |= wire::reason(wire::Reason::UnsupportedMode);
        return {};
    }
    e->expected_entry = micro_pc;
    return token_;
}
inline void
AcceptedInvocationScope::seal()
{
    if (sealed_) {
        return;
    }
    sealed_ = true;
    if (!observer_ || observer_->finished_) {
        return;
    }
    auto *e = observer_->find(token_);
    if (!e) {
        return;
    }
    e->invocation_returned = true;
    if (observer_->closing_) {
        controls_.reset();
        e->terminal.reason_mask |= wire::reason(wire::Reason::PostCut);
        observer_->emitTerminal(token_.event(), true);
        observer_->maybeFinish();
        return;
    }
    auto &s = e->invocation;
    s.event = token_.event();
    s.context = token_.context();
    s.generation = token_.generation();
    s.mode = e->accepted.mode;
    s.status = e->supported && e->expected_entry ? 1 : 2;
    s.controls_post = observer_->read_();
    s.known |= 32 | 64;
    if (e->accepted.cause == wire::Cause::PageFault) {
        s.cr2_after = observer_->cr2_();
        s.known |= 16;
    }
    std::vector<fastsim::fst::mmuctl::Operation> ops;
    std::uint64_t visits = 0, first_ticket = 0, last_ticket = 0;
    if (controls_ && controls_->payload) {
        auto payload = controls_->payload;
        controls_.reset();
        if (payload->unsupported ||
            payload->writes + payload->commands != payload->ops.size()) {
            e->terminal.reason_mask |=
                wire::reason(wire::Reason::EffectOrderUnsupported);
            s.status = 2;
            observer_->mmu_.diagnostic(
                payload, fastsim::fst::mmuctl::Reason::EffectOrderUnsupported);
            payload->discarded = true;
        } else {
            ops = payload->ops;
            s.writes = static_cast<std::uint32_t>(payload->writes);
            s.commands = static_cast<std::uint32_t>(payload->commands);
            visits = payload->visits;
            first_ticket = payload->first;
            last_ticket = payload->last;
        }
    } else {
        controls_.reset();
    }
    auto logical_state = e->accepted.controls_pre;
    bool logical_match = true;
    for (const auto &op : ops) {
        if (const auto *w = std::get_if<fastsim::fst::mmuctl::Write>(&op)) {
            logical_match &= w->before_known == 15 && w->after_known == 15 &&
                             w->before == logical_state;
            logical_state = w->after;
        }
    }
    logical_match &= logical_state == s.controls_post;
    if (e->accepted.cause == wire::Cause::PageFault && e->accepted.mode == 1) {
        logical_match &= s.cr2_after == e->accepted.fault_va &&
                         s.error_arg == e->accepted.error_code;
    }
    if (!logical_match) {
        e->terminal.reason_mask |= wire::reason(wire::Reason::ControlMismatch);
        s.status = 2;
    }
    s.op_count = static_cast<std::uint32_t>(ops.size());
    s.op_hash = controls::operation_hash(ops);
    e->invocation_sealed = true;
    e->terminal.control_ops = s.op_count;
    e->terminal.control_op_hash = s.op_hash;
    auto &f = observer_->footer_;
    observer_->count(f.invocation_sealed);
    observer_->count(f.boundary_ops, s.op_count);
    observer_->count(f.boundary_writes, s.writes);
    observer_->count(f.boundary_commands, s.commands);
    if (observer_->writer_) {
        observer_->writer_->invocation_seal(s);
    }
    controls::Owner owner;
    owner.owner_kind = controls::OwnerKind::ArchitecturalBoundary;
    owner.owner_id = token_.event();
    owner.context = token_.context();
    owner.generation = token_.generation();
    owner.count = s.op_count;
    owner.op_hash = s.op_hash;
    owner.status = s.status == 1 ? controls::Sealed : controls::Unsupported;
    Observer::PendingControl pending;
    pending.first_ticket = first_ticket;
    pending.last_ticket = last_ticket;
    pending.owner = owner;
    pending.operations = std::move(ops);
    pending.predecessor = e->accepted.predecessor_ordinal;
    observer_->pending_controls_.emplace(token_.event(), std::move(pending));
    observer_->emitControls();
    auto &cf = observer_->control_footer_;
    observer_->count(cf.boundary_groups);
    observer_->count(cf.invocation_sealed_boundary);
    observer_->count(cf.unsupported_groups,
                     bool(owner.status & controls::Unsupported));
    observer_->count(cf.boundary_ops, s.op_count);
    observer_->count(cf.boundary_writes, s.writes);
    observer_->count(cf.boundary_commands, s.commands);
    observer_->count(cf.visits, visits);
}
} // namespace gem5::functional_arch
#endif
