// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_PHYSICAL_BYTE_COVERAGE_HH__
#define __CPU_O3_PHYSICAL_BYTE_COVERAGE_HH__

#include <algorithm>
#include <cassert>
#include <vector>

#include "base/types.hh"

namespace gem5::o3
{

// LSQRequest::physicalBytes supplies sorted, unique *enabled* physical
// bytes, including all translated fragments. A missing translation is not
// represented by an empty vector: callers must wait for physicalBytes to
// succeed before using these operations. Security domains are checked by
// callers since addresses alone do not identify a memory location.
struct PhysicalByteCoverage
{
    struct Match { bool overlap = false; bool uncovered = false; };

    static bool
    intersects(const std::vector<Addr> &a, const std::vector<Addr> &b)
    {
        auto i = a.begin();
        auto j = b.begin();
        while (i != a.end() && j != b.end()) {
            if (*i < *j) ++i;
            else if (*j < *i) ++j;
            else return true;
        }
        return false;
    }

    static bool
    contains(const std::vector<Addr> &producer, const std::vector<Addr> &load)
    {
        return std::includes(producer.begin(), producer.end(),
                             load.begin(), load.end());
    }

    // Visit stores from youngest to oldest. The caller owns the coverage
    // vector so MDP learning retains the precise observation's population.
    static Match
    cover(const std::vector<Addr> &load, const std::vector<Addr> &store,
          std::vector<bool> &covered)
    {
        assert(covered.size() == load.size());
        Match result;
        size_t i = 0, j = 0;
        while (i < load.size() && j < store.size()) {
            if (load[i] < store[j]) ++i;
            else if (store[j] < load[i]) ++j;
            else {
                result.overlap = true;
                result.uncovered |= !covered[i];
                covered[i] = true;
                ++i;
                ++j;
            }
        }
        return result;
    }
};

} // namespace gem5::o3
#endif
