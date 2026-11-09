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
        bool installStarted = false;
        uint64_t installCompleteCycle = 0;
        bool softwarePrefetch = false;
        bool writeInitiated = false;
    };

    unsigned capacity = 0;
    unsigned writeCapacity = 0;
    unsigned writes = 0;
    unsigned demandReads = 0;
    std::unordered_map<Addr, Entry> entries;

  public:
    void setCapacity(unsigned value) { capacity = value; }
    void setWriteCapacity(unsigned value) { writeCapacity = value; }
    bool canAllocate(bool write = false) const
    {
        return (capacity == 0 || entries.size() < capacity) &&
            (!write || writeCapacity == 0 || writes < writeCapacity);
    }
    bool contains(Addr line) const { return entries.count(line) != 0; }
    void markSoftwarePrefetch(Addr line) { entries.at(line).softwarePrefetch = true; }
    bool fromSoftwarePrefetch(Addr line) const
    {
        const auto it = entries.find(line);
        return it != entries.end() && it->second.softwarePrefetch;
    }
    size_t size() const { return entries.size(); }
    unsigned demandReadCount() const { return demandReads; }
    unsigned readyCount() const
    {
        unsigned count = 0;
        for (const auto &[line, entry] : entries) count += entry.dataReady;
        return count;
    }
    bool ready(Addr line) const
    {
        auto it = entries.find(line);
        return it != entries.end() && it->second.dataReady;
    }
    bool installStarted(Addr line) const
    {
        return entries.at(line).installStarted;
    }
    bool installComplete(Addr line, uint64_t cycle) const
    {
        const auto &entry = entries.at(line);
        return entry.installStarted && cycle >= entry.installCompleteCycle;
    }
    void startInstall(Addr line, uint64_t complete_cycle)
    {
        auto &entry = entries.at(line);
        assert(entry.dataReady && !entry.installStarted);
        entry.installStarted = true;
        entry.installCompleteCycle = complete_cycle;
    }

    void allocate(Addr line, bool demand_read, bool write_initiated)
    {
        assert(canAllocate(write_initiated));
        [[maybe_unused]] const bool inserted =
            entries.emplace(line, Entry{false, demand_read, false, 0, false,
                                        write_initiated}).second;
        assert(inserted);
        if (demand_read) ++demandReads;
        if (write_initiated) ++writes;
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
        if (it->second.writeInitiated) {
            assert(writes);
            --writes;
        }
        entries.erase(it);
    }
};

} // namespace gem5::ruby
#endif
