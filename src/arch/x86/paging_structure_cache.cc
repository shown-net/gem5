// SPDX-License-Identifier: BSD-3-Clause
#include "arch/x86/paging_structure_cache.hh"

#include <algorithm>

namespace gem5::X86ISA
{

PagingStructureCache::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(), "Upper-prefix cache queries"),
      ADD_STAT(misses, statistics::units::Count::get(), "Queries requiring the root walk"),
      ADD_STAT(fills, statistics::units::Count::get(), "Validated upper-prefix fills"),
      ADD_STAT(invalidated, statistics::units::Count::get(), "Prefix entries removed by invalidation"),
      ADD_STAT(hits, statistics::units::Count::get(), "Hits by cached upper paging level")
{
    hits.init(3);
    hits.subname(0, "pml4");
    hits.subname(1, "pdp");
    hits.subname(2, "pd");
}

PagingStructureCache::PagingStructureCache(const X86PagingStructureCacheParams &p)
    : SimObject(p), entriesPerLevel(p.entriesPerLevel), stats(this)
{}

bool
PagingStructureCache::lookup(uint64_t cr3, uint64_t cr4, uint64_t efer,
                            uint16_t pcid, Addr address, Prefix &result)
{
    ++stats.lookups;
    for (int level = 2; level >= 0; --level) {
        auto &pool = entries[level];
        const Addr tag = (address & mask(48)) >> shift(level);
        auto found = std::find_if(pool.begin(), pool.end(), [&](const Prefix &p) {
            return p.cr3 == cr3 && p.cr4 == cr4 && p.efer == efer &&
                   p.pcid == pcid && p.tag == tag;
        });
        if (found != pool.end()) {
            result = *found;
            pool.splice(pool.begin(), pool, found);
            ++stats.hits[level];
            return true;
        }
    }
    ++stats.misses;
    return false;
}

void
PagingStructureCache::insert(Prefix prefix)
{
    if (!entriesPerLevel)
        return;
    auto &pool = entries.at(prefix.level);
    auto found = std::find_if(pool.begin(), pool.end(), [&](const Prefix &p) {
        return p.cr3 == prefix.cr3 && p.cr4 == prefix.cr4 &&
               p.efer == prefix.efer && p.pcid == prefix.pcid && p.tag == prefix.tag;
    });
    if (found != pool.end())
        pool.erase(found);
    else if (pool.size() == entriesPerLevel)
        pool.pop_back();
    pool.push_front(std::move(prefix));
    ++stats.fills;
}

void
PagingStructureCache::invalidate(int pcid)
{
    for (auto &pool : entries)
        for (auto it = pool.begin(); it != pool.end();) {
            if (pcid < 0 || it->pcid == pcid) {
                it = pool.erase(it);
                ++stats.invalidated;
            } else {
                ++it;
            }
        }
}

} // namespace gem5::X86ISA
