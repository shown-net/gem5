// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_STAGE_DEMAND_HH__
#define __CPU_O3_STAGE_DEMAND_HH__

#include "cpu/inst_seq.hh"

namespace gem5::o3
{

/**
 * Instruction semantics at each pipeline boundary, not resource ownership.
 * A zero group/last sequence denotes the containing instruction itself.
 * Queues own their reservations, occupancy, completion and recovery state.
 */
struct StageDemand
{
    unsigned frontendUnits = 1;
    unsigned idqUnits = 1;
    unsigned allocationUnits = 1;
    unsigned robUnits = 1;
    unsigned schedulerUnits = 1;
    unsigned schedulerActions = 1;
    unsigned retirementUnits = 1;
    // Wide-capability entries are a subset of the shared vector allocation
    // budget. ISA lowering supplies this demand; functional fragment count is
    // not a substitute for architectural vector width.
    unsigned floatWideEntries = 0;
    InstSeqNum allocationGroup = 0;
    InstSeqNum allocationLast = 0;
    InstSeqNum robGroup = 0;
    InstSeqNum robLast = 0;
    InstSeqNum schedulerGroup = 0;
    InstSeqNum retirementGroup = 0;
    InstSeqNum retirementLast = 0;
};

} // namespace gem5::o3
#endif
