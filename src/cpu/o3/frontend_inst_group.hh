// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_FRONTEND_INST_GROUP_HH__
#define __CPU_O3_FRONTEND_INST_GROUP_HH__

#include <array>
#include <deque>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

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

    explicit FrontendInstGroup(const std::deque<DynInstPtr> &queue);
    FrontendInstGroup(const DynInstPtr *insts, unsigned available);

  private:
    void validate() const;
};

} // namespace o3
} // namespace gem5

#endif
