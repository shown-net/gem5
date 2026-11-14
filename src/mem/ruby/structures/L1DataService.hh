// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_RUBY_STRUCTURES_L1_DATA_SERVICE_HH__
#define __MEM_RUBY_STRUCTURES_L1_DATA_SERVICE_HH__

#include <cassert>
#include <cstddef>
#include <deque>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "mem/l1_data_access.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/system/SequencerRequest.hh"

namespace gem5::ruby
{

// Per-L1 data service. This resource owns fill credits and the distinct
// installation, load-return budgets and snapshots awaiting delivery. Ruby
// TBEs own coherence state and the Sequencer retains CPU packet identity.
class L1DataService
{
  public:
    struct PendingResponse
    {
        SequencerRequest request;
        DataBlock data;
    };
    struct FillParticipant
    {
        PacketPtr packet;
        bool follower;
        bool hasServiceSource;
    };

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
    unsigned readyEntries = 0;
    std::unordered_map<Addr, Entry> entries;
    uint64_t nextInstallCycle = 0;
    const unsigned installBytesPerCycle;
    unsigned returnWidth = 0;
    unsigned returnsUsed = 0;
    uint64_t returnCycle = uint64_t(-1);
    std::deque<PendingResponse> pendingResponses;
    struct FillRegistration
    {
        bool follower;
        bool hasServiceSource;
    };
    struct OrderedLineRequest
    {
        PacketPtr packet;
        bool shareable;
    };
    // Packet ownership stays with the Sequencer. The L1 service retains its
    // own per-line order and barriers, including operations that do not join
    // an FB, so selection cannot cross an older store or special request.
    std::unordered_map<Addr, std::list<OrderedLineRequest>> lineRequests;
    std::unordered_map<PacketPtr, FillRegistration> fillParticipants;
    bool publishedStoreLineValid = false;
    uint64_t publishedStoreCycle = 0;
    Addr publishedStoreLine = 0;
    L1DataAccessBudget requestAccess;

  public:
    enum class InstallResult { Blocked, InProgress, Complete };

    explicit L1DataService(unsigned install_bytes_per_cycle)
        : installBytesPerCycle(install_bytes_per_cycle)
    {}

