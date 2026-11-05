// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_RUBY_STRUCTURES_DATA_FILL_BUFFER_HH__
#define __MEM_RUBY_STRUCTURES_DATA_FILL_BUFFER_HH__

#include <cassert>
#include <cstddef>
#include <unordered_map>

#include "base/types.hh"

namespace gem5::ruby
{

// Per-L1 data-fill credits. Ruby TBEs own coherence transactions and the
// Sequencer owns CPU packets; neither is stored in this resource.
class DataFillBuffer
{
  private:
    struct Entry
    {
        bool dataReady = false;
        bool demandRead = false;
    };

    unsigned capacity = 0;
    unsigned demandReads = 0;
    std::unordered_map<Addr, Entry> entries;

  public:
    void setCapacity(unsigned value) { capacity = value; }
    bool canAllocate() const { return capacity == 0 || entries.size() < capacity; }
    bool contains(Addr line) const { return entries.count(line) != 0; }
    size_t size() const { return entries.size(); }
    unsigned demandReadCount() const { return demandReads; }

    void allocate(Addr line, bool demand_read)
    {
        assert(canAllocate());
        [[maybe_unused]] const bool inserted =
            entries.emplace(line, Entry{false, demand_read}).second;
        assert(inserted);
        if (demand_read) ++demandReads;
    }

    void markReady(Addr line)
    {
        auto it = entries.find(line);
        if (it != entries.end()) it->second.dataReady = true;
    }

    bool pending(Addr line) const
    {
        auto it = entries.find(line);
        return it != entries.end() && !it->second.dataReady;
    }

    bool needsDemandRead(Addr line) const
    {
        auto it = entries.find(line);
        return it != entries.end() && !it->second.demandRead;
    }

    void observeDemandRead(Addr line)
    {
        auto it = entries.find(line);
        assert(it != entries.end() && !it->second.demandRead);
        it->second.demandRead = true;
        ++demandReads;
    }

    void release(Addr line)
    {
        auto it = entries.find(line);
        assert(it != entries.end());
        if (it->second.demandRead) {
            assert(demandReads);
            --demandReads;
        }
        entries.erase(it);
    }
};

} // namespace gem5::ruby
#endif
