// SPDX-License-Identifier: BSD-3-Clause
#ifndef __ARCH_X86_PAGING_STRUCTURE_CACHE_HH__
#define __ARCH_X86_PAGING_STRUCTURE_CACHE_HH__

#include <array>
#include <list>

#include "arch/x86/pagetable.hh"
#include "base/statistics.hh"
#include "params/X86PagingStructureCache.hh"
#include "sim/sim_object.hh"

namespace gem5::X86ISA
{

// Upper virtual-address prefixes, not leaf translations or physical PTE data.
// Capacity and LRU are explicit engineering priors; both walkers share this
// object on the SPR core. Ordinary walkers retain uncached ISA support.
class PagingStructureCache : public SimObject
{
  public:
    struct Prefix {
        unsigned level;
        uint64_t cr3, cr4, efer;
        uint16_t pcid;
        Addr tag, pteAddress;
        uint64_t pte;
        fastsim::fst::TranslationMapping provenance;
    };

  private:
    const unsigned entriesPerLevel;
    std::array<std::list<Prefix>, 3> entries;
    struct Stats : statistics::Group {
        statistics::Scalar lookups, misses, fills, invalidated;
        statistics::Vector hits;
        explicit Stats(statistics::Group *parent);
    } stats;

  public:
    explicit PagingStructureCache(const X86PagingStructureCacheParams &p);
    static unsigned shift(unsigned level) { return 39 - 9 * level; }
    bool lookup(uint64_t cr3, uint64_t cr4, uint64_t efer, uint16_t pcid,
                Addr address, Prefix &result);
    void insert(Prefix prefix);
    void invalidate(int pcid = -1);
};

} // namespace gem5::X86ISA
#endif
