// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/frontend.hh"
#include <algorithm>
#include <cstring>
#include "arch/x86/insts/static_inst.hh"
#include "arch/x86/pcstate.hh"
#include "base/logging.hh"
#include "cpu/o3/limits.hh"

namespace gem5::o3
{
namespace {
void describeDemand(FrontendRecord &r)
{
    r.demand = initialDemand(*r.instruction, r.macroop.get());
    if (!r.allocationGroup) return;
    const auto &g = *r.allocationGroup;
    const bool first = r.sequence == g.leader;
    r.demand.frontendUnits = first;
    r.demand.idqUnits = first;
    r.demand.allocationUnits = first;
    r.demand.allocationGroup = g.leader;
    r.demand.allocationLast = g.end;
    if (!g.independentBackendEntries) {
        r.demand.robUnits = first;
        r.demand.robGroup = g.leader;
        r.demand.robLast = g.end;
        r.demand.schedulerGroup = g.leader;
    }
    r.demand.retirementGroup = g.leader;
    r.demand.retirementLast = g.end;
    r.demand.retirementUops = first;
    r.demand.schedulerActions = r.sequence != g.branch;
}
}
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
    : SimObject(p), budget(p.bytesPerCycle), blocks(p.residentBlocks), stats(this),
      lookaheadEnabled(p.lookaheadEnabled), requestDepth(p.requestDepth)
{
    fatal_if(!budget || (budget & (budget - 1)) || !p.bufferBytes ||
             p.bufferBytes % budget || !requestDepth ||
             blocks.size() < requestDepth + 1,
             "Invalid instruction byte geometry");
    for (auto &block : blocks)
        block.data.resize(p.bufferBytes);
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
    auto &block = blocks[nextBlock];
    nextBlock = (nextBlock + 1) % blocks.size();
    fatal_if(count != block.data.size(), "Instruction response size mismatch");
    std::memcpy(block.data.data(), bytes, count);
    block.virtualBase = va;
    block.physicalBase = pa;
    block.valid = true;
    block.reusable = can_reuse;
    block.readyCycle = cycle + 1;
    stats.returnedBytes += count;
}

const InstructionBytes::Block *
InstructionBytes::find(Addr address) const
{
    for (const auto &block : blocks)
        if (block.valid && address >= block.virtualBase &&
            address - block.virtualBase < block.data.size())
            return &block;
    return nullptr;
}

bool
InstructionBytes::contains(Addr address) const
{
    return find(address);
}

bool
InstructionBytes::matchesPhysicalBlock(Addr block, unsigned size) const
{
    return std::any_of(blocks.begin(), blocks.end(), [&](const auto &entry) {
        return entry.valid && entry.physicalBase / size == block / size;
    });
}

bool
InstructionBytes::serve(Addr address, unsigned count, uint64_t cycle)
{
    const auto *resident = find(address);
    if (!count || !resident || find(address + count - 1) != resident ||
        cycle < resident->readyCycle)
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
    const auto *block = find(address);
    assert(block);
    return block->data.data() + address - block->virtualBase;
}

Addr
InstructionBytes::physical(Addr address) const
{
    const auto *block = find(address);
    assert(block);
    return block->physicalBase + address - block->virtualBase;
}

void
InstructionBytes::invalidate()
{
    for (auto &block : blocks) {
        if (block.valid)
            ++stats.invalidations;
        block.valid = block.reusable = false;
    }
    nextBlock = 0;
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
      ADD_STAT(enqueuedUnits, statistics::units::Count::get(), "IDQ units admitted"),
      ADD_STAT(sentUnits, statistics::units::Count::get(), "IDQ units sent downstream"),
      ADD_STAT(acceptedUnits, statistics::units::Count::get(), "IDQ units released by acceptance"),
      ADD_STAT(cancelledUnits, statistics::units::Count::get(), "IDQ units cancelled"),
      ADD_STAT(occupancy, statistics::units::Count::get(),
               "Queued plus sent but unaccepted IDQ units")
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
    fatal_if(freeEntries() < record->demand.idqUnits, "IDQ enqueue without capacity");
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
    queuedSlots += record->demand.idqUnits;
    stats.enqueuedUnits += record->demand.idqUnits;
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
    queuedSlots -= record->demand.idqUnits;
    inFlightSlots += record->demand.idqUnits;
    stats.sentUnits += record->demand.idqUnits;
    fatal_if(!inFlight.emplace(record->sequence, record->demand.idqUnits).second,
             "Duplicate IDQ send");
    ++stats.sentOps;
    return record;
}

void
InstructionDeliveryQueue::accepted(InstSeqNum sequence)
{
    auto found = inFlight.find(sequence);
    fatal_if(found == inFlight.end(),
             "Rename acceptance without an outstanding IDQ operation");
    inFlightSlots -= found->second;
    stats.acceptedUnits += found->second;
    inFlight.erase(found);
    ++stats.acceptedOps;
    checkDrain();
}

void
InstructionDeliveryQueue::cancelAfter(InstSeqNum through)
{
    while (!records.empty() && records.back().record->sequence > through) {
        queuedSlots -= records.back().record->demand.idqUnits;
        stats.cancelledUnits += records.back().record->demand.idqUnits;
        if (records.back().record->allocationGroup)
            records.back().record->allocationGroup->truncate(through);
        records.pop_back();
        ++stats.cancelledOps;
    }
    auto begin = inFlight.upper_bound(through);
    for (auto it = begin; it != inFlight.end(); ++it) {
        inFlightSlots -= it->second;
        stats.cancelledUnits += it->second;
    }
    stats.cancelledOps += std::distance(begin, inFlight.end());
    inFlight.erase(begin, inFlight.end());
    for (auto &entry : records) describeDemand(*entry.record);
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

MiteService::Stats::Stats(statistics::Group *parent, unsigned budget,
                          unsigned batch_limit)
    : statistics::Group(parent),
      ADD_STAT(acceptedInstructions, statistics::units::Count::get(),
               "Machine instructions accepted into source service"),
      ADD_STAT(deliveredOps, statistics::units::Count::get(),
               "Gem5 operations delivered from source service into IDQ"),
      ADD_STAT(deliveredSlots, statistics::units::Count::get(),
               "IDQ units delivered from source service into IDQ"),
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
               "IDQ deliveries by model source; microcode loops are not hardware MS uops"),
      ADD_STAT(sourceCycles, statistics::units::Cycle::get(),
               "IDQ delivery cycles by source"),
      ADD_STAT(sourceCyclesGe6, statistics::units::Cycle::get(),
               "Source cycles delivering at least six proxy operations"),
      ADD_STAT(sourceWidths, statistics::units::Count::get(),
               "Nonempty delivery groups by source and operation count"),
      ADD_STAT(sourceSlots, statistics::units::Count::get(),
               "IDQ unit deliveries by model source"),
      ADD_STAT(sourceSlotCycles, statistics::units::Cycle::get(),
               "Cycles delivering allocation units by model source"),
      ADD_STAT(sourceSlotCyclesGe6, statistics::units::Cycle::get(),
               "Source cycles delivering at least six allocation units"),
      ADD_STAT(sourceSlotWidths, statistics::units::Count::get(),
               "Nonempty delivery groups by source and allocation-unit count")
{
    const unsigned max_records = std::max(4 * budget, batch_limit);
    delivery.init(0, max_records, 1);
    sourceWidths.init(4, max_records + 1);
    sourceSlotWidths.init(4, budget + 1);
    sourceWidths.subname(0, "mite");
    sourceWidths.subname(1, "dsb");
    sourceWidths.subname(2, "ms");
    sourceWidths.subname(3, "lsd");
    sourceSlotWidths.subname(0, "mite");
    sourceSlotWidths.subname(1, "dsb");
    sourceSlotWidths.subname(2, "ms");
    sourceSlotWidths.subname(3, "lsd");
    for (unsigned i = 0; i <= max_records; ++i)
        sourceWidths.ysubname(i, std::to_string(i));
    for (unsigned i = 0; i <= budget; ++i)
        sourceSlotWidths.ysubname(i, std::to_string(i));
    for (auto *v : {&sourceOps, &sourceCycles, &sourceCyclesGe6,
                    &sourceSlots, &sourceSlotCycles, &sourceSlotCyclesGe6}) {
        v->init(4);
        v->subname(0, "mite");
        v->subname(1, "dsb");
        v->subname(2, "ms");
        v->subname(3, "lsd");
    }
}

MiteService::MiteService(const MiteServiceParams &p)
    : SimObject(p), instructionBudget(p.instructionsPerBatch),
      outputBudget(p.opsPerCycle), batchLimit(p.batchOps), latency(p.latency),
      stats(this, p.opsPerCycle, p.batchOps)
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

void MiteService::refreshDemands()
{
    for (auto &entry : pending) describeDemand(*entry.record);
}

unsigned
MiteService::deliver(InstructionDeliveryQueue &idq, uint64_t cycle,
        InstSeqNum before,
        const std::function<void(const FrontendRecord &, uint64_t)> &observe)
{
    unsigned delivered = 0;
    unsigned sources[4] = {};
    unsigned allocation_sources[4] = {}, delivered_slots = 0;
    unsigned group = 0, slots = 0;
    const unsigned limit = std::min(outputBudget, idq.capacityValue());
    for (auto it = pending.begin(); it != pending.end();) {
        const auto allocation_group = it->record->allocationGroup;
        unsigned records = 0, group_slots = 0;
        auto end = it;
        do {
            if (end->ready > cycle || end->record->sequence >= before) break;
            group_slots += end->record->demand.idqUnits;
            ++records;
            ++end;
        } while (allocation_group && end != pending.end() &&
                 end->record->allocationGroup == allocation_group);
        if (end != pending.end() && allocation_group &&
            end->record->allocationGroup == allocation_group)
            break;
        if (!records || slots + group_slots > limit ||
            (group && group + records > 4 * limit))
            break;
        if (slots + group_slots > idq.freeEntries()) {
            if (slots == idq.freeEntries()) ++stats.backpressureCycles;
            ++stats.groupCreditStalls;
            break;
        }
        slots += group_slots;
        group += records;
        it = end;
    }
    while (delivered < group) {
        const unsigned source = static_cast<unsigned>(pending.front().record->source);
        ++sources[source];
        const unsigned cost = pending.front().record->demand.idqUnits;
        allocation_sources[source] += cost;
        delivered_slots += cost;
        deliveredThrough = pending.front().record->sequence;
        if (observe && pending.front().record->source != FrontendSource::Lsd)
            observe(*pending.front().record, cycle);
        idq.enqueue(std::move(pending.front().record), cycle, cycle);
        pending.pop_front();
        ++delivered;
    }
    stats.deliveredOps += delivered;
    stats.deliveredSlots += delivered_slots;
    if (delivered)
        ++stats.activeCycles;
    if (delivered >= 6)
        ++stats.cyclesGe6;
    stats.delivery.sample(delivered);
    for (unsigned i = 0; i < 4; ++i) {
        stats.sourceOps[i] += sources[i];
        if (sources[i]) ++stats.sourceCycles[i];
        if (sources[i] >= 6) ++stats.sourceCyclesGe6[i];
        if (sources[i]) ++stats.sourceWidths[i][sources[i]];
        stats.sourceSlots[i] += allocation_sources[i];
        if (allocation_sources[i]) ++stats.sourceSlotCycles[i];
        if (allocation_sources[i] >= 6) ++stats.sourceSlotCyclesGe6[i];
        if (allocation_sources[i]) ++stats.sourceSlotWidths[i][allocation_sources[i]];
    }
    checkDrain();
    return delivered;
}

void
MiteService::cancelAfter(InstSeqNum through)
{
    while (!pending.empty() && pending.back().record->sequence > through) {
        if (pending.back().record->allocationGroup)
            pending.back().record->allocationGroup->truncate(through);
        pending.pop_back();
        ++stats.cancelledOps;
    }
    refreshDemands();
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
      ADD_STAT(evictions, statistics::units::Count::get(), "Decoded entry LRU evictions"),
      ADD_STAT(invalidatedTemplates, statistics::units::Count::get(),
               "Templates invalidated by code or translation context changes"),
      ADD_STAT(staleFills, statistics::units::Count::get(),
               "Fills rejected after an intervening invalidation"),
      ADD_STAT(oversizeTemplates, statistics::units::Count::get(),
               "Templates exceeding the configured operation capacity")
{}

DsbTemplates::DsbTemplates(const DsbTemplatesParams &p)
    : MiteService(p), numSets(p.numSets), associativity(p.associativity),
      opsPerEntry(p.opsPerEntry), maxRegionOps(p.maxRegionOps), cacheStats(this)
{
    fatal_if(!numSets || (numSets & (numSets - 1)) || !associativity ||
             !opsPerEntry, "DSB needs nonzero power-of-two sets and geometry");
}

unsigned
DsbTemplates::setIndex(Addr address) const
{
    return (address >> 6) & (numSets - 1);
}

void
DsbTemplates::erase(Entry entry)
{
    for (const auto &member : entry->members)
        index.erase(member.identity);
    entries.erase(entry);
}

const FrontendTemplate *
DsbTemplates::candidate(const FrontendCodeIdentity &identity,
        uint64_t cycle) const
{
    auto found = index.find(identity);
    if (found == index.end()) return nullptr;
    const auto &member = found->second.entry->members[found->second.member];
    return member.available <= cycle ? &member : nullptr;
}

const FrontendTemplate *
DsbTemplates::lookup(const FrontendCodeIdentity &identity,
        const std::vector<Addr> &validated_blocks, uint64_t cycle)
{
    ++cacheStats.lookups;
    auto found = index.find(identity);
    if (found == index.end())
        return nullptr;
    const auto entry = found->second.entry;
    const size_t member_index = found->second.member;
    const auto &member = entry->members[member_index];
    if (member.available > cycle)
        return nullptr;
    if (member.physicalBlocks != validated_blocks) {
        cacheStats.invalidatedTemplates += entry->members.size();
        erase(entry);
        return nullptr;
    }
    entries.splice(entries.begin(), entries, entry);
    ++cacheStats.hits;
    return &entry->members[member_index];
}

unsigned
FrontendTemplate::encodedSlots() const
{
    if (macroop && macroop->usesMicrocodeService())
        return std::max(1U, macroop->microcodePrefixOps());
    // Decode/encoding qualification owns storage. Backend allocation groups
    // are deliberately not consulted: an unlaminated memory form can retain
    // one encoded slot while allocating separate backend entries.
    if (macroop && (macroop->isMicroFusedMemory() || macroop->isStackRegAllocation()))
        return 1;
    switch (nearStackControl(macroop.get())) {
      case NearStackControl::DirectCall:
      case NearStackControl::RegisterCall:
        return 2;
      case NearStackControl::MemoryCall:
        return 3;
      case NearStackControl::Return:
        return 1;
      case NearStackControl::ReturnImmediate:
        return 3;
      case NearStackControl::None:
        break;
    }
    unsigned slots = 0;
    for (const auto &op : operations) {
        const auto divide = op->intDivide();
        if (divide.bytes)
            slots += divide.bytes == 8 ? 5 : 4;
        else
            // Effective fallback for forms not yet given a distinct encoding
            // description; this is not evidence of physical uop count.
            ++slots;
    }
    return slots;
}

bool
DsbTemplates::fill(FrontendTemplate value, uint64_t accepted_epoch,
        uint64_t cycle)
{
    if (accepted_epoch != codeEpoch) {
        ++cacheStats.staleFills;
        return false;
    }
    if ((value.operations.empty() &&
         !(value.macroop && value.macroop->usesMicrocodeService())) ||
        value.physicalBlocks.empty())
        return false;
    if (value.encodedSlots() > opsPerEntry) {
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
        erase(existing->second.entry);
    const auto same_region = [&value](const CacheEntry &entry) {
        const auto &identity = entry.members.front().identity;
        return identity.addressSpace == value.identity.addressSpace &&
            identity.decoderContext == value.identity.decoderContext &&
            (identity.address >> 6) == (value.identity.address >> 6);
    };
    if (maxRegionOps) {
        unsigned region_units = value.encodedSlots();
        for (const auto &entry : entries)
            if (same_region(entry))
                region_units += entry.units;
        if (region_units > maxRegionOps) {
            // A failed region qualification discards its partial templates.
            // The bounded cache itself owns this state; redecoding on the
            // next traversal may qualify a different observed path.
            for (auto it = entries.begin(); it != entries.end();) {
                auto entry = it++;
                if (same_region(*entry))
                    erase(entry);
            }
            ++cacheStats.oversizeTemplates;
            return false;
        }
    }
    // Pack consecutive instructions from the same observed path. A taken
    // branch seals its entry; this is an effective model, not a physical tag.
    const unsigned set = setIndex(value.identity.address);
    const unsigned units = value.encodedSlots();
    auto target = entries.end();
    unsigned occupied = 0;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const auto &first = it->members.front();
        if (setIndex(first.identity.address) != set) continue;
        ++occupied;
        const auto &last = it->members.back();
        if (!it->closed && it->units + units <= opsPerEntry &&
            first.identity.addressSpace == value.identity.addressSpace &&
            first.identity.decoderContext == value.identity.decoderContext &&
            (first.identity.address >> 6) == (value.identity.address >> 6) &&
            last.identity.address + last.bytes == value.identity.address)
            target = it;
    }
    if (target == entries.end()) {
        if (occupied >= associativity) {
            auto victim = entries.end();
            for (auto it = entries.end(); it != entries.begin();) {
                --it;
                if (setIndex(it->members.front().identity.address) == set) {
                    victim = it;
                    break;
                }
            }
            fatal_if(victim == entries.end(), "DSB set has no evictable entry");
            erase(victim);
            ++cacheStats.evictions;
        }
        entries.emplace_front();
        target = entries.begin();
    } else {
        entries.splice(entries.begin(), entries, target);
    }
    value.available = cycle + 1;
    const bool closed = value.endsEntry;
    target->members.push_back(std::move(value));
    target->units += units;
    target->closed = closed;
    const size_t offset = target->members.size() - 1;
    index.emplace(target->members.back().identity, Location{target, offset});
    ++cacheStats.fills;
    return true;
}

void
DsbTemplates::invalidate(Addr physical_block)
{
    for (auto it = entries.begin(); it != entries.end();) {
        auto entry = it++;
        const bool overlaps = std::any_of(entry->members.begin(), entry->members.end(),
            [physical_block](const auto &member) {
                return std::find(member.physicalBlocks.begin(), member.physicalBlocks.end(),
                                 physical_block) != member.physicalBlocks.end();
            });
        if (overlaps) {
            cacheStats.invalidatedTemplates += entry->members.size();
            erase(entry);
        }
    }
}

void
DsbTemplates::invalidateAll()
{
    ++codeEpoch;
    for (const auto &entry : entries)
        cacheStats.invalidatedTemplates += entry.members.size();
    entries.clear();
    index.clear();
}

FrontendCoordinator::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(sourceTransitions, statistics::units::Count::get(),
               "Source changes at actual IDQ handoff"),
      ADD_STAT(switchWaitCycles, statistics::units::Cycle::get(),
               "Observed frontend cycles in a DSB to MITE wait; not a certified hardware penalty"),
      ADD_STAT(switchWaitRenameBlocked, statistics::units::Cycle::get(),
               "Switch-wait cycles overlapping Rename backpressure"),
      ADD_STAT(switchWaitIdqFull, statistics::units::Cycle::get(),
               "Switch-wait cycles overlapping a full IDQ"),
      ADD_STAT(outputs, statistics::units::Count::get(), "IDQ output operations"),
      ADD_STAT(redirects, statistics::units::Count::get(), "Frontend cancellations"),
      ADD_STAT(translationValidations, statistics::units::Count::get(),
               "Native Execute translation completions used to validate code"),
      ADD_STAT(allocationOps, statistics::units::Count::get(),
               "Frontend records actually accepted by Rename"),
      ADD_STAT(allocationSlots, statistics::units::Count::get(),
               "Allocation units accepted by Rename, including fused pairs"),
      ADD_STAT(notDeliveredSlots, statistics::units::Count::get(),
               "Missing IDQ output units within the delayed receiver-capacity candidate"),
      ADD_STAT(allocationCycles, statistics::units::Cycle::get(),
               "Observed active Rename cycles"),
      ADD_STAT(backendAcceptingCycles, statistics::units::Cycle::get(),
               "IDQ output cycles with positive receiver capacity and no backward block"),
      ADD_STAT(backendBlockedCycles, statistics::units::Cycle::get(),
               "IDQ output cycles excluded by actual backward block or zero receiver capacity"),
      ADD_STAT(notDeliveredOps, statistics::units::Count::get(),
               "Missing IDQ output records, clipped to the receiver-capacity candidate"),
      ADD_STAT(zeroDeliveryCycles, statistics::units::Cycle::get(),
               "Receiver-accepting cycles with no actual IDQ output"),
      ADD_STAT(fullDeliveryCycles, statistics::units::Cycle::get(),
               "Receiver-accepting IDQ output cycles delivering the configured full width"),
      ADD_STAT(receiverUnknownCycles, statistics::units::Cycle::get(),
               "IDQ output cycles without a valid delayed receiver observation"),
      ADD_STAT(partialReceiverCycles, statistics::units::Cycle::get(),
               "Receiver-accepting IDQ output cycles with capacity below the full width"),
      ADD_STAT(allocationWidths, statistics::units::Count::get(),
               "Actual Rename acceptance width, independently of IDQ output"),
      ADD_STAT(backendBlocks, statistics::units::Cycle::get(),
               "Exclusion reasons; multiple reasons may overlap"),
      ADD_STAT(sqHeadroomAcceptingCycles, statistics::units::Cycle::get(),
               "Receiver-accepting IDQ output cycles by delayed free SQ entries"),
      ADD_STAT(sqHeadroomMissingSlots, statistics::units::Count::get(),
               "Missing IDQ output units by delayed free SQ entries; correlation only")
{}

FrontendCoordinator::FrontendCoordinator(const FrontendCoordinatorParams &p)
    : SimObject(p), enableLsdReplay(p.enableLsdReplay),
      stats(this),
      instructionBytes(p.instructionBytes), mite(p.mite), dsb(p.dsb),
      ms(p.ms), lsd(p.lsd), idq(p.idq)
{}

void
FrontendCoordinator::configure(unsigned threads, unsigned width, unsigned size)
{
    fatal_if(threads != 1 || !width || !size || (size & (size - 1)),
             "Component frontend requires one thread and valid geometry");
    outputWidth = width;
    lineSize = size;
    stats.allocationWidths.init(MaxWidth + 1);
    for (unsigned i = 0; i <= MaxWidth; ++i)
        stats.allocationWidths.subname(i, std::to_string(i));
    stats.backendBlocks.init(7);
    const char *reasons[] = {"signal", "recovery", "serialize", "rob", "iq", "lsq", "prf"};
    for (unsigned i = 0; i < 7; ++i) stats.backendBlocks.subname(i, reasons[i]);
    stats.sqHeadroomAcceptingCycles.init(outputWidth + 1);
    stats.sqHeadroomMissingSlots.init(outputWidth + 1);
    for (unsigned i = 0; i <= outputWidth; ++i) {
        const std::string label = i == outputWidth ?
            "at_least_" + std::to_string(i) : std::to_string(i);
        stats.sqHeadroomAcceptingCycles.subname(i, label);
        stats.sqHeadroomMissingSlots.subname(i, label);
    }
}

bool FrontendCoordinator::canAccept() const
{
    return !stoppedThisCycle && (mite->freeBatchOps() || dsb->freeBatchOps() ||
                                ms->freeBatchOps() || lsd->freeBatchOps());
}
MiteService *FrontendCoordinator::sourceService(FrontendSource source) const
{
    if (source == FrontendSource::Dsb) return dsb;
    if (source == FrontendSource::Microcode) return ms;
    if (source == FrontendSource::Lsd) return lsd;
    return mite;
}
bool FrontendCoordinator::canAccept(FrontendSource source) const
{
    return !stoppedThisCycle && sourceService(source)->freeBatchOps();
}
void FrontendCoordinator::allocation(unsigned accepted, unsigned slots)
{
    panic_if(slots > outputWidth, "Rename acceptance exceeds allocation width");
    ++stats.allocationCycles;
    stats.allocationOps += accepted;
    stats.allocationSlots += slots;
    ++stats.allocationWidths[accepted];
}
bool FrontendCoordinator::empty() const
{
    return mite->idle() && dsb->idle() && ms->idle() && lsd->idle() &&
        idq->empty() && pendingFills.empty();
}

void
FrontendCoordinator::advance(uint64_t cycle, bool rename_blocked,
        const TimeStruct::RenameComm &receiver,
        const std::function<bool(FrontendRecordPtr)> &emit)
{
    stoppedThisCycle = false;
    unsigned sent = 0, sent_records = 0;
    auto output = [&] {
        while (!rename_blocked && !stoppedThisCycle && sent_records < MaxWidth &&
               idq->headReady(cycle)) {
            if (idq->headSlots() > outputWidth - sent) break;
            if (sent == outputWidth) {
                // A zero-cost follower completes the last accepted group.
                // The IDQ's ordered head determines its remaining cost.
                if (idq->headSlots()) break;
            }
            auto record = idq->send(cycle);
            sent += record->demand.idqUnits;
            if (deliveryObserver && record->source == FrontendSource::Lsd)
                deliveryObserver(*record, cycle);
            ++sent_records;
            ++stats.outputs;
            if (!emit(std::move(record))) stoppedThisCycle = true;
        }
    };
    output();
    if (!stoppedThisCycle) {
        // Sources prepare independently. Only the globally oldest source can
        // hand off this cycle, and it cannot pass another source's head.
        MiteService *oldest = nullptr;
        InstSeqNum before = ~InstSeqNum(0);
        for (auto *service : {mite, static_cast<MiteService *>(dsb),
                              static_cast<MiteService *>(ms), lsd}) {
            if (service->idle()) continue;
            if (!oldest || service->headSequence() < oldest->headSequence()) {
                if (oldest) before = std::min(before, oldest->headSequence());
                oldest = service;
            } else before = std::min(before, service->headSequence());
        }
        if (fusionTail)
            before = std::min(before, fusionTail->allocationGroup ?
                fusionTail->allocationGroup->leader : fusionTail->sequence);
        if (haveSource && selected == FrontendSource::Dsb && oldest == mite &&
            mite->preparingHead(cycle)) {
            ++stats.switchWaitCycles;
            if (rename_blocked) ++stats.switchWaitRenameBlocked;
            if (!idq->freeEntries()) ++stats.switchWaitIdqFull;
        }
        auto observe = [this](const FrontendRecord &record, uint64_t at) {
            if (haveSource && selected != record.source) ++stats.sourceTransitions;
            selected = record.source;
            haveSource = true;
            if (deliveryObserver) deliveryObserver(record, at);
        };
        if (oldest) oldest->deliver(*idq, cycle, before, observe);
        const InstSeqNum delivered = std::max({mite->lastDelivered(),
            dsb->lastDelivered(), ms->lastDelivered(), lsd->lastDelivered()});
        while (!pendingFills.empty() && pendingFills.front().through <= delivered) {
            for (auto &value : pendingFills.front().templates)
                dsb->fill(std::move(value), pendingFills.front().epoch, cycle);
            pendingFills.pop_front();
        }
        output();
    }
    if (!receiver.receiverValid) {
        ++stats.receiverUnknownCycles;
    } else if (rename_blocked || !receiver.receiverCapacity) {
        ++stats.backendBlockedCycles;
        const unsigned reasons = receiver.receiverBlocks |
            (rename_blocked ? unsigned(RenameSignal) : 0U);
        for (unsigned i = 0; i < 7; ++i)
            if (reasons & (1U << i)) ++stats.backendBlocks[i];
    } else {
        ++stats.backendAcceptingCycles;
        const unsigned capacity = std::min(receiver.receiverCapacity, outputWidth);
        const unsigned missing = capacity - std::min(sent, capacity);
        if (missing) {
            // An unfinished decode or the oldest unready source can expose
            // this miss. Ready queued records hiding the delay do not.
            if (decodingMiss && idq->occupied() == 0 &&
                mite->idle() && dsb->idle() && ms->idle())
                *decodingMiss = true;
            MiteService *head = nullptr;
            for (auto *service : {mite, static_cast<MiteService *>(dsb),
                                  static_cast<MiteService *>(ms), lsd}) {
                if (service->idle()) continue;
                if (!head || service->headSequence() < head->headSequence())
                    head = service;
            }
            if (head && head->preparingHead(cycle) &&
                head->headRecord()->dsbMissExposed)
                *head->headRecord()->dsbMissExposed = true;
        }
        stats.notDeliveredSlots += missing;
        stats.notDeliveredOps += capacity - std::min(sent_records, capacity);
        if (!sent) ++stats.zeroDeliveryCycles;
        if (sent == outputWidth) ++stats.fullDeliveryCycles;
        if (capacity < outputWidth) ++stats.partialReceiverCycles;
        const unsigned sq_bin = std::min(receiver.freeSQEntries, outputWidth);
        ++stats.sqHeadroomAcceptingCycles[sq_bin];
        stats.sqHeadroomMissingSlots[sq_bin] += missing;
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
    fuseAcrossBatches(records);
    for (auto &record : records) {
        describeDemand(*record);
        observeLoop(*record);
    }
    if (!fills.empty())
        pendingFills.push_back({records.back()->sequence, dsb->epoch(), std::move(fills)});
    auto *service = sourceService(source);
    service->submit(std::move(records), instructions, cycle);
    decodingMiss.reset();
}

void FrontendCoordinator::accepted(InstSeqNum sequence)
{
    idq->accepted(sequence);
    checkDrain();
}
void FrontendCoordinator::cancelAfter(InstSeqNum through)
{
    decodingMiss.reset();
    fusionTail = nullptr;
    mite->cancelAfter(through);
    dsb->cancelAfter(through);
    ms->cancelAfter(through);
    lsd->cancelAfter(through);
    idq->cancelAfter(through);
    pendingFills.clear();
    clearLoop();
    checkDrain();
}
void FrontendCoordinator::reset()
{
    ++translationEpochValue;
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
    // Predicted translations must not evict the current decode's lease
    // before its resident bytes can be turned into a DSB template.
    while (leases.size() > instructionBytes->residentCapacity())
        leases.pop_front();
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
    fusionTail = nullptr;
    clearLoop();
    dsb->invalidate(block);
    // Physical invalidation revokes only templates using this line. A
    // global epoch change here would reject unrelated pending code fills
    // whenever the snooping instruction port observes a data eviction.
    for (auto it = pendingFills.begin(); it != pendingFills.end();) {
        auto &templates = it->templates;
        templates.erase(std::remove_if(templates.begin(), templates.end(),
            [block](const FrontendTemplate &value) {
                return std::find(value.physicalBlocks.begin(),
                    value.physicalBlocks.end(), block) !=
                    value.physicalBlocks.end();
            }), templates.end());
        if (templates.empty()) it = pendingFills.erase(it);
        else ++it;
    }
    leases.erase(std::remove_if(leases.begin(), leases.end(),
        [&](const CodeLease &lease) { return lease.physicalBlock == block; }), leases.end());
    if (instructionBytes->matchesPhysicalBlock(block, lineSize))
        instructionBytes->invalidate();
}

bool
InstructionDeliveryQueue::markCodeWrite(ThreadID tid, InstSeqNum after,
                                         Addr block)
{
    bool matched = false;
    for (auto &entry : records)
        matched |= entry.record->markCodeWrite(tid, after, block);
    return matched;
}

bool
MiteService::markCodeWrite(ThreadID tid, InstSeqNum after, Addr block)
{
    bool matched = false;
    for (auto &entry : pending)
        matched |= entry.record->markCodeWrite(tid, after, block);
    return matched;
}

bool
FrontendCoordinator::markCodeWrite(ThreadID tid, InstSeqNum after, Addr block)
{
    bool matched = idq->markCodeWrite(tid, after, block);
    matched |= mite->markCodeWrite(tid, after, block);
    matched |= dsb->markCodeWrite(tid, after, block);
    matched |= ms->markCodeWrite(tid, after, block);
    matched |= lsd->markCodeWrite(tid, after, block);
    return matched;
}
void FrontendCoordinator::invalidateTranslations()
{
    ++translationEpochValue;
    fusionTail = nullptr;
    clearLoop();
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
    // No successor is required once fetch has stopped for drain.
    fusionTail = nullptr;
    return empty() ? DrainState::Drained : DrainState::Draining;
}

bool
FrontendCoordinator::fusionEligibleProducer(const StaticInst &inst)
{
    if (inst.intExecution().family == IntExecutionFamily::ALU) return true;
    const auto *x86 = dynamic_cast<const X86ISA::X86StaticInst *>(&inst);
    return x86 && x86->isByteTestRegisterFusionCandidate();
}

void
FrontendCoordinator::fuseAcrossBatches(std::vector<FrontendRecordPtr> &records)
{
    if (records.empty()) return;
    auto &next = *records.front();
    if (fusionTail && fusionTail->source == next.source &&
        fusionTail->source != FrontendSource::Microcode &&
        fusionTail->thread == next.thread &&
        fusionTail->addressSpace == next.addressSpace &&
        fusionTail->fromUser == next.fromUser &&
        !fusionTail->prediction.taken &&
        fusionTail->predictedPC->instAddr() == next.pc->instAddr() &&
        fusionTail->pc->instAddr() / 64 == next.pc->instAddr() / 64 &&
        (!next.instruction->isMicroop() || next.instruction->isLastMicroop()) &&
        fusionTail->instruction->canMacroFuseWith(*next.instruction)) {
        auto group = fusionTail->allocationGroup;
        if (!group) {
            group = std::make_shared<AllocationGroup>(fusionTail->sequence,
                next.sequence, fusionTail->sequence, next.sequence);
        }
        group->branchProducer = fusionTail->sequence;
        group->branch = group->end = next.sequence;
        fusionTail->allocationGroup = next.allocationGroup = group;
        for (auto *s : {mite, static_cast<MiteService *>(dsb),
                       static_cast<MiteService *>(ms), lsd}) s->refreshDemands();
    }
    fusionTail = nullptr;
    auto &last = *records.back();
    // Keep one flag-producing instruction as decode lookahead. This avoids
    // making a software preparation-batch boundary a hardware fusion limit.
    if (last.source != FrontendSource::Microcode && !last.prediction.taken &&
        !last.instruction->isControl() &&
        last.instruction->numDestRegs(CCRegClass) &&
        fusionEligibleProducer(*last.instruction) &&
        (!last.instruction->isMicroop() || last.instruction->isLastMicroop()) &&
        (!last.allocationGroup || !last.allocationGroup->branch))
        fusionTail = &last;
}

void FrontendCoordinator::clearLoop()
{
    loopCapture.clear();
    loopTemplates.clear();
    loopBuilding.reset();
    loopCaptureUnits = 0;
}

const FrontendTemplate *
FrontendCoordinator::loopCandidate(Addr address) const
{
    if (!enableLsdReplay) return nullptr;
    for (const auto &entry : loopTemplates)
        if (entry.identity.address == address &&
            entry.identity.addressSpace == addressSpace &&
            entry.identity.decoderContext == decoderContext)
            return &entry;
    return nullptr;
}

void FrontendCoordinator::observeLoop(const FrontendRecord &record)
{
    if (!enableLsdReplay) return;
    const auto &op = record.instruction;
    if (record.source == FrontendSource::Lsd) {
        if (op->isControl() && !record.prediction.taken) clearLoop();
        return;
    }
    if (record.source == FrontendSource::Microcode || record.fault != NoFault ||
        op->isCall() || op->isReturn() || op->isIndirectCtrl() ||
        op->isSerializeBefore() || op->isSerializeAfter() || op->isQuiesce() ||
        op->isMicrocodeControl()) {
        clearLoop();
        return;
    }
    if (!loopBuilding) {
        std::vector<Addr> blocks;
        const unsigned bytes = record.pc->as<X86ISA::PCState>().size();
        if (!physicalBlocks(record.pc->instAddr(), bytes, blocks)) {
            clearLoop();
            return;
        }
        loopBuilding = FrontendTemplate{identity(record.pc->instAddr()),
            record.macroop ? record.macroop : op, {}, bytes,
            std::move(blocks), 0};
    }
    loopBuilding->operations.push_back(op);
    loopCaptureUnits += record.demand.idqUnits;
    if (loopCaptureUnits > idq->capacityValue()) {
        clearLoop();
        return;
    }
    if (op->isMicroop() && !op->isLastMicroop()) return;
    loopCapture.push_back(std::move(*loopBuilding));
    loopBuilding.reset();
    if (!op->isControl()) return;
    const Addr target = record.predictedPC->instAddr();
    const auto begin = std::find_if(loopCapture.begin(), loopCapture.end(),
        [target](const auto &entry) { return entry.identity.address == target; });
    // Bounded simple-loop candidate: one direct taken backedge, no internal
    // control transfer. Eligibility is deliberately not inferred from size alone.
    if (record.prediction.taken && target <= record.pc->instAddr() &&
        begin != loopCapture.end())
        loopTemplates.assign(begin, loopCapture.end());
    else loopTemplates.clear();
    loopCapture.clear();
    loopCaptureUnits = 0;
}
}
