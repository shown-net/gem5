// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_FRONTEND_HH__
#define __CPU_O3_FRONTEND_HH__

#include <array>
#include <optional>
#include "fst_instruction_fetch.hh"
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>
#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/frontend_record.hh"
#include "params/DsbTemplates.hh"
#include "params/FrontendCoordinator.hh"
#include "params/InstructionBytes.hh"
#include "params/InstructionDeliveryQueue.hh"
#include "params/MiteService.hh"
#include "params/MicrocodeService.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5::o3
{
class CPU;
class InstructionBytes : public SimObject
{
    const unsigned budget;
    struct Block
    {
        std::vector<uint8_t> data;
        bool valid = false, reusable = false;
        Addr virtualBase = 0, physicalBase = 0;
        uint64_t readyCycle = 0;
        std::optional<fastsim::fst::TranslationFragment> translation;
    };
    std::vector<Block> blocks;
    unsigned nextBlock = 0;
    const Block *find(Addr address) const;
    uint64_t serviceCycle = ~uint64_t(0);
    Addr serviceBlock = 0;
    struct Stats : statistics::Group
    {
        statistics::Scalar returnedBytes, serviceBlocks, invalidations;
        statistics::Scalar translationWaitCycles, responseWaitCycles, retryWaitCycles;
        Stats(statistics::Group *parent);
    } stats;
  public:
    const bool lookaheadEnabled;
    const unsigned requestDepth;
    unsigned residentCapacity() const { return blocks.size(); }
    uint64_t nextReady(uint64_t cycle) const;
    InstructionBytes(const InstructionBytesParams &p);
    void waiting(bool translation, bool response, bool retry);
    void complete(Addr virtual_address, Addr physical_address,
                  const uint8_t *bytes, unsigned count,
                  uint64_t cycle, bool can_reuse,
                  std::optional<fastsim::fst::TranslationFragment> facts = std::nullopt);
    fastsim::fst::TranslationFragment instructionTranslation(Addr address, unsigned count) const;
    enum class ChunkResult { Missing, Invalidated, NotReady, Served };
    ChunkResult copyChunk(Addr address, unsigned count, uint64_t cycle,
        const std::function<bool(Addr)> &pendingWrite, void *output,
        Addr &physicalAddress, bool &reusable);
    bool contains(Addr address) const;
    bool matchesPhysicalBlock(Addr block, unsigned size) const;
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
    struct InFlight { unsigned units; AllocationGroupPtr group; };
    std::map<InstSeqNum, InFlight> inFlight;
    unsigned queuedSlots = 0, inFlightSlots = 0;
    struct Stats : statistics::Group
    {
        statistics::Scalar enqueuedOps, sentOps, acceptedOps, cancelledOps;
        statistics::Scalar bypassedOps;
        statistics::Scalar enqueuedUnits, sentUnits, acceptedUnits, cancelledUnits;
        statistics::Distribution occupancy;
        Stats(statistics::Group *parent, unsigned capacity);
    } stats;
    void checkDrain();
  public:
    InstructionDeliveryQueue(const InstructionDeliveryQueueParams &p);
    unsigned occupied() const { return queuedSlots + inFlightSlots; }
    unsigned capacityValue() const { return capacity; }
    unsigned freeEntries() const { return capacity - occupied(); }
    bool empty() const { return records.empty() && inFlight.empty(); }
    void enqueue(FrontendRecordPtr record, uint64_t ready, uint64_t cycle);
    bool markCodeWrite(ThreadID tid, InstSeqNum after, Addr block);
    FrontendRecord *findRecord(InstSeqNum sequence);
    void collectTruncatedGroups(InstSeqNum through,
        std::set<AllocationGroupPtr> &groups) const;
    void refreshGroups(const std::set<AllocationGroupPtr> &groups);
    bool headReady(uint64_t cycle) const;
    uint64_t nextReady(uint64_t cycle) const
    { return !records.empty() && records.front().ready > cycle ? records.front().ready : ~uint64_t(0); }
    unsigned headSlots() const { return records.front().record->demand.idqUnits; }
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
        statistics::Scalar acceptedInstructions, deliveredOps, deliveredSlots;
        statistics::Scalar activeCycles, cyclesGe6, backpressureCycles;
        statistics::Scalar cancelledOps;
        statistics::Scalar groupCreditStalls;
        statistics::Distribution delivery;
        statistics::Vector sourceOps, sourceCycles, sourceCyclesGe6;
        statistics::Vector2d sourceWidths;
        statistics::Vector sourceSlots, sourceSlotCycles, sourceSlotCyclesGe6;
        statistics::Vector2d sourceSlotWidths;
        Stats(statistics::Group *parent, unsigned budget, unsigned batch_limit);
    } stats;
    void checkDrain();
  public:
    MiteService(const MiteServiceParams &p);
    bool idle() const { return pending.empty(); }
    bool preparingHead(uint64_t cycle) const
    { return !pending.empty() && pending.front().ready > cycle; }
    uint64_t nextReady(uint64_t cycle) const
    { return preparingHead(cycle) ? pending.front().ready : ~uint64_t(0); }
    unsigned freeBatchOps() const { return batchLimit - pending.size(); }
    InstSeqNum lastDelivered() const { return deliveredThrough; }
    unsigned maxInstructions() const { return instructionBudget; }
    unsigned extractionWidth() const { return outputBudget; }
    const FrontendRecord *headRecord() const
    { return pending.empty() ? nullptr : pending.front().record.get(); }
    unsigned maxBatchOps() const { return batchLimit; }
    unsigned maxOpsPerCycle() const { return outputBudget; }
    InstSeqNum headSequence() const
    { return pending.empty() ? ~InstSeqNum(0) : pending.front().record->sequence; }
    void collectTruncatedGroups(InstSeqNum through,
        std::set<AllocationGroupPtr> &groups) const;
    void refreshGroups(const std::set<AllocationGroupPtr> &groups);
    void refreshTailGroup(const AllocationGroupPtr &group);
    bool markCodeWrite(ThreadID tid, InstSeqNum after, Addr block);
    FrontendRecord *findRecord(InstSeqNum sequence);
    void submit(std::vector<FrontendRecordPtr> records,
                unsigned instructions, uint64_t cycle);
    unsigned deliver(InstructionDeliveryQueue &idq, uint64_t cycle,
                     InstSeqNum before,
                     const std::function<void(const FrontendRecord &,
                                              uint64_t)> &observe);
    void cancelAfter(InstSeqNum through);
    void reset();
    DrainState drain() override;
};

