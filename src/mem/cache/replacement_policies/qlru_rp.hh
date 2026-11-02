/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_RP_HH__

#include <cstdint>
#include <memory>
#include <utility>

#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/qlru_state.hh"

namespace gem5
{

struct QLRUH00M1R0U1RPParams;

namespace replacement_policy
{

/** One candidate access-sequence policy, not a physical SPR claim. */
class QLRUH00M1R0U1 : public Base
{
  private:
    struct Data : ReplacementData
    {
        const unsigned way;
        const std::shared_ptr<QLRUState> state;
        Data(unsigned way, std::shared_ptr<QLRUState> state)
            : way(way), state(std::move(state)) {}
    };

    const unsigned numWays;
    uint64_t count = 0;
    std::shared_ptr<QLRUState> currentSet;

  public:
    using Params = QLRUH00M1R0U1RPParams;
    explicit QLRUH00M1R0U1(const Params &p);

    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void touch(const std::shared_ptr<ReplacementData> &data) const override;
    void reset(const std::shared_ptr<ReplacementData> &data) const override;
    ReplaceableEntry *getVictim(
        const ReplacementCandidates &candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_RP_HH__
