#ifndef __CPU_O3_TASK_UOP_STATS_HH__
#define __CPU_O3_TASK_UOP_STATS_HH__

#include <memory>
#include "base/statistics.hh"
#include "cpu/pc_event.hh"

namespace gem5
{
class ThreadContext;
namespace o3
{

// A shared acceptance gate, not a guest TID test at every observation site.
class TaskCounter : public statistics::Scalar
{
    const bool *gate = nullptr;
  public:
    using statistics::Scalar::Scalar;
    void bind(const bool &enabled) { gate = &enabled; }
    void operator++() { if (gate && *gate) statistics::Scalar::operator++(); }
    void operator+=(statistics::Counter n)
    { if (gate && *gate) statistics::Scalar::operator+=(n); }
    void operator-=(statistics::Counter n)
    { if (gate && *gate) statistics::Scalar::operator-=(n); }
};

struct TaskUopStats : public statistics::Group
{
    bool enabled = false;
    bool nextEnabled = false;
    unsigned allocatedThisCycle = 0, scheduledThisCycle = 0;
    bool retiredThisCycle = false;
    uint32_t target = 0;
    unsigned pidOffset = 0;
    std::unique_ptr<PCEvent> switchEvent;

    TaskCounter allocatedOps, allocationActiveCycles;
    TaskCounter scheduledOps, scheduleCyclesGe1, scheduleCyclesGe2;
    TaskCounter scheduleCyclesGe3, scheduleCyclesGe4, scheduleZeroCycles;
    TaskCounter retiredSlots, retireActiveCycles, retireZeroCycles;
    TaskCounter retiredNonLastMicroops, instructions, cycles;
    TaskCounter scheduledNoNeedFU, scheduledNoCapableFU;
    statistics::Scalar targetTid, scopeValid, targetSwitches;

    TaskUopStats(statistics::Group *parent, const char *name);
    void observe(ThreadContext *tc, Addr switchPc, Addr currentOffset,
                 unsigned pid_offset);
    void switched(ThreadContext *tc);
    void beginCycle();
    void endCycle() { enabled = nextEnabled; }
    void allocated();
    void scheduled(bool no_need_fu, bool no_capable_fu);
    void retired(bool last);
    void accountGap(uint64_t count);
};

} // namespace o3
} // namespace gem5
#endif