class MicrocodeService : public MiteService
{
  public:
    explicit MicrocodeService(const MicrocodeServiceParams &p) : MiteService(p) {}
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
    unsigned encodedSlots() const;
    // A fill-time path boundary; not a claim about undisclosed hardware tags.
    bool endsEntry = false;
    std::array<uint8_t, 15> functionalBytes{};
    unsigned functionalByteCount = 0;
    uint64_t functionalDecoderContext = 0;
};

// Transient fill identity; the installed template remains purely static.
struct FrontendFillCandidate
{
    FrontendTemplate value;
    InstSeqNum first, last;
    FrontendRecord *lastRecord;
};

struct DsbLookupResult
{
    enum class State { Miss, NeedsValidation, Hit } state;
    const FrontendTemplate *value = nullptr;
    Addr missingBlock = 0;
};

class DsbTemplates : public MiteService
{
    const unsigned numSets, associativity, opsPerEntry, maxRegionOps;
    uint64_t codeEpoch = 0;
    struct CacheEntry {
        std::vector<FrontendTemplate> members;
        unsigned units = 0;
        bool closed = false;
    };
    using Set = std::list<CacheEntry>;
    // Replacement and path packing depend only on the order within a set.
    std::vector<Set> sets;
    using Entry = Set::iterator;
    struct Location { Entry entry; size_t member; };
    std::map<FrontendCodeIdentity, Location> index;
    std::map<Addr, std::set<CacheEntry *>> physicalIndex;
    struct CacheStats : statistics::Group
    {
        statistics::Scalar lookups, hits, fills, evictions;
        statistics::Scalar invalidatedTemplates, staleFills, oversizeTemplates;
        CacheStats(statistics::Group *parent);
    } cacheStats;
    void erase(Entry entry);
    unsigned setIndex(Addr address) const;
  public:
    DsbTemplates(const DsbTemplatesParams &p);
    uint64_t epoch() const { return codeEpoch; }
    DsbLookupResult lookup(const FrontendCodeIdentity &identity, uint64_t cycle,
        const std::function<bool(Addr, unsigned, std::vector<Addr> &, Addr &)> &resolve);
    const FrontendTemplate *candidate(const FrontendCodeIdentity &identity,
        uint64_t cycle) const;
    bool fill(FrontendTemplate value, uint64_t accepted_epoch, uint64_t cycle);
    void invalidate(Addr physical_block);
    void invalidateAll();
};

