#include "cpu/o3/task_uop_stats.hh"
namespace gem5 { namespace o3 {
CustomUopStats::CustomUopStats(statistics::Group *parent)
    : statistics::Group(parent, "custom"),
      ADD_STAT(allocatedOps, statistics::units::Count::get(), "Accepted rename-to-backend operations"),
      ADD_STAT(allocationActiveCycles, statistics::units::Cycle::get(), "Cycles with accepted allocation"),
      ADD_STAT(scheduledOps, statistics::units::Count::get(), "Accepted execution dispatches including reissues and later squash"),
      ADD_STAT(scheduleCyclesGe1, statistics::units::Cycle::get(), "Cycles with at least one dispatch"),
      ADD_STAT(scheduleCyclesGe2, statistics::units::Cycle::get(), "Cycles with at least two dispatches"),
      ADD_STAT(scheduleCyclesGe3, statistics::units::Cycle::get(), "Cycles with at least three dispatches"),
      ADD_STAT(scheduleCyclesGe4, statistics::units::Cycle::get(), "Cycles with at least four dispatches"),
      ADD_STAT(scheduleZeroCycles, statistics::units::Cycle::get(), "Target unhalted cycles without dispatch"),
      ADD_STAT(retireActiveCycles, statistics::units::Cycle::get(), "Cycles with retirement"),
      ADD_STAT(retireZeroCycles, statistics::units::Cycle::get(), "Target unhalted cycles without retirement"),
      ADD_STAT(retiredNonLastMicroops, statistics::units::Count::get(), "Retired non-last microops of each macro instruction"),
      ADD_STAT(scheduledNoNeedFU, statistics::units::Count::get(), "Dispatches needing no FU"),
      ADD_STAT(scheduledNoCapableFU, statistics::units::Count::get(), "Dispatches with no capable FU"),
      ADD_STAT(targetTid, statistics::units::Count::get(), "Guest Linux TID; endpoint gauge"),
      ADD_STAT(scopeValid, statistics::units::Count::get(), "Task identity established; endpoint gauge"),
      ADD_STAT(targetSwitches, statistics::units::Count::get(), "Observed switches into or out of target")
{
}

void CustomUopStats::beginCycle()
{
    allocatedThisCycle = scheduledThisCycle = 0;
    retiredThisCycle = false;
    ++activeCycles;
    ++scheduleZeroCycles;
    ++retireZeroCycles;
}

void CustomUopStats::allocated()
{
    ++allocatedOps;
    if (++allocatedThisCycle == 1) ++allocationActiveCycles;
}

void CustomUopStats::scheduled(bool no_need_fu, bool no_capable_fu)
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

void CustomUopStats::retired(bool last)
{
    if (!last) ++retiredNonLastMicroops;
    if (!retiredThisCycle) {
        ++retireActiveCycles;
        retireZeroCycles -= 1;
        retiredThisCycle = true;
    }
}

void CustomUopStats::accountGap(uint64_t count)
{
    activeCycles += count;
    scheduleZeroCycles += count;
    retireZeroCycles += count;
}
} }
