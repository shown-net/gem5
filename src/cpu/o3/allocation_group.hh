// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_ALLOCATION_GROUP_HH__
#define __CPU_O3_ALLOCATION_GROUP_HH__

#include <memory>
#include "cpu/inst_seq.hh"

namespace gem5::o3
{
/** ISA lowering/fusion description; resource lifetimes belong to each stage. */
struct AllocationGroup
{
    const InstSeqNum leader;
    InstSeqNum end;
    InstSeqNum branchProducer, branch;
    // ISA lowering may require independent ROB records while retaining a
    // shared allocation/retirement unit. No occupancy lives in this descriptor.
    bool independentBackendEntries = false;
    AllocationGroup(InstSeqNum first, InstSeqNum last,
                    InstSeqNum producer, InstSeqNum jump)
        : leader(first), end(last), branchProducer(producer), branch(jump) {}
    bool paired() const { return leader != end; }
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
