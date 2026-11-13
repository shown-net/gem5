// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_RUBY_STRUCTURES_L2_CACHE_SERVICE_HH__
#define __MEM_RUBY_STRUCTURES_L2_CACHE_SERVICE_HH__

#include <cassert>

namespace gem5
{
namespace ruby
{

/**
 * Resources belonging to one private L2 controller.
 *
 * A fill credit is owned by a distinct line transaction and lives until the
 * protocol releases it.  It is neither an L1 fill buffer nor a Ruby TBE.
 * The latter remains authoritative for the coherence state of a line.
 */
class L2CacheService
{
  private:
    unsigned fillCapacity = 0;
    unsigned fillOccupancy = 0;

  public:
    void setFillCapacity(unsigned capacity) { fillCapacity = capacity; }
    bool canAllocateFill() const
    { return fillCapacity == 0 || fillOccupancy < fillCapacity; }
    unsigned allocateFill()
    {
        assert(canAllocateFill());
        return ++fillOccupancy;
    }
    unsigned releaseFill()
    {
        assert(fillOccupancy > 0);
        return --fillOccupancy;
    }
    unsigned outstandingFills() const { return fillOccupancy; }

};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_L2_CACHE_SERVICE_HH__
