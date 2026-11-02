/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_STATE_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_STATE_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace gem5::replacement_policy
{

/** State for QLRU_H00_M1_R0_U1, with physical-way order as the tie break.
 * The update includes invalid ways, as in the access-sequence reference.
 * Invalidating a way removes its tag without resetting its recorded age.
 */
class QLRUState
{
  private:
    std::vector<uint8_t> ages;
    std::vector<bool> valid;

    void update(unsigned index, uint8_t age)
    {
        assert(index < ages.size() && ages.size() >= 2);
        ages[index] = age;
        uint8_t maximum = 0;
        for (unsigned i = 0; i < ages.size(); ++i) {
            if (i != index)
                maximum = std::max(maximum, ages[i]);
        }
        const uint8_t increment = 3 - maximum;
        for (unsigned i = 0; i < ages.size(); ++i) {
            if (i != index)
                ages[i] += increment;
        }
    }

  public:
    explicit QLRUState(unsigned ways) : ages(ways, 3), valid(ways, false)
    {
        assert(ways >= 2);
    }

    void invalidate(unsigned index)
    {
        assert(index < ages.size());
        valid[index] = false;
    }

    void insert(unsigned index)
    {
        assert(index < ages.size());
        valid[index] = true;
        update(index, 1);
    }

    void hit(unsigned index)
    {
        assert(index < ages.size() && valid[index]);
        update(index, 0);
    }

    unsigned victim() const
    {
        for (unsigned i = 0; i < ages.size(); ++i) {
            if (!valid[i])
                return i;
        }
        for (unsigned i = 0; i < ages.size(); ++i) {
            if (ages[i] == 3)
                return i;
        }
        assert(false && "QLRU U1 must leave an age-3 way");
        std::abort();
    }

    uint8_t age(unsigned index) const { return ages.at(index); }
    bool isValid(unsigned index) const { return valid.at(index); }
};

} // namespace gem5::replacement_policy

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_QLRU_STATE_HH__
