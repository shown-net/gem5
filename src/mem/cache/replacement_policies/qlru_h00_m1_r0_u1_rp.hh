// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_H00_M1_R0_U1_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_H00_M1_R0_U1_RP_HH__

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "mem/cache/replacement_policies/base.hh"
#include "params/QLRUH00M1R0U1RP.hh"

namespace gem5::replacement_policy
{

// H00/M1/R0/U1, with consecutive replacement-data instances forming a set.
class QLRUH00M1R0U1 : public Base
{
    struct SetState
    {
        explicit SetState(unsigned ways) : ages(ways, 3), valid(ways, false) {}
        std::vector<uint8_t> ages;
        std::vector<bool> valid;
    };
    struct Data : ReplacementData
    {
        Data(std::shared_ptr<SetState> state, unsigned way)
            : state(std::move(state)), way(way) {}
        std::shared_ptr<SetState> state;
        unsigned way;
    };
    const unsigned numWays;
    unsigned count = 0;
    std::shared_ptr<SetState> currentSet;

    void update(const std::shared_ptr<ReplacementData> &data,
                uint8_t age) const;

  public:
    using Params = QLRUH00M1R0U1RPParams;
    explicit QLRUH00M1R0U1(const Params &p);
    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void touch(const std::shared_ptr<ReplacementData> &data) const override;
    void reset(const std::shared_ptr<ReplacementData> &data) const override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace gem5::replacement_policy
#endif
