// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_FRONTEND_INST_GROUP_HH__
#define __CPU_O3_FRONTEND_INST_GROUP_HH__

#include <array>
#include <deque>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{
namespace o3
{

/** A stage admission/transfer unit containing the actual execution objects.
 * Queues and time buffers retain all members in program order. A stage peeks
 * the complete group, reserves resources, then processes every member before
 * considering the next group. No member has a timing weight or a second life.
 */
struct FrontendInstGroup
{
    std::array<DynInstPtr, 2> members;
    unsigned size;

    explicit FrontendInstGroup(const std::deque<DynInstPtr> &queue)
        : size(queue.front()->frontendGroupSize)
    {
        panic_if(size == 0 || size > members.size() || queue.size() < size,
                 "Incomplete frontend group at stage admission");
        for (unsigned i = 0; i < size; ++i) {
            members[i] = queue[i];
        }
        validate();
    }

    FrontendInstGroup(const DynInstPtr *insts, unsigned available)
        : size(insts[0]->frontendGroupSize)
    {
        panic_if(size == 0 || size > members.size() || available < size,
                 "Incomplete frontend group in stage transport");
        for (unsigned i = 0; i < size; ++i)
            members[i] = insts[i];
        validate();
    }

  private:
    void validate() const
    {
        for (unsigned i = 0; i < size; ++i) {
            panic_if(!members[i] || members[i]->frontendGroupSize != size ||
                     members[i]->frontendGroupKind != members[0]->frontendGroupKind ||
                     members[i]->frontendGroupIndex != i ||
                     members[i]->frontendGroupHead !=
                         members[0]->frontendGroupHead ||
                     members[i]->threadNumber != members[0]->threadNumber,
                     "Frontend group members disagree at stage admission");
        }
        panic_if(size == 2 &&
                 (members[0]->macroop != members[1]->macroop ||
                  members[0]->pcState().instAddr() !=
                      members[1]->pcState().instAddr()),
                 "Frontend group contains members of different instructions");
        const auto kind = members[0]->frontendGroupKind;
        panic_if((size == 1 && kind != DynInst::FrontendGroupKind::Single) ||
                 (size == 2 && kind == DynInst::FrontendGroupKind::Single),
                 "Frontend group kind and membership disagree");
        panic_if(kind == DynInst::FrontendGroupKind::Store &&
                 (!members[0]->isStoreAddress() || !members[1]->isStoreData()),
                 "Frontend store group lacks its real STA/STD pair");
        panic_if(kind == DynInst::FrontendGroupKind::LoadAlu &&
                 (!members[0]->isLoad() || members[1]->isMemRef() ||
                  members[1]->isControl()),
                 "Frontend load-ALU group lacks its real execution members");
    }
};

} // namespace o3
} // namespace gem5

#endif
