#include "cpu/o3/task_uop_stats.hh"

#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "sim/system.hh"

namespace gem5
{
namespace o3
{
namespace
{
// Read-only physical page walk: no simulated request, TLB fill or A/D write.
template<class T> T
readGuest(ThreadContext *tc, Addr va)
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
    TaskUopStats &stats;
  public:
    TaskSwitchEvent(ThreadContext *tc, Addr pc, TaskUopStats &s)
        : PCEvent(tc, "uop TASK switch", pc), stats(s) {}
    void process(ThreadContext *tc) override { stats.switched(tc); }
};
}

TaskUopStats::TaskUopStats(statistics::Group *parent, const char *name)
    : statistics::Group(parent, name),
      ADD_STAT(allocatedOps, statistics::units::Count::get(), "Accepted rename-to-backend operations"),
      ADD_STAT(allocationActiveCycles, statistics::units::Cycle::get(), "Cycles with accepted allocation"),
      ADD_STAT(scheduledOps, statistics::units::Count::get(), "Accepted execution dispatches including reissues and later squash"),
      ADD_STAT(scheduleCyclesGe1, statistics::units::Cycle::get(), "Cycles with at least one dispatch"),
      ADD_STAT(scheduleCyclesGe2, statistics::units::Cycle::get(), "Cycles with at least two dispatches"),
      ADD_STAT(scheduleCyclesGe3, statistics::units::Cycle::get(), "Cycles with at least three dispatches"),
      ADD_STAT(scheduleCyclesGe4, statistics::units::Cycle::get(), "Cycles with at least four dispatches"),
      ADD_STAT(scheduleZeroCycles, statistics::units::Cycle::get(), "Target unhalted cycles without dispatch"),
      ADD_STAT(retiredSlots, statistics::units::Count::get(), "Consumed model retirement slots"),
      ADD_STAT(retireActiveCycles, statistics::units::Cycle::get(), "Cycles with retirement"),
      ADD_STAT(retireZeroCycles, statistics::units::Cycle::get(), "Target unhalted cycles without retirement"),
      ADD_STAT(retiredNonLastMicroops, statistics::units::Count::get(), "Retired non-last microops of each macro instruction"),
      ADD_STAT(instructions, statistics::units::Count::get(), "Retired architectural instructions"),
      ADD_STAT(cycles, statistics::units::Cycle::get(), "Target TASK U+K active CPU cycles"),
      ADD_STAT(scheduledNoNeedFU, statistics::units::Count::get(), "Dispatches needing no FU"),
      ADD_STAT(scheduledNoCapableFU, statistics::units::Count::get(), "Dispatches with no capable FU"),
      ADD_STAT(targetTid, statistics::units::Count::get(), "Guest Linux TID; endpoint gauge"),
      ADD_STAT(scopeValid, statistics::units::Count::get(), "Task identity established; endpoint gauge"),
      ADD_STAT(targetSwitches, statistics::units::Count::get(), "Observed switches into or out of target")
{
    for (auto *counter : {&allocatedOps, &allocationActiveCycles, &scheduledOps,
            &scheduleCyclesGe1, &scheduleCyclesGe2, &scheduleCyclesGe3,
            &scheduleCyclesGe4, &scheduleZeroCycles, &retiredSlots,
            &retireActiveCycles, &retireZeroCycles, &retiredNonLastMicroops,
            &instructions, &cycles, &scheduledNoNeedFU, &scheduledNoCapableFU})
        counter->bind(enabled);
}

void
TaskUopStats::observe(ThreadContext *tc, Addr switchPc, Addr currentOffset,
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
    targetTid = target;
    scopeValid = 1;
    enabled = nextEnabled = true;
    switchEvent = std::make_unique<TaskSwitchEvent>(tc, switchPc, *this);
}

void
TaskUopStats::switched(ThreadContext *tc)
{
    Addr task = tc->getReg(X86ISA::int_reg::Rsi); // __switch_to(prev, next)
    uint32_t next = readGuest<uint32_t>(tc, task + pidOffset);
    fatal_if(next >= (1U << 30), "TASK stats: invalid next TID %u", next);
    const bool accept = next == target;
    if (accept != nextEnabled) ++targetSwitches;
    nextEnabled = accept; // One ownership decision for all events in each CPU tick.
}

void TaskUopStats::beginCycle()
{
    allocatedThisCycle = scheduledThisCycle = 0;
    retiredThisCycle = false;
    ++cycles;
    ++scheduleZeroCycles;
    ++retireZeroCycles;
}

void TaskUopStats::allocated()
{
    ++allocatedOps;
    if (++allocatedThisCycle == 1) ++allocationActiveCycles;
}

void TaskUopStats::scheduled(bool no_need_fu, bool no_capable_fu)
{
    ++scheduledOps;
    switch (++scheduledThisCycle) {
      case 1: ++scheduleCyclesGe1; scheduleZeroCycles -= 1; break;
      case 2: ++scheduleCyclesGe2; break;
      case 3: ++scheduleCyclesGe3; break;
      case 4: ++scheduleCyclesGe4; break;
    }
    if (no_need_fu) ++scheduledNoNeedFU;
    if (no_capable_fu) ++scheduledNoCapableFU;
}

void TaskUopStats::retired(bool last)
{
    ++retiredSlots;
    if (last) ++instructions;
    else ++retiredNonLastMicroops;
    if (!retiredThisCycle) {
        ++retireActiveCycles;
        retireZeroCycles -= 1;
        retiredThisCycle = true;
    }
}

void TaskUopStats::accountGap(uint64_t count)
{
    cycles += count;
    scheduleZeroCycles += count;
    retireZeroCycles += count;
}
} // namespace o3
} // namespace gem5
