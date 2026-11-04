// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_ALLOCATION_GROUP_HH__
#define __CPU_O3_ALLOCATION_GROUP_HH__

#include <cassert>
#include <array>
#include <memory>
#include "cpu/inst_seq.hh"
#include "cpu/reg_class.hh"

namespace gem5::o3
{
class IQUnit;

/** Functional pieces sharing one allocation and queue reservation. */
struct AllocationGroup
{
    const InstSeqNum leader;
    InstSeqNum end;
    InstSeqNum branchProducer, branch;
    std::array<unsigned, CCRegClass + 1> renameDests{};
    unsigned loadMembers = 0;
    // Frontend/retirement sharing need not imply shared ROB or IQ entries.
    bool independentBackendEntries = false;
    // Split stores and fused operations spanning IQ filters may share
    // retirement/ROB accounting while consuming independent scheduler entries.
    bool independentIQEntries = false;
    unsigned iqMembers = 0, robMembers = 0;
    IQUnit *iq = nullptr;
    AllocationGroup(InstSeqNum first, InstSeqNum last,
                    InstSeqNum producer, InstSeqNum jump)
        : leader(first), end(last), branchProducer(producer), branch(jump) {}
    bool paired() const { return leader != end; }
    unsigned enterIQ() { return iqMembers++ == 0; }
    unsigned leaveIQ() { assert(iqMembers); return --iqMembers == 0; }
    unsigned enterROB() { return robMembers++ == 0; }
    unsigned leaveROB() { assert(robMembers); return --robMembers == 0; }
    void truncate(InstSeqNum through)
    {
        if (leader <= through && through < end) {
            end = through;
            if (branch > through) branchProducer = branch = 0;
        }
    }
};
using AllocationGroupPtr = std::shared_ptr<AllocationGroup>;
}
#endif
