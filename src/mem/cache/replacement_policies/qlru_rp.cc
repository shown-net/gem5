/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/cache/replacement_policies/qlru_rp.hh"

#include "base/logging.hh"
#include "params/QLRUH00M1R0U1RP.hh"

namespace gem5::replacement_policy
{

QLRUH00M1R0U1::QLRUH00M1R0U1(const Params &p)
    : Base(p), numWays(p.num_ways)
{
    fatal_if(numWays < 2, "QLRU requires at least two ways");
}

void
QLRUH00M1R0U1::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    const auto entry = std::static_pointer_cast<Data>(data);
    entry->state->invalidate(entry->way);
}

void
QLRUH00M1R0U1::touch(const std::shared_ptr<ReplacementData> &data) const
{
    const auto entry = std::static_pointer_cast<Data>(data);
    entry->state->hit(entry->way);
}

void
QLRUH00M1R0U1::reset(const std::shared_ptr<ReplacementData> &data) const
{
    const auto entry = std::static_pointer_cast<Data>(data);
    entry->state->insert(entry->way);
}

ReplaceableEntry *
QLRUH00M1R0U1::getVictim(const ReplacementCandidates &candidates) const
{
    fatal_if(candidates.size() != numWays,
             "QLRU needs one complete set of %u candidates", numWays);
    const auto first = std::static_pointer_cast<Data>(
        candidates.front()->replacementData);
    const auto selected = first->state->victim();
    ReplaceableEntry *victim = nullptr;
    for (auto *candidate : candidates) {
        const auto entry = std::static_pointer_cast<Data>(
            candidate->replacementData);
        fatal_if(entry->state != first->state,
                 "QLRU candidates do not share a replacement set");
        if (entry->way == selected)
            victim = candidate;
    }
    fatal_if(!victim, "QLRU selected way is absent from candidates");
    return victim;
}

std::shared_ptr<ReplacementData>
QLRUH00M1R0U1::instantiateEntry()
{
    const auto way = count++ % numWays;
    if (way == 0)
        currentSet = std::make_shared<QLRUState>(numWays);
    return std::make_shared<Data>(way, currentSet);
}

} // namespace gem5::replacement_policy