/** One clock owner; source components own their accepting and serving state. */
class FrontendCoordinator : public SimObject
{
    CPU *owner = nullptr;
    EventFunctionWrapper wakeEvent;
    struct CodeLease { Addr virtualBlock, physicalBlock;
        std::optional<fastsim::fst::TranslationFragment> translation; };
    std::deque<CodeLease> leases;
    uint64_t addressSpace = ~uint64_t(0), decoderContext = ~uint64_t(0);
    unsigned lineSize = 0, outputWidth = 0;
    const bool enableLsdReplay;
    FrontendSource selected = FrontendSource::Mite;
    bool haveSource = false, stoppedThisCycle = false;
    // Diagnostic source history excludes functional records with zero units.
    FrontendSource positiveDeliverySource = FrontendSource::Mite;
    bool havePositiveDeliverySource = false;
    struct PendingFill {
        InstSeqNum first, last;
        uint64_t epoch;
        AllocationGroupPtr group;
        FrontendTemplate value;
    };
    std::deque<PendingFill> pendingFills;
    // A simple-loop candidate, not a claim about the physical LSD directory.
    std::vector<FrontendTemplate> loopCapture, loopTemplates;
    std::optional<FrontendTemplate> loopBuilding;
    unsigned loopCaptureUnits = 0;
    FrontendRecord *fusionTail = nullptr;
    std::shared_ptr<bool> decodingMiss;
    struct MsInterval { InstSeqNum first, last; };
    std::vector<MsInterval> msIntervals;
    bool msInputOpen = false;
    // Physical code invalidations revoke matching requests separately.
    uint64_t translationEpochValue = 0;
    std::function<void(const FrontendRecord &, uint64_t)> deliveryObserver;
    void observeLoop(const FrontendRecord &record);
    void clearLoop();
    void fuseAcrossBatches(std::vector<FrontendRecordPtr> &records);
    struct Stats : statistics::Group
    {
        statistics::Scalar sourceTransitions, dsbToMiteDeliveries;
        statistics::Scalar switchWaitCycles, switchWaitRenameBlocked;
        statistics::Scalar switchWaitIdqFull;
        statistics::Scalar outputs, redirects, translationValidations;
        statistics::Scalar allocationOps, allocationSlots, allocationCycles;
        statistics::Scalar notDeliveredSlots, fullReceiverNotDeliveredSlots;
        statistics::Scalar backendAcceptingCycles, backendBlockedCycles;
        statistics::Scalar notDeliveredOps, zeroDeliveryCycles;
        statistics::Scalar fullDeliveryCycles, receiverUnknownCycles;
        statistics::Scalar partialReceiverCycles;
        statistics::Vector allocationWidths, backendBlocks;
        statistics::Vector sqHeadroomAcceptingCycles, sqHeadroomMissingSlots;
        Stats(statistics::Group *parent);
    } stats;
    void checkDrain();
  public:
    void attachCPU(CPU *cpu) { owner = cpu; }
    void refreshWake();
    void cancelWake();
    InstructionBytes *const instructionBytes;
    MiteService *const mite;
    DsbTemplates *const dsb;
    MicrocodeService *const ms;
    MiteService *const lsd;
    InstructionDeliveryQueue *const idq;
    enum AllocationBlock : unsigned {
        RenameSignal = 1, Recovery = 2, Serialize = 4,
        FullROB = 8, FullIQ = 16, FullLSQ = 32, FullPRF = 64
    };
    FrontendCoordinator(const FrontendCoordinatorParams &p);
    static bool fusionEligibleProducer(const StaticInst &inst);
    void configure(unsigned threads, unsigned width, unsigned line_size);
    bool canAccept() const;
    bool canAccept(FrontendSource source) const;
    MiteService *sourceService(FrontendSource source) const;
    void allocation(unsigned accepted, unsigned slots);
    void decodingDsbMiss(std::shared_ptr<bool> exposed)
    { decodingMiss = std::move(exposed); }
    void decodedDsbMiss(const std::shared_ptr<bool> &exposed)
    { if (decodingMiss == exposed) decodingMiss.reset(); }
    bool empty() const;
    bool readySupply(uint64_t cycle) const
    {
        if (idq->headReady(cycle)) return true;
        for (auto *service : {mite, static_cast<MiteService *>(dsb),
                              static_cast<MiteService *>(ms), lsd})
            if (!service->idle() && !service->preparingHead(cycle)) return true;
        return false;
    }
    void advance(uint64_t cycle, bool rename_blocked,
        const TimeStruct::RenameComm &receiver,
        const std::function<bool(FrontendRecordPtr)> &emit,
        bool hold = false);
    void submit(FrontendSource source, std::vector<FrontendRecordPtr> records,
        unsigned instructions, std::vector<FrontendFillCandidate> fills,
        uint64_t cycle);
    void accepted(InstSeqNum sequence);
    void cancelAfter(InstSeqNum through);
    void reset();
    void finishLookahead() { fusionTail = nullptr; }
    bool context(uint64_t address_space, uint64_t decoder_context);
    void redirect(InstSeqNum through) { ++stats.redirects; cancelAfter(through); }
    FrontendCodeIdentity identity(Addr address) const;
    void translated(Addr virtual_address, Addr physical_address,
        std::optional<fastsim::fst::TranslationFragment> facts = std::nullopt);
    std::shared_ptr<const fastsim::fst::InstructionFetchFacts> instructionTranslation(Addr address, unsigned bytes) const;
    bool physicalBlocks(Addr address, unsigned bytes, std::vector<Addr> &out) const;
    bool physicalBlocks(Addr address, unsigned bytes,
                        std::array<Addr, 2> &out, unsigned &count) const;
    DsbLookupResult lookup(Addr address, uint64_t cycle);
    const FrontendTemplate *loopCandidate(Addr address) const;
    void setDeliveryObserver(
        std::function<void(const FrontendRecord &, uint64_t)> observer)
    { deliveryObserver = std::move(observer); }
    void invalidateCode(Addr physical_block);
    enum class LateRetarget { Missing, Applied, Replay, AwaitBackend };
    LateRetarget retargetRecord(InstSeqNum sequence, const PCStateBase &target,
        uint64_t version, InstSeqNum &boundary, std::unique_ptr<PCStateBase> &restart);
    std::vector<Addr> virtualCodeBlocks(Addr physical_block) const;
    bool markCodeWrite(ThreadID tid, InstSeqNum after, Addr block);
    void invalidateTranslations();
    uint64_t translationEpoch() const { return translationEpochValue; }
    DrainState drain() override;
};
}
#endif
