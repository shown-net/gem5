// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/frontend.hh"
#include <algorithm>
#include <cstring>
#include "base/logging.hh"

namespace gem5::o3
{
InstructionBytes::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(returnedBytes, statistics::units::Byte::get(),
               "Bytes returned by the native instruction request"),
      ADD_STAT(serviceBlocks, statistics::units::Count::get(),
               "Aligned byte blocks served to MITE"),
      ADD_STAT(invalidations, statistics::units::Count::get(),
               "Resident byte validity revocations"),
      ADD_STAT(translationWaitCycles, statistics::units::Cycle::get(),
               "Cycles Fetch waits for Execute translation"),
      ADD_STAT(responseWaitCycles, statistics::units::Cycle::get(),
               "Cycles Fetch waits for an instruction-cache response, including hits"),
      ADD_STAT(retryWaitCycles, statistics::units::Cycle::get(),
               "Cycles Fetch waits to retry the instruction-cache request")
{}

InstructionBytes::InstructionBytes(const InstructionBytesParams &p)
    : SimObject(p), budget(p.bytesPerCycle), data(p.bufferBytes), stats(this)
{
    fatal_if(!budget || (budget & (budget - 1)) || data.empty() ||
             data.size() % budget, "Invalid instruction byte geometry");
}

void
InstructionBytes::waiting(bool translation, bool response, bool retry)
{
    stats.translationWaitCycles += translation;
    stats.responseWaitCycles += response;
    stats.retryWaitCycles += retry;
}

void
InstructionBytes::complete(Addr va, Addr pa, const uint8_t *bytes,
        unsigned count, uint64_t cycle, bool can_reuse)
{
    fatal_if(count != data.size(), "Instruction response size mismatch");
    std::memcpy(data.data(), bytes, count);
    virtualBase = va;
    physicalBase = pa;
    valid = true;
    reusable = can_reuse;
    readyCycle = cycle + 1;
    stats.returnedBytes += count;
}

bool
InstructionBytes::contains(Addr address) const
{
    return valid && address >= virtualBase && address - virtualBase < data.size();
}

bool
InstructionBytes::matchesPhysicalBlock(Addr block, unsigned size) const
{
    return valid && physicalBase / size == block / size;
}

bool
InstructionBytes::serve(Addr address, unsigned count, uint64_t cycle)
{
    if (!count || !contains(address) || !contains(address + count - 1) ||
        cycle < readyCycle)
        return false;
    const Addr block = address & ~Addr(budget - 1);
    if ((address + count - 1) / budget != address / budget)
        return false;
    if (serviceCycle == cycle)
        return block == serviceBlock;
    serviceCycle = cycle;
    serviceBlock = block;
    ++stats.serviceBlocks;
    return true;
}

const uint8_t *
InstructionBytes::at(Addr address) const
{
    assert(contains(address));
    return data.data() + address - virtualBase;
}

Addr
InstructionBytes::physical(Addr address) const
{
    assert(contains(address));
    return physicalBase + address - virtualBase;
}

void
InstructionBytes::invalidate()
{
    if (valid)
        ++stats.invalidations;
    valid = reusable = false;
}

InstructionDeliveryQueue::Stats::Stats(statistics::Group *parent, unsigned cap)
    : statistics::Group(parent),
      ADD_STAT(enqueuedOps, statistics::units::Count::get(), "Admitted gem5 ops"),
      ADD_STAT(sentOps, statistics::units::Count::get(), "Ops sent to Rename"),
      ADD_STAT(acceptedOps, statistics::units::Count::get(),
               "Ops actually accepted by Rename, releasing IDQ capacity"),
      ADD_STAT(cancelledOps, statistics::units::Count::get(),
               "Queued or in-flight ops cancelled before acceptance"),
      ADD_STAT(bypassedOps, statistics::units::Count::get(),
               "New ready ops sent using the remaining ordered output budget"),
      ADD_STAT(occupancy, statistics::units::Count::get(),
               "Queued plus sent but unaccepted ops")
{
    occupancy.init(0, cap, 1);
}

InstructionDeliveryQueue::InstructionDeliveryQueue(
        const InstructionDeliveryQueueParams &p)
    : SimObject(p), capacity(p.capacity), stats(this, p.capacity)
{
    fatal_if(!capacity, "IDQ capacity must be positive");
}

void
InstructionDeliveryQueue::enqueue(FrontendRecordPtr record,
        uint64_t ready, uint64_t cycle)
{
    fatal_if(!freeEntries(), "IDQ enqueue without capacity");
    if (!records.empty())
        fatal_if(records.back().record->sequence >= record->sequence,
                 "IDQ sequence order violation: tail sn=%llu pc=%#x "
                 "fault=%s, incoming sn=%llu pc=%#x fault=%s",
                 records.back().record->sequence,
                 records.back().record->pc->instAddr(),
                 records.back().record->fault ?
                    records.back().record->fault->name() : "none",
                 record->sequence, record->pc->instAddr(),
                 record->fault ? record->fault->name() : "none");
    records.push_back({std::move(record), ready, cycle});
    ++stats.enqueuedOps;
}

bool
InstructionDeliveryQueue::headReady(uint64_t cycle) const
{
    return !records.empty() && records.front().ready <= cycle;
}

FrontendRecordPtr
InstructionDeliveryQueue::send(uint64_t cycle)
{
    assert(headReady(cycle));
    if (records.front().enqueued == cycle)
        ++stats.bypassedOps;
    auto record = std::move(records.front().record);
    records.pop_front();
    fatal_if(!inFlight.insert(record->sequence).second,
             "Duplicate IDQ send");
    ++stats.sentOps;
    return record;
}

void
InstructionDeliveryQueue::accepted(InstSeqNum sequence)
{
    fatal_if(inFlight.erase(sequence) != 1,
             "Rename acceptance without an outstanding IDQ operation");
    ++stats.acceptedOps;
    checkDrain();
}

void
InstructionDeliveryQueue::cancelAfter(InstSeqNum through)
{
    while (!records.empty() && records.back().record->sequence > through) {
        records.pop_back();
        ++stats.cancelledOps;
    }
    auto begin = inFlight.upper_bound(through);
    stats.cancelledOps += std::distance(begin, inFlight.end());
    inFlight.erase(begin, inFlight.end());
    checkDrain();
}

void InstructionDeliveryQueue::reset() { cancelAfter(0); }
void InstructionDeliveryQueue::checkDrain()
{
    if (empty() && drainState() == DrainState::Draining)
        signalDrainDone();
}
DrainState InstructionDeliveryQueue::drain()
{
    return empty() ? DrainState::Drained : DrainState::Draining;
}

MiteService::Stats::Stats(statistics::Group *parent, unsigned budget)
    : statistics::Group(parent),
      ADD_STAT(acceptedInstructions, statistics::units::Count::get(),
               "Machine instructions accepted into source service"),
      ADD_STAT(deliveredOps, statistics::units::Count::get(),
               "Gem5 operations delivered from source service into IDQ"),
      ADD_STAT(activeCycles, statistics::units::Cycle::get(),
               "Cycles delivering at least one gem5 operation into IDQ"),
      ADD_STAT(cyclesGe6, statistics::units::Cycle::get(),
               "Cycles delivering at least six proxy operations, not maximum width"),
      ADD_STAT(backpressureCycles, statistics::units::Cycle::get(),
               "Ready source cycles blocked by IDQ admission capacity"),
      ADD_STAT(cancelledOps, statistics::units::Count::get(),
               "Service operations cancelled before IDQ admission"),
      ADD_STAT(groupCreditStalls, statistics::units::Cycle::get(),
               "Ready delivery groups waiting for sufficient IDQ credits"),
      ADD_STAT(delivery, statistics::units::Count::get(),
               "Per-cycle source delivery distribution"),
      ADD_STAT(sourceOps, statistics::units::Count::get(),
               "IDQ deliveries by gem5 source, ROM is not a hardware MS mapping"),
      ADD_STAT(sourceCycles, statistics::units::Cycle::get(),
               "IDQ delivery cycles by source"),
      ADD_STAT(sourceCyclesGe6, statistics::units::Cycle::get(),
               "Source cycles delivering at least six proxy operations"),
      ADD_STAT(sourceWidths, statistics::units::Count::get(),
               "Nonempty delivery groups by source and operation count")
{
    delivery.init(0, budget, 1);
    sourceWidths.init(3, budget + 1);
    sourceWidths.subname(0, "mite");
    sourceWidths.subname(1, "dsb");
    sourceWidths.subname(2, "rom");
    for (unsigned i = 0; i <= budget; ++i)
        sourceWidths.ysubname(i, std::to_string(i));
    for (auto *v : {&sourceOps, &sourceCycles, &sourceCyclesGe6}) {
        v->init(3);
        v->subname(0, "mite");
        v->subname(1, "dsb");
        v->subname(2, "rom");
    }
}

MiteService::MiteService(const MiteServiceParams &p)
    : SimObject(p), instructionBudget(p.instructionsPerBatch),
      outputBudget(p.opsPerCycle), batchLimit(p.batchOps), latency(p.latency),
      stats(this, p.opsPerCycle)
{
    fatal_if(!instructionBudget || !outputBudget || !batchLimit,
             "Source service budgets must be positive");
}

void
MiteService::submit(std::vector<FrontendRecordPtr> records,
        unsigned instructions, uint64_t cycle)
{
    fatal_if(records.empty() || records.size() > freeBatchOps() ||
             instructions > instructionBudget,
             "Source accepting batch violates its capacity or budget");
    const uint64_t ready = std::max(cycle + uint64_t(latency),
        pending.empty() ? uint64_t(0) : pending.back().ready);
    for (auto &record : records)
        pending.push_back({std::move(record), ready});
    stats.acceptedInstructions += instructions;
}

unsigned
MiteService::deliver(InstructionDeliveryQueue &idq, uint64_t cycle)
{
    unsigned delivered = 0;
    unsigned sources[3] = {};
    unsigned group = 0;
    const unsigned limit = std::min(outputBudget, idq.capacityValue());
    for (const auto &entry : pending) {
        if (entry.ready > cycle || group == limit) break;
        ++group;
    }
    if (group && idq.freeEntries() < group) {
        if (!idq.freeEntries()) ++stats.backpressureCycles;
        ++stats.groupCreditStalls;
        group = 0;
    }
    while (delivered < group) {
        ++sources[static_cast<unsigned>(pending.front().record->source)];
        deliveredThrough = pending.front().record->sequence;
        idq.enqueue(std::move(pending.front().record), cycle, cycle);
        pending.pop_front();
        ++delivered;
    }
    stats.deliveredOps += delivered;
    if (delivered)
        ++stats.activeCycles;
    if (delivered >= 6)
        ++stats.cyclesGe6;
    stats.delivery.sample(delivered);
    for (unsigned i = 0; i < 3; ++i) {
        stats.sourceOps[i] += sources[i];
        if (sources[i]) ++stats.sourceCycles[i];
        if (sources[i] >= 6) ++stats.sourceCyclesGe6[i];
        if (sources[i]) ++stats.sourceWidths[i][sources[i]];
    }
    checkDrain();
    return delivered;
}

void
MiteService::cancelAfter(InstSeqNum through)
{
    while (!pending.empty() && pending.back().record->sequence > through) {
        pending.pop_back();
        ++stats.cancelledOps;
    }
    checkDrain();
}
void MiteService::reset() { cancelAfter(0); }
void MiteService::checkDrain()
{
    if (idle() && drainState() == DrainState::Draining)
        signalDrainDone();
}
DrainState MiteService::drain()
{
    return idle() ? DrainState::Drained : DrainState::Draining;
}

DsbTemplates::CacheStats::CacheStats(statistics::Group *parent)
    : statistics::Group(parent, "templates"),
      ADD_STAT(lookups, statistics::units::Count::get(), "Template lookups"),
      ADD_STAT(hits, statistics::units::Count::get(), "Validated template hits"),
      ADD_STAT(fills, statistics::units::Count::get(), "Complete template fills"),
      ADD_STAT(evictions, statistics::units::Count::get(), "Whole LRU evictions"),
      ADD_STAT(invalidatedTemplates, statistics::units::Count::get(),
               "Templates invalidated by code or translation context changes"),
      ADD_STAT(staleFills, statistics::units::Count::get(),
               "Fills rejected after an intervening invalidation"),
      ADD_STAT(oversizeTemplates, statistics::units::Count::get(),
               "Templates exceeding the configured operation capacity")
{}

DsbTemplates::DsbTemplates(const DsbTemplatesParams &p)
    : MiteService(p), capacity(p.capacity), cacheStats(this)
{}

void
DsbTemplates::erase(Entry entry)
{
    used -= entry->operations.size();
    index.erase(entry->identity);
    templates.erase(entry);
}

const FrontendTemplate *
DsbTemplates::candidate(const FrontendCodeIdentity &identity,
        uint64_t cycle) const
{
    auto found = index.find(identity);
    return found != index.end() && found->second->available <= cycle ?
        &*found->second : nullptr;
}

const FrontendTemplate *
DsbTemplates::lookup(const FrontendCodeIdentity &identity,
        const std::vector<Addr> &validated_blocks, uint64_t cycle)
{
    ++cacheStats.lookups;
    auto found = index.find(identity);
    if (found == index.end())
        return nullptr;
    const auto entry = found->second;
    if (entry->available > cycle)
        return nullptr;
    if (entry->physicalBlocks != validated_blocks) {
        ++cacheStats.invalidatedTemplates;
        erase(entry);
        return nullptr;
    }
    templates.splice(templates.begin(), templates, entry);
    ++cacheStats.hits;
    return &templates.front();
}

bool
DsbTemplates::fill(FrontendTemplate value, uint64_t accepted_epoch,
        uint64_t cycle)
{
    if (accepted_epoch != codeEpoch) {
        ++cacheStats.staleFills;
        return false;
    }
    if (!capacity || value.operations.empty() || value.physicalBlocks.empty())
        return false;
    if (value.operations.size() > capacity) {
        ++cacheStats.oversizeTemplates;
        return false;
    }
    for (size_t i = 0; i < value.operations.size(); ++i) {
        const auto &op = value.operations[i];
        if (op->isControl() && (i + 1 != value.operations.size() ||
            (op->isMicroop() && !op->isLastMicroop())))
            return false;
    }
    auto existing = index.find(value.identity);
    if (existing != index.end())
        erase(existing->second);
    while (used + value.operations.size() > capacity) {
        ++cacheStats.evictions;
        erase(std::prev(templates.end()));
    }
    used += value.operations.size();
    value.available = cycle + 1;
    templates.push_front(std::move(value));
    index.emplace(templates.front().identity, templates.begin());
    ++cacheStats.fills;
    return true;
}

void
DsbTemplates::invalidate(Addr physical_block)
{
    ++codeEpoch;
    for (auto it = templates.begin(); it != templates.end();) {
        auto entry = it++;
        if (std::find(entry->physicalBlocks.begin(), entry->physicalBlocks.end(),
                      physical_block) != entry->physicalBlocks.end()) {
            ++cacheStats.invalidatedTemplates;
            erase(entry);
        }
    }
}

void
DsbTemplates::invalidateAll()
{
    ++codeEpoch;
    cacheStats.invalidatedTemplates += templates.size();
    templates.clear();
    index.clear();
    used = 0;
}

FrontendCoordinator::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(sourceTransitions, statistics::units::Count::get(),
               "Source changes at batch boundaries"),
      ADD_STAT(extraSwitchCycles, statistics::units::Cycle::get(),
               "Configured extra DSB to MITE source wait"),
      ADD_STAT(outputs, statistics::units::Count::get(), "IDQ output operations"),
      ADD_STAT(redirects, statistics::units::Count::get(), "Frontend cancellations"),
      ADD_STAT(translationValidations, statistics::units::Count::get(),
               "Native Execute translation completions used to validate code"),
      ADD_STAT(allocationOps, statistics::units::Count::get(),
               "Frontend records actually accepted by Rename"),
      ADD_STAT(allocationCycles, statistics::units::Cycle::get(),
               "Observed active Rename cycles"),
      ADD_STAT(backendAcceptingCycles, statistics::units::Cycle::get(),
               "Rename cycles with full-width resource headroom and no block"),
      ADD_STAT(backendBlockedCycles, statistics::units::Cycle::get(),
               "Rename cycles excluded from frontend delivery shortage"),
      ADD_STAT(notDeliveredOps, statistics::units::Count::get(),
               "Missing allocation slots while the model backend can accept"),
      ADD_STAT(zeroDeliveryCycles, statistics::units::Cycle::get(),
               "Backend-accepting cycles with no actual Rename acceptance"),
      ADD_STAT(allocationWidths, statistics::units::Count::get(),
               "Actual Rename acceptance width on backend-accepting cycles"),
      ADD_STAT(backendBlocks, statistics::units::Cycle::get(),
               "Exclusion reasons; multiple reasons may overlap")
{}

FrontendCoordinator::FrontendCoordinator(const FrontendCoordinatorParams &p)
    : SimObject(p), switchDelay(p.dsbToMiteDelay), stats(this),
      instructionBytes(p.instructionBytes), mite(p.mite), dsb(p.dsb), idq(p.idq)
{}

void
FrontendCoordinator::configure(unsigned threads, unsigned width, unsigned size)
{
    fatal_if(threads != 1 || !width || !size || (size & (size - 1)),
             "Component frontend requires one thread and valid geometry");
    outputWidth = width;
    lineSize = size;
    stats.allocationWidths.init(width + 1);
    for (unsigned i = 0; i <= width; ++i)
        stats.allocationWidths.subname(i, std::to_string(i));
    stats.backendBlocks.init(7);
    const char *reasons[] = {"signal", "recovery", "serialize", "rob", "iq", "lsq", "prf"};
    for (unsigned i = 0; i < 7; ++i) stats.backendBlocks.subname(i, reasons[i]);
}

bool FrontendCoordinator::canAccept() const
{
    return !stoppedThisCycle && (selected == FrontendSource::Dsb ?
        dsb->freeBatchOps() : mite->freeBatchOps());
}
bool FrontendCoordinator::canAccept(FrontendSource source) const
{
    if (!canAccept()) return false;
    if ((!mite->idle() || !dsb->idle()) && source != selected) return false;
    return source == FrontendSource::Dsb ? dsb->freeBatchOps() : mite->freeBatchOps();
}
void FrontendCoordinator::allocation(unsigned accepted, unsigned blocked)
{
    panic_if(accepted > outputWidth, "Rename acceptance exceeds allocation width");
    ++stats.allocationCycles;
    stats.allocationOps += accepted;
    if (blocked) {
        ++stats.backendBlockedCycles;
        for (unsigned i = 0; i < 7; ++i)
            if (blocked & (1U << i)) ++stats.backendBlocks[i];
    } else {
        ++stats.backendAcceptingCycles;
        ++stats.allocationWidths[accepted];
        stats.notDeliveredOps += outputWidth - accepted;
        if (!accepted) ++stats.zeroDeliveryCycles;
    }
}
bool FrontendCoordinator::empty() const
{
    return mite->idle() && dsb->idle() && idq->empty() && pendingFills.empty();
}

void
FrontendCoordinator::advance(uint64_t cycle, bool rename_blocked,
        const std::function<bool(FrontendRecordPtr)> &emit)
{
    stoppedThisCycle = false;
    unsigned sent = 0;
    auto output = [&] {
        while (!rename_blocked && !stoppedThisCycle && sent < outputWidth &&
               idq->headReady(cycle)) {
            auto record = idq->send(cycle);
            ++sent;
            ++stats.outputs;
            if (!emit(std::move(record))) stoppedThisCycle = true;
        }
    };
    output();
    fatal_if(!mite->idle() && !dsb->idle(), "Concurrent MITE and DSB batches");
    if (!stoppedThisCycle) {
        if (selected == FrontendSource::Dsb) dsb->deliver(*idq, cycle);
        else mite->deliver(*idq, cycle);
        const InstSeqNum delivered = std::max(mite->lastDelivered(), dsb->lastDelivered());
        while (!pendingFills.empty() && pendingFills.front().through <= delivered) {
            for (auto &value : pendingFills.front().templates)
                dsb->fill(std::move(value), pendingFills.front().epoch, cycle);
            pendingFills.pop_front();
        }
        output();
    }
    idq->sample();
    checkDrain();
}

void
FrontendCoordinator::submit(FrontendSource source,
        std::vector<FrontendRecordPtr> records, unsigned instructions,
        std::vector<FrontendTemplate> fills, uint64_t cycle)
{
    fatal_if(!canAccept(source), "Frontend submitted while accepting is blocked");
    Cycles wait(0);
    if (haveSource && selected != source) {
        ++stats.sourceTransitions;
        if (selected == FrontendSource::Dsb && source == FrontendSource::Mite) {
            wait = switchDelay;
            stats.extraSwitchCycles += uint64_t(wait);
        }
    }
    selected = source;
    haveSource = true;
    if (!fills.empty())
        pendingFills.push_back({records.back()->sequence, dsb->epoch(), std::move(fills)});
    auto *service = source == FrontendSource::Dsb ? dsb : mite;
    service->submit(std::move(records), instructions, cycle + uint64_t(wait));
}

void FrontendCoordinator::accepted(InstSeqNum sequence)
{
    idq->accepted(sequence);
    checkDrain();
}
void FrontendCoordinator::cancelAfter(InstSeqNum through)
{
    mite->cancelAfter(through);
    dsb->cancelAfter(through);
    idq->cancelAfter(through);
    pendingFills.clear();
    checkDrain();
}
void FrontendCoordinator::reset()
{
    cancelAfter(0);
    dsb->invalidateAll();
    instructionBytes->invalidate();
    leases.clear();
    haveSource = false;
}

void
FrontendCoordinator::context(uint64_t space, uint64_t decoder)
{
    if (addressSpace != space || decoderContext != decoder) {
        addressSpace = space;
        decoderContext = decoder;
        invalidateTranslations();
    }
}
FrontendCodeIdentity FrontendCoordinator::identity(Addr address) const
{
    return {address, addressSpace, decoderContext};
}
void
FrontendCoordinator::translated(Addr va, Addr pa)
{
    va &= ~Addr(lineSize - 1);
    pa &= ~Addr(lineSize - 1);
    leases.erase(std::remove_if(leases.begin(), leases.end(),
        [&](const CodeLease &lease) { return lease.virtualBlock == va; }), leases.end());
    leases.push_back({va, pa});
    // Two adjacent blocks suffice for a maximum-length x86 instruction.
    while (leases.size() > 2) leases.pop_front();
    ++stats.translationValidations;
}

bool
FrontendCoordinator::physicalBlocks(Addr address, unsigned bytes,
        std::vector<Addr> &out) const
{
    out.clear();
    for (Addr block = address & ~Addr(lineSize - 1);
         block <= ((address + std::max(bytes, 1u) - 1) & ~Addr(lineSize - 1));
         block += lineSize) {
        auto found = std::find_if(leases.begin(), leases.end(),
            [&](const CodeLease &lease) { return lease.virtualBlock == block; });
        if (found == leases.end()) return false;
        out.push_back(found->physicalBlock);
    }
    return true;
}

const FrontendTemplate *
FrontendCoordinator::lookup(Addr address, uint64_t cycle)
{
    const auto *value = dsb->candidate(identity(address), cycle);
    if (!value) return dsb->lookup(identity(address), {}, cycle);
    std::vector<Addr> blocks;
    if (!physicalBlocks(address, value->bytes, blocks)) return nullptr;
    return dsb->lookup(identity(address), blocks, cycle);
}

Addr
FrontendCoordinator::validationAddress(Addr address, uint64_t cycle) const
{
    const auto *value = dsb->candidate(identity(address), cycle);
    const unsigned bytes = value ? value->bytes : 1;
    for (Addr block = address & ~Addr(lineSize - 1);
         block <= ((address + bytes - 1) & ~Addr(lineSize - 1)); block += lineSize) {
        if (std::none_of(leases.begin(), leases.end(),
            [&](const CodeLease &lease) { return lease.virtualBlock == block; }))
            return block;
    }
    return address;
}

void FrontendCoordinator::invalidateCode(Addr block)
{
    dsb->invalidate(block);
    leases.erase(std::remove_if(leases.begin(), leases.end(),
        [&](const CodeLease &lease) { return lease.physicalBlock == block; }), leases.end());
    if (instructionBytes->matchesPhysicalBlock(block, lineSize))
        instructionBytes->invalidate();
}
void FrontendCoordinator::invalidateTranslations()
{
    leases.clear();
    instructionBytes->invalidate();
    dsb->invalidateAll();
    pendingFills.clear();
}
void FrontendCoordinator::checkDrain()
{
    if (empty() && drainState() == DrainState::Draining) signalDrainDone();
}
DrainState FrontendCoordinator::drain()
{
    return empty() ? DrainState::Drained : DrainState::Draining;
}
}
