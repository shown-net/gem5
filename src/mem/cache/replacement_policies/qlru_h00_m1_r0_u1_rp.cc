// SPDX-License-Identifier: BSD-3-Clause
#include "mem/cache/replacement_policies/qlru_h00_m1_r0_u1_rp.hh"

#include <algorithm>

#include "base/logging.hh"

namespace gem5::replacement_policy
{

QLRUH00M1R0U1::QLRUH00M1R0U1(const Params &p)
    : Base(p), numWays(p.num_ways)
{
    fatal_if(numWays < 2, "QLRU U1 requires at least two ways");
}

void
QLRUH00M1R0U1::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    const auto entry = std::static_pointer_cast<Data>(data);
    entry->state->valid[entry->way] = false;
}

void
QLRUH00M1R0U1::update(const std::shared_ptr<ReplacementData> &data,
                       uint8_t age) const
{
    const auto entry = std::static_pointer_cast<Data>(data);
    auto &ages = entry->state->ages;
    ages[entry->way] = age;
    uint8_t highest_other = 0;
    for (unsigned i = 0; i < numWays; ++i)
        if (i != entry->way) highest_other = std::max(highest_other, ages[i]);
    for (unsigned i = 0; i < numWays; ++i)
        if (i != entry->way) ages[i] += 3 - highest_other;
}

void
QLRUH00M1R0U1::touch(const std::shared_ptr<ReplacementData> &data) const
{
    update(data, 0);
}

void
QLRUH00M1R0U1::reset(const std::shared_ptr<ReplacementData> &data) const
{
    const auto entry = std::static_pointer_cast<Data>(data);
    entry->state->valid[entry->way] = true;
    update(data, 1);
}

ReplaceableEntry *
QLRUH00M1R0U1::getVictim(const ReplacementCandidates &candidates) const
{
    for (auto *candidate : candidates) {
        const auto data = std::static_pointer_cast<Data>(candidate->replacementData);
        if (!data->state->valid[data->way]) return candidate;
    }
    for (auto *candidate : candidates) {
        const auto data = std::static_pointer_cast<Data>(candidate->replacementData);
        if (data->state->ages[data->way] == 3) return candidate;
    }
    panic("QLRU set has no age-3 victim");
}

std::shared_ptr<ReplacementData>
QLRUH00M1R0U1::instantiateEntry()
{
    if (count % numWays == 0)
        currentSet = std::make_shared<SetState>(numWays);
    return std::make_shared<Data>(currentSet, count++ % numWays);
}

} // namespace gem5::replacement_policy
