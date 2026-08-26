#ifndef __CPU_PROBES_ROI_RETIRE_HH__
#define __CPU_PROBES_ROI_RETIRE_HH__

#include "cpu/base.hh"

namespace gem5
{

inline bool
isTargetUserRetire(
    const ArchitecturalRetireRecord &record, Addr exec_start, Addr exec_end)
{
    return record.originUser && record.pc >= exec_start &&
           record.pc < exec_end;
}

} // namespace gem5

#endif // __CPU_PROBES_ROI_RETIRE_HH__
