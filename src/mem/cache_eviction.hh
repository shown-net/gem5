// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_CACHE_EVICTION_HH__
#define __MEM_CACHE_EVICTION_HH__

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/extensible.hh"
#include "mem/request.hh"

namespace gem5
{

/** A private capacity replacement does not change coherent memory values. */
class CacheEvictionOrigin : public Extension<Request, CacheEvictionOrigin>
{
  public:
    bool localCapacity = false;

    std::unique_ptr<ExtensionBase> clone() const override
    {
        return std::make_unique<CacheEvictionOrigin>(*this);
    }
};

/** A live read whose cache copy has been replaced still observes writers. */
class CapacityReadObserver
{
  public:
    virtual ~CapacityReadObserver() = default;
    virtual void notifyCapacityReadWrite(Addr line,
                                        const RequestPtr &writer) = 0;
};

/** Only lost, unretired reads subscribe; a write visits its own line. */
class CapacityReadObservations
{
  private:
    std::unordered_map<Addr, std::unordered_set<CapacityReadObserver *>> lines;

  public:
    void subscribe(Addr line, CapacityReadObserver *observer)
    {
        lines[line].insert(observer);
    }

    void unsubscribe(Addr line, CapacityReadObserver *observer)
    {
        auto found = lines.find(line);
        if (found == lines.end())
            return;
        found->second.erase(observer);
        if (found->second.empty())
            lines.erase(found);
    }

    void notifyWrite(Addr line, const RequestPtr &writer)
    {
        auto found = lines.find(line);
        if (found == lines.end())
            return;
        // A recipient can release a subscription during notification.
        const std::vector<CapacityReadObserver *> recipients(
            found->second.begin(), found->second.end());
        for (auto *observer : recipients)
            observer->notifyCapacityReadWrite(line, writer);
    }
};

} // namespace gem5

#endif
