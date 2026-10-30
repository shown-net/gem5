// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_FRONTEND_HH__
#define __CPU_O3_FRONTEND_HH__

#include <deque>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <tuple>
#include <vector>
#include "base/statistics.hh"
#include "cpu/o3/frontend_record.hh"
#include "params/DsbTemplates.hh"
#include "params/FrontendCoordinator.hh"
#include "params/InstructionBytes.hh"
#include "params/InstructionDeliveryQueue.hh"
#include "params/MiteService.hh"
#include "sim/sim_object.hh"

namespace gem5::o3
{
class InstructionBytes : public SimObject
{
    const unsigned budget;
    std::vector<uint8_t> data;
    bool valid = false;
    bool reusable = false;
    Addr virtualBase = 0, physicalBase = 0;
    uint64_t readyCycle = 0;
    uint64_t serviceCycle = ~uint64_t(0);
    Addr serviceBlock = 0;
    struct Stats : statistics::Group
    {
        statistics::Scalar returnedBytes, serviceBlocks, invalidations;
        statistics::Scalar translationWaitCycles, responseWaitCycles, retryWaitCycles;
        Stats(statistics::Group *parent);
    } stats;
  public:
    InstructionBytes(const InstructionBytesParams &p);
    void waiting(bool translation, bool response, bool retry);
    void complete(Addr virtual_address, Addr physical_address,
                  const uint8_t *bytes, unsigned count,
                  uint64_t cycle, bool can_reuse);
    bool serve(Addr address, unsigned count, uint64_t cycle);
    const uint8_t *at(Addr address) const;
    bool contains(Addr address) const;
    bool matchesPhysicalBlock(Addr block, unsigned size) const;
    bool canReuse() const { return valid && reusable; }
    Addr physical(Addr address) const;
    void invalidate();
};

class InstructionDeliveryQueue : public SimObject
{
    const unsigned capacity;
    struct ReadyRecord
    {
        FrontendRecordPtr record;
        uint64_t ready;
        uint64_t enqueued;
    };
    std::deque<ReadyRecord> records;
    std::set<InstSeqNum> inFlight;
    struct Stats : statistics::Group
    {
        statistics::Scalar enqueuedOps, sentOps, acceptedOps, cancelledOps;
        statistics::Scalar bypassedOps;
        statistics::Distribution occupancy;
        Stats(statistics::Group *parent, unsigned capacity);
    } stats;
    void checkDrain();
  public:
    InstructionDeliveryQueue(const InstructionDeliveryQueueParams &p);
    unsigned occupied() const { return records.size() + inFlight.size(); }
    unsigned capacityValue() const { return capacity; }
    unsigned freeEntries() const { return capacity - occupied(); }
    bool empty() const { return records.empty() && inFlight.empty(); }
    void enqueue(FrontendRecordPtr record, uint64_t ready, uint64_t cycle);
    bool headReady(uint64_t cycle) const;
    FrontendRecordPtr send(uint64_t cycle);
    void accepted(InstSeqNum sequence);
    void cancelAfter(InstSeqNum through);
    void reset();
    void sample() { stats.occupancy.sample(occupied()); }
    DrainState drain() override;
};

/** Bounded source stream with per-record service time and grouped IDQ delivery. */
class MiteService : public SimObject
{
    const unsigned instructionBudget, outputBudget, batchLimit;
    const Cycles latency;
    struct ReadyRecord { FrontendRecordPtr record; uint64_t ready; };
    std::deque<ReadyRecord> pending;
    InstSeqNum deliveredThrough = 0;
  protected:
    struct Stats : statistics::Group
    {
        statistics::Scalar acceptedInstructions, deliveredOps;
        statistics::Scalar activeCycles, cyclesGe6, backpressureCycles;
        statistics::Scalar cancelledOps;
        statistics::Scalar groupCreditStalls;
        statistics::Distribution delivery;
        statistics::Vector sourceOps, sourceCycles, sourceCyclesGe6;
        statistics::Vector2d sourceWidths;
        Stats(statistics::Group *parent, unsigned budget);
    } stats;
    void checkDrain();
  public:
    MiteService(const MiteServiceParams &p);
    bool idle() const { return pending.empty(); }
    unsigned freeBatchOps() const { return batchLimit - pending.size(); }
    InstSeqNum lastDelivered() const { return deliveredThrough; }
    unsigned maxInstructions() const { return instructionBudget; }
    unsigned maxBatchOps() const { return batchLimit; }
    unsigned maxOpsPerCycle() const { return outputBudget; }
    void submit(std::vector<FrontendRecordPtr> records,
                unsigned instructions, uint64_t cycle);
    unsigned deliver(InstructionDeliveryQueue &idq, uint64_t cycle);
    void cancelAfter(InstSeqNum through);
    void reset();
    DrainState drain() override;
};

struct FrontendCodeIdentity
{
    Addr address;
    uint64_t addressSpace;
    uint64_t decoderContext;
    bool operator<(const FrontendCodeIdentity &other) const
    {
        return std::tie(address, addressSpace, decoderContext) <
            std::tie(other.address, other.addressSpace, other.decoderContext);
    }
};

/** A reusable semantic expansion. It contains no dynamic prediction. */
struct FrontendTemplate
{
    FrontendCodeIdentity identity;
    StaticInstPtr macroop;
    std::vector<StaticInstPtr> operations;
    unsigned bytes;
    std::vector<Addr> physicalBlocks;
    uint64_t available;
};

class DsbTemplates : public MiteService
{
    const unsigned capacity;
    unsigned used = 0;
    uint64_t codeEpoch = 0;
    std::list<FrontendTemplate> templates;
    using Entry = std::list<FrontendTemplate>::iterator;
    std::map<FrontendCodeIdentity, Entry> index;
    struct CacheStats : statistics::Group
    {
        statistics::Scalar lookups, hits, fills, evictions;
        statistics::Scalar invalidatedTemplates, staleFills, oversizeTemplates;
        CacheStats(statistics::Group *parent);
    } cacheStats;
    void erase(Entry entry);
  public:
    DsbTemplates(const DsbTemplatesParams &p);
    uint64_t epoch() const { return codeEpoch; }
    const FrontendTemplate *lookup(const FrontendCodeIdentity &identity,
        const std::vector<Addr> &validated_blocks, uint64_t cycle);
    const FrontendTemplate *candidate(const FrontendCodeIdentity &identity,
        uint64_t cycle) const;
    bool fill(FrontendTemplate value, uint64_t accepted_epoch, uint64_t cycle);
    void invalidate(Addr physical_block);
    void invalidateAll();
};

/** One clock owner; source components own their accepting and serving state. */
class FrontendCoordinator : public SimObject
{
    struct CodeLease { Addr virtualBlock, physicalBlock; };
    std::deque<CodeLease> leases;
    uint64_t addressSpace = ~uint64_t(0), decoderContext = ~uint64_t(0);
    unsigned lineSize = 0, outputWidth = 0;
    FrontendSource selected = FrontendSource::Mite;
    bool haveSource = false, stoppedThisCycle = false;
    const Cycles switchDelay;
    struct PendingFill {
        InstSeqNum through;
        uint64_t epoch;
        std::vector<FrontendTemplate> templates;
    };
    std::deque<PendingFill> pendingFills;
    struct Stats : statistics::Group
    {
        statistics::Scalar sourceTransitions, extraSwitchCycles;
        statistics::Scalar outputs, redirects, translationValidations;
        statistics::Scalar allocationOps, allocationCycles;
        statistics::Scalar backendAcceptingCycles, backendBlockedCycles;
        statistics::Scalar notDeliveredOps, zeroDeliveryCycles;
        statistics::Vector allocationWidths, backendBlocks;
        Stats(statistics::Group *parent);
    } stats;
    void checkDrain();
  public:
    InstructionBytes *const instructionBytes;
    MiteService *const mite;
    DsbTemplates *const dsb;
    InstructionDeliveryQueue *const idq;
    enum AllocationBlock : unsigned {
        RenameSignal = 1, Recovery = 2, Serialize = 4,
        FullROB = 8, FullIQ = 16, FullLSQ = 32, FullPRF = 64
    };
    FrontendCoordinator(const FrontendCoordinatorParams &p);
    void configure(unsigned threads, unsigned width, unsigned line_size);
    bool canAccept() const;
    bool canAccept(FrontendSource source) const;
    void allocation(unsigned accepted, unsigned blocked);
    bool empty() const;
    void advance(uint64_t cycle, bool rename_blocked,
        const std::function<bool(FrontendRecordPtr)> &emit);
    void submit(FrontendSource source, std::vector<FrontendRecordPtr> records,
        unsigned instructions, std::vector<FrontendTemplate> fills,
        uint64_t cycle);
    void accepted(InstSeqNum sequence);
    void cancelAfter(InstSeqNum through);
    void reset();
    void context(uint64_t address_space, uint64_t decoder_context);
    void redirect(InstSeqNum through) { ++stats.redirects; cancelAfter(through); }
    FrontendCodeIdentity identity(Addr address) const;
    void translated(Addr virtual_address, Addr physical_address);
    bool physicalBlocks(Addr address, unsigned bytes, std::vector<Addr> &out) const;
    const FrontendTemplate *lookup(Addr address, uint64_t cycle);
    Addr validationAddress(Addr address, uint64_t cycle) const;
    void invalidateCode(Addr physical_block);
    void invalidateTranslations();
    DrainState drain() override;
};
}
#endif
