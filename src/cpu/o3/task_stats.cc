#include "cpu/o3/task_stats.hh"
#include "cpu/o3/cpu.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "sim/system.hh"
namespace gem5 { namespace o3 {
namespace {
// Read-only physical page walk: no simulated request, TLB fill or A/D write.
template<class T> T
readGuest(gem5::ThreadContext *tc, Addr va)
{
    auto &proxy = tc->getSystemPtr()->physProxy;
    constexpr Addr mask = 0x000ffffffffff000ULL;
    Addr table = tc->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) & mask;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t entry = proxy.read<uint64_t>(table + ((va >> shift) & 511) * 8);
        fatal_if(!(entry & 1), "TASK stats: unmapped guest kernel address %#x", va);
        if (shift == 12 || ((shift == 30 || shift == 21) && (entry & 128))) {
            Addr offset = (Addr(1) << shift) - 1;
            fatal_if((va & offset) + sizeof(T) > offset + 1,
                     "TASK stats: field crosses a guest page");
            return proxy.read<T>((entry & mask & ~offset) | (va & offset));
        }
        table = entry & mask;
    }
    panic("TASK stats: invalid page walk");
}

class TaskSwitchEvent : public PCEvent
{
    TaskStatsObserver &stats;
  public:
    TaskSwitchEvent(gem5::ThreadContext *tc, Addr pc, TaskStatsObserver &s)
        : PCEvent(tc, "TASK statistics switch", pc), stats(s) {}
    void process(gem5::ThreadContext *tc) override { stats.switched(tc); }
};
}


void
TaskStatsObserver::bind(statistics::Group &group, const char *name,
                        const statistics::Counter *source)
{
    auto *info = const_cast<statistics::Info *>(group.resolveStat(name));
    info->desc += " [target TASK U+K; boundary accounted]";
    if (auto *s = dynamic_cast<statistics::ScalarInfo *>(info)) {
        scalars.push_back({s, 0, 0, source});
        auto &entry = scalars.back();
        entry.previous = entry.raw();
        s->taskValue = &entry.total;
    } else if (auto *v = dynamic_cast<statistics::VectorInfo *>(info)) {
        vectors.push_back({[v]() -> const statistics::VCounter & {
            return v->rawValue(); }, {}, {}});
        auto &entry = vectors.back();
        entry.previous = entry.raw();
        entry.total.assign(entry.previous.size(), 0);
        v->taskValues = &entry.total;
    } else {
        panic("TASK observation only binds Scalar or Vector statistics");
    }
}
void
TaskStatsObserver::settle()
{
    for (auto &entry : scalars) {
        const auto value = entry.raw();
        if (enabled) entry.total += value - entry.previous;
        entry.previous = value;
    }
    for (auto &entry : vectors) {
        const auto &values = entry.raw();
        for (size_t i = 0; i < values.size(); ++i) {
            if (enabled) entry.total[i] += values[i] - entry.previous[i];
            entry.previous[i] = values[i];
        }
    }
}
void
TaskStatsObserver::endCycle()
{
    if (enabled != nextEnabled) {
        settle();
        enabled = nextEnabled;
    }
}
void
TaskStatsObserver::observe(gem5::ThreadContext *tc, Addr switchPc, Addr currentOffset,
                      unsigned pid_offset)
{
    if (!switchPc) return; // Explicit observer-off control; scopeValid stays zero.
    fatal_if(switchEvent, "TASK stats observer already attached");
    pidOffset = pid_offset;
    Addr gs = tc->readMiscRegNoEffect(tc->getIsaPtr()->inUserMode() ?
        X86ISA::misc_reg::KernelGsBase : X86ISA::misc_reg::GsBase);
    Addr task = readGuest<Addr>(tc, gs + currentOffset);
    target = readGuest<uint32_t>(tc, task + pidOffset);
    fatal_if(!target || target >= (1U << 30), "TASK stats: invalid target TID %u", target);
    cpu.customStats.targetTid = target;
    cpu.customStats.scopeValid = 1;
    // Binding happens once after the complete statistics hierarchy exists.
    for (const char *name : {"commitStats0.numInsts",
            "commitStats0.numOps", "commitStats0.numUserInsts",
            "commitStats0.numUserOps", "commitStats0.committedInstType",
            "instsIssued", "issuedInstType", "statFuBusy"})
        bind(cpu, name);
    bind(cpu, "numCycles", &cpu.customStats.activeCycles);
    for (const char *name : {"allocatedOps", "allocationActiveCycles",
            "scheduledOps", "scheduleCyclesGe1", "scheduleCyclesGe2",
            "scheduleCyclesGe3", "scheduleCyclesGe4", "scheduleZeroCycles",
            "retireActiveCycles", "retireZeroCycles", "retiredNonLastMicroops",
            "scheduledNoNeedFU", "scheduledNoCapableFU"})
        bind(cpu.customStats, name);
    enabled = nextEnabled = true;
    switchEvent = std::make_unique<TaskSwitchEvent>(tc, switchPc, *this);
}

void
TaskStatsObserver::switched(gem5::ThreadContext *tc)
{
    Addr task = tc->getReg(X86ISA::int_reg::Rsi); // __switch_to(prev, next)
    uint32_t next = readGuest<uint32_t>(tc, task + pidOffset);
    fatal_if(next >= (1U << 30), "TASK stats: invalid next TID %u", next);
    const bool accept = next == target;
    if (accept != nextEnabled) ++cpu.customStats.targetSwitches;
    nextEnabled = accept; // One ownership decision for all events in each CPU tick.
}

} }