    void setCapacity(unsigned value) { capacity = value; }
    void setWriteCapacity(unsigned value) { writeCapacity = value; }
    L1DataAccessResource *dataAccess() { return &requestAccess; }
    void admitLineRequest(Addr line, PacketPtr packet, bool shareable)
    {
        assert(packet);
        lineRequests[line].push_back({packet, shareable});
    }
    bool mayCompleteHit(Addr line, PacketPtr packet) const
    {
        const auto found = lineRequests.find(line);
        if (found == lineRequests.end())
            return false;
        for (const auto &entry : found->second) {
            if (entry.packet == packet)
                return true;
            if (!entry.shareable)
                return false;
        }
        return false;
    }
    void finishLineRequest(Addr line, PacketPtr packet)
    {
        const auto found = lineRequests.find(line);
        if (found == lineRequests.end())
            return;
        auto &ordered = found->second;
        for (auto it = ordered.begin(); it != ordered.end(); ++it) {
            if (it->packet == packet) {
                ordered.erase(it);
                if (ordered.empty()) lineRequests.erase(found);
                return;
            }
        }
    }
    bool registerFillRead(PacketPtr packet, bool follower,
                          bool has_service_source)
    {
        assert(packet);
        auto [it, inserted] = fillParticipants.emplace(
            packet, FillRegistration{follower, has_service_source});
        if (!inserted)
            it->second = {follower, has_service_source};
        return inserted;
    }
    bool hasFillRead(PacketPtr packet) const
    { return packet && fillParticipants.count(packet) != 0; }
    bool cancelFillRead(PacketPtr packet)
    {
        return packet && fillParticipants.erase(packet) != 0;
    }
    std::vector<FillParticipant> selectFillReads(Addr line)
    {
        std::vector<FillParticipant> selected;
        const auto found = lineRequests.find(line);
        if (found == lineRequests.end())
            return selected;
        auto &ordered = found->second;
        bool retained_older = false;
        for (auto entry = ordered.begin(); entry != ordered.end();) {
            const bool can_share = entry->shareable;
            // A non-shareable request may be supplied only when it will be
            // at the head after the preceding selected reads are removed.
            if (!can_share && retained_older)
                break;
            const auto it = fillParticipants.find(entry->packet);
            if (it == fillParticipants.end()) {
                if (!can_share)
                    break;
                retained_older = true;
                ++entry;
                continue;
            }
            selected.push_back({entry->packet, it->second.follower,
                                it->second.hasServiceSource});
            fillParticipants.erase(it);
            entry = ordered.erase(entry);
            if (!can_share)
                break;
        }
        if (ordered.empty()) lineRequests.erase(found);
        return selected;
    }
    void setReturnWidth(unsigned value) { returnWidth = value; }
    bool hasReturnLimit() const { return returnWidth != 0; }
    size_t pendingResponseCount() const { return pendingResponses.size(); }
    bool hasPendingResponses() const { return !pendingResponses.empty(); }
    bool hasOverdueResponse(Cycles now, Cycles threshold) const
    {
        for (const auto &response : pendingResponses)
            if (now - response.request.issue_time >= threshold)
                return true;
        return false;
    }
    void enqueueResponse(const SequencerRequest &request, const DataBlock &data)
    { pendingResponses.push_back({request, data}); }
    std::optional<PendingResponse> dequeueResponse(uint64_t cycle)
    {
        if (pendingResponses.empty() || !tryReturn(cycle))
            return std::nullopt;
        PendingResponse response = std::move(pendingResponses.front());
        pendingResponses.pop_front();
        return response;
    }
    int functionalWriteResponses(Packet *pkt)
    {
        int written = 0;
        for (auto &response : pendingResponses) {
            const int bytes = response.data.getBlockSize();
            assert(bytes > 0 && (bytes & (bytes - 1)) == 0);
            const Addr line = response.request.pkt->getAddr() & ~(Addr(bytes) - 1);
            const bool overlaps = pkt->isSecure() == response.request.pkt->isSecure() &&
                pkt->getAddr() < line + bytes &&
                line < pkt->getAddr() + pkt->getSize();
            pkt->trySatisfyFunctional(nullptr, line,
                response.request.pkt->isSecure(), bytes,
                response.data.getDataMod(0));
            written += overlaps;
        }
        return written;
    }
    bool tryReturn(uint64_t cycle)
    {
        if (!returnWidth) return true;
        if (returnCycle != cycle) {
            returnCycle = cycle;
            returnsUsed = 0;
        }
        if (returnsUsed >= returnWidth) return false;
        ++returnsUsed;
        return true;
    }
    bool canPublishStore(Addr line, uint64_t cycle) const
    {
        return !publishedStoreLineValid || publishedStoreCycle != cycle ||
            publishedStoreLine == line;
    }
    void notePublishedStore(Addr line, uint64_t cycle)
    {
        assert(canPublishStore(line, cycle));
        publishedStoreLineValid = true;
        publishedStoreCycle = cycle;
        publishedStoreLine = line;
    }
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
    unsigned readyCount() const { return readyEntries; }
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

    InstallResult advanceInstall(Addr line, uint64_t cycle, unsigned bytes)
    {
        // Permission-only protocol transactions do not use a fill credit.
        if (!contains(line)) return InstallResult::Complete;
        if (!installStarted(line)) {
            if (cycle < nextInstallCycle) return InstallResult::Blocked;
            const unsigned beats = (bytes + installBytesPerCycle - 1) /
                                   installBytesPerCycle;
            assert(beats);
            startInstall(line, cycle + beats - 1);
            nextInstallCycle = cycle + beats;
        }
        return installComplete(line, cycle) ? InstallResult::Complete :
                                             InstallResult::InProgress;
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
        if (it != entries.end() && !it->second.dataReady) {
            it->second.dataReady = true;
            ++readyEntries;
        }
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
        if (it->second.dataReady) {
            assert(readyEntries);
            --readyEntries;
        }
        entries.erase(it);
    }
};

} // namespace gem5::ruby
#endif
