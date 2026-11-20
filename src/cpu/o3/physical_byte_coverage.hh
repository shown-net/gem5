// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_PHYSICAL_BYTE_COVERAGE_HH__
#define __CPU_O3_PHYSICAL_BYTE_COVERAGE_HH__

#include <algorithm>
#include <cassert>
#include <vector>

#include "base/bitfield.hh"
#include "base/types.hh"

namespace gem5::o3
{

struct PhysicalLineCoverage
{
    Addr line;
    uint64_t bytes;

    using Lines = std::vector<PhysicalLineCoverage>;
    struct Match { bool overlap = false; bool uncovered = false; };

    bool operator==(const PhysicalLineCoverage &other) const
    { return line == other.line && bytes == other.bytes; }

    static Addr firstByte(const Lines &lines)
    {
        assert(!lines.empty() && lines.front().bytes);
        return lines.front().line + findLsbSet(lines.front().bytes);
    }

    static size_t byteCount(const Lines &lines)
    {
        size_t total = 0;
        for (const auto &entry : lines)
            total += popCount(entry.bytes);
        return total;
    }

    static bool
    contains(const Lines &producer, const Lines &consumer)
    {
        auto supplying = producer.begin();
        for (const auto &entry : consumer) {
            while (supplying != producer.end() && supplying->line < entry.line)
                ++supplying;
            if (supplying == producer.end() || supplying->line != entry.line ||
                (entry.bytes & ~supplying->bytes))
                return false;
        }
        return true;
    }

    static bool
    containsByte(const Lines &lines, Addr address)
    {
        const Addr line = address & ~Addr(63);
        auto it = std::lower_bound(lines.begin(), lines.end(), line,
            [](const auto &entry, Addr key) { return entry.line < key; });
        return it != lines.end() && it->line == line &&
            (it->bytes & (uint64_t(1) << (address & 63)));
    }

    static bool
    intersects(const Lines &a, const Lines &b)
    {
        auto i = a.begin();
        auto j = b.begin();
        while (i != a.end() && j != b.end()) {
            if (i->line < j->line) {
                ++i;
            } else if (j->line < i->line) {
                ++j;
            } else {
                if (i->bytes & j->bytes)
                    return true;
                ++i;
                ++j;
            }
        }
        return false;
    }
    // Visit stores from youngest to oldest. The caller owns the coverage
    // masks indexed by the load's sorted line positions.
    static Match
    cover(const Lines &load, const Lines &store, std::vector<uint64_t> &covered)
    {
        assert(covered.size() == load.size());
        Match result;
        size_t i = 0, j = 0;
        while (i < load.size() && j < store.size()) {
            if (load[i].line < store[j].line) {
                ++i;
            } else if (store[j].line < load[i].line) {
                ++j;
            } else {
                const uint64_t overlap = load[i].bytes & store[j].bytes;
                result.overlap |= overlap != 0;
                result.uncovered |= (overlap & ~covered[i]) != 0;
                covered[i] |= overlap;
                ++i;
                ++j;
            }
        }
        return result;
    }
};

} // namespace gem5::o3
#endif
