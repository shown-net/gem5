#ifndef __CPU_O3_TASK_UOP_STATS_HH__
#define __CPU_O3_TASK_UOP_STATS_HH__
#include "base/statistics.hh"
namespace gem5 { namespace o3 {
// Model events; TASK scope is applied by the common boundary observer.
struct CustomUopStats : public statistics::Group
{
    unsigned allocatedThisCycle = 0, scheduledThisCycle = 0;
    bool retiredThisCycle = false;
    statistics::Counter activeCycles = 0;
    statistics::Scalar allocatedOps, allocationActiveCycles;
    statistics::Scalar scheduledOps, scheduleCyclesGe1, scheduleCyclesGe2;
    statistics::Scalar scheduleCyclesGe3, scheduleCyclesGe4, scheduleZeroCycles;
    statistics::Scalar retireActiveCycles, retireZeroCycles;
    statistics::Scalar retiredNonLastMicroops;
    statistics::Scalar scheduledNoNeedFU, scheduledNoCapableFU;
    statistics::Scalar targetTid, scopeValid, targetSwitches;
    explicit CustomUopStats(statistics::Group *parent);
    void beginCycle();
    void allocated();
    void scheduled(bool no_need_fu, bool no_capable_fu);
    void retired(bool last);
    void accountGap(uint64_t count);
};
} }
#endif
