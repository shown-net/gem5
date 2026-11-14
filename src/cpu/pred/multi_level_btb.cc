/*
 * Copyright (c) 2026 The University of Edinburgh
 * Copyright (c) 2026 Technical University of Munich
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/pred/multi_level_btb.hh"

#include <algorithm>
#include <optional>
#include <typeinfo>

#include "base/cprintf.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"

namespace gem5::branch_prediction
{

BTBLevel::BTBLevel(const BTBLevelParams &p)
    : SimObject(p),
      btb(name() + ".btb", p.numEntries, p.associativity, p.replPolicy,
          p.indexingPolicy, BTBEntry(genTagExtractor(p.indexingPolicy))),
      latency(p.latency),
      inclusive(p.inclusive)
{
    auto *policy = dynamic_cast<BTBSetAssociative *>(p.indexingPolicy);
    exactIndex = policy && typeid(*policy) == typeid(BTBSetAssociative) &&
        policy->exactAddressTags();
    fatal_if(!isPowerOf2(p.numEntries / p.associativity),
             "Number of BTB sets must be a power of two");
}

void
BTBLevel::indexInsert(ThreadID tid, Addr pc)
{
    if (exactIndex) residentMask[{tid, pc >> 6}] |= uint64_t(1) << (pc & 63);
    identities[{tid, pc}] = nextIdentity++;
}

void
BTBLevel::indexErase(ThreadID tid, Addr pc)
{
    identities.erase({tid, pc});
    if (!exactIndex) return;
    auto found = residentMask.find({tid, pc >> 6});
    if (found == residentMask.end()) return;
    found->second &= ~(uint64_t(1) << (pc & 63));
    if (!found->second) residentMask.erase(found);
}

BTBEntry *
BTBLevel::lookup(ThreadID tid, Addr instPC)
{
    auto *entry = btb.accessEntry({instPC, tid});
    if (entry && !inclusive) {
        indexErase(tid, entry->getBranchPC());
        btb.invalidate(entry);
    }

    return entry;
}

BTBEntry *
BTBLevel::multiLookup(ThreadID tid, Addr instPC, Cycles &hit_latency,
                      unsigned &hit_level)
{
    auto *entry = lookup(tid, instPC);
    if (entry) {
        hit_latency = latency;
        hit_level = level;
        return entry;
    }

    if (!nextLevel) {
        return nullptr;
    }

    entry = nextLevel->multiLookup(tid, instPC, hit_latency, hit_level);
    if (entry && inclusive) {
        // Refilling into this level can cascade an eviction and overwrite
        // the entry in the hit level. Preserve it before starting the refill.
        BTBEntry hit_copy(*entry);
        entry = insertEntry(tid, instPC, *hit_copy.target, hit_copy.inst);
    }

    return entry;
}

BTBEntry *
BTBLevel::insertEntry(ThreadID tid, Addr instPC, const PCStateBase &target,
                      StaticInstPtr inst, std::optional<BTBEntry> *evicted)
{
    BTBEntry *entry = btb.findEntry({instPC, tid});
    if (entry) {
        if (entry->getBranchPC() != instPC)
            indexErase(entry->getTag().tid, entry->getBranchPC());
        btb.accessEntry(entry);
    } else {
        std::optional<BTBEntry> victim;
        entry = btb.findVictim({instPC, tid}, [&victim](const BTBEntry &v) {
            victim.emplace(v);
        });
        if (evicted && victim && victim->isValid()) *evicted = *victim;
        if (victim && victim->isValid())
            indexErase(victim->getTag().tid, victim->getBranchPC());
        btb.insertEntry({instPC, tid}, entry);

        if (nextLevel) {
            nextLevel->doWriteback(tid, *victim);
        }
    }

    entry->update(target, inst);
    entry->setBranchPC(instPC);
    indexInsert(tid, instPC);
    return entry;
}

void
BTBLevel::doWriteback(ThreadID tid, const BTBEntry &upper_victim)
{
    // An inclusive level already contains entries inserted in its upper
    // level. Only a victim-buffer level needs to accept the writeback.
    // Also, ignore writeback if the target address is null - invalid entry.
    if (inclusive || !upper_victim.isValid() || !upper_victim.target) {
        return;
    }

    insertEntry(tid, upper_victim.getBranchPC(), *upper_victim.target,
                upper_victim.inst);
}

MultiLevelBTB::MultiLevelBTBStats::MultiLevelBTBStats(
    statistics::Group *parent, const std::vector<BTBLevel *> &levels)
    : statistics::Group(parent),
      ADD_STAT(levelHits, statistics::units::Count::get(),
               "Number of BTB hits at each level")
{
    using namespace statistics;

    levelHits.init(levels.size()).flags(total);

    for (unsigned i = 0; i < levels.size(); ++i) {
        fatal_if(!levels[i], "BTB level %d is null", i + 1);
        levelHits.subname(i, csprintf("L%d", i + 1));
    }
}

MultiLevelBTB::MultiLevelBTB(const MultiLevelBTBParams &p)
    : BranchTargetBuffer(p), levels(p.levels), multilevelstats(this, levels)
{
    fatal_if(levels.empty(), "%s must contain at least one BTB level", name());
    DPRINTF(BTB, "%s: creating a %d-level BTB hierarchy\n", name(),
            levels.size());

    for (unsigned level = 0; level < levels.size(); ++level) {
        levels[level]->level = level;
        if (level + 1 < levels.size()) {
            levels[level]->nextLevel = levels[level + 1];
        }
    }

    fatal_if(!levels.front()->inclusive, "%s: L1 BTB must be inclusive",
             name());
    for (auto *level : levels)
        fatal_if(!level->inclusive,
                 "%s: BTB levels are replicas of the main table, not victims",
                 name());
}

void
MultiLevelBTB::memInvalidate()
{
    ++contextEpoch;
    invalidationEpochs.clear();
    for (auto *level : levels) {
        level->btb.clear();
        level->residentMask.clear();
        level->identities.clear();
    }
}

bool
MultiLevelBTB::valid(ThreadID tid, Addr instPC)
{
    unsigned level_num = 1;
    for (auto *level : levels) {
        if (level->btb.findEntry({instPC, tid})) {
            DPRINTF(BTB, "L%d BTB valid for PC %#x\n", level_num, instPC);
            return true;
        }
        ++level_num;
    }

    return false;
}

void
MultiLevelBTB::invalidate(ThreadID tid, Addr instPC)
{
    ++invalidationEpochs[{tid, instPC}];
    for (auto *level : levels) {
        if (auto *entry = level->btb.findEntry({instPC, tid})) {
            if (entry->getBranchPC() != instPC)
                ++invalidationEpochs[{tid, entry->getBranchPC()}];
            level->indexErase(tid, entry->getBranchPC());
            level->btb.invalidate(entry);
        }
    }
}

Cycles
MultiLevelBTB::lookupLatency(ThreadID tid, Addr instPC) const
{
    for (const auto *level : levels) {
        if (level->btb.findEntry({instPC, tid}))
            return level->latency;
    }
    return Cycles(0);
}

bool
MultiLevelBTB::rangeCandidates(ThreadID tid, Addr start, unsigned width,
        unsigned stride, std::optional<Addr> &early,
        std::optional<Addr> &late) const
{
    if (stride != 1 || std::any_of(levels.begin(), levels.end(),
            [](const auto *level) { return !level->exactIndex; }))
        return false;
    early.reset();
    late.reset();
    uint64_t remaining = uint64_t(width) + 1;
    Addr pc = start;
    while (remaining) {
        const unsigned offset = pc & 63;
        const unsigned count = std::min<uint64_t>(64 - offset, remaining);
        const uint64_t lower = ~uint64_t(0) << offset;
        const uint64_t upper = count == 64 - offset ? ~uint64_t(0) :
            ((uint64_t(1) << (offset + count)) - 1);
        uint64_t masks = 0, first = 0;
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto found = levels[i]->residentMask.find({tid, pc >> 6});
            if (found == levels[i]->residentMask.end()) continue;
            const uint64_t bits = found->second & lower & upper;
            masks |= bits;
            if (!i) first = bits;
        }
        while (masks) {
            const unsigned bit = __builtin_ctzll(masks);
            const Addr candidate = (pc & ~Addr(63)) | bit;
            if (first & (uint64_t(1) << bit)) {
                early = candidate;
                return true;
            }
            if (!late) {
                for (const auto *level : levels) {
                    const auto *entry = level->btb.findEntry({candidate, tid});
                    if (entry) {
                        if (entry->inst) late = candidate;
                        break;
                    }
                }
            }
            masks &= masks - 1;
        }
        pc += count;
        remaining -= count;
    }
    return true;
}

bool
MultiLevelBTB::earlyValid(ThreadID tid, Addr instPC)
{
    return levels.front()->btb.findEntry({instPC, tid}) != nullptr;
}

std::optional<BranchTargetBuffer::Snapshot>
MultiLevelBTB::probe(ThreadID tid, Addr pc) const
{
    for (size_t i = 0; i < levels.size(); ++i) {
        const auto *level = levels[i];
        const auto *entry = level->btb.findEntry({pc, tid});
        if (!entry || !entry->target) continue;
        const auto id = level->identities.find({tid, entry->getBranchPC()});
        if (id == level->identities.end()) return std::nullopt;
        const Addr resident = entry->getBranchPC();
        auto invalidation = invalidationEpochs.find({tid, resident});
        const uint64_t epoch = invalidation == invalidationEpochs.end() ?
            0 : invalidation->second;
        auto query_invalidation = invalidationEpochs.find({tid, pc});
        const uint64_t query_epoch = query_invalidation == invalidationEpochs.end() ?
            0 : query_invalidation->second;
        return Snapshot{tid, pc, resident, unsigned(i), id->second, contextEpoch, epoch,
                        query_epoch, level->latency,
                        entry->inst,
                        std::unique_ptr<PCStateBase>(entry->target->clone())};
    }
    return std::nullopt;
}

bool
MultiLevelBTB::snapshotValid(const Snapshot &snapshot) const
{
    auto found = invalidationEpochs.find({snapshot.tid, snapshot.residentPC});
    const uint64_t epoch = found == invalidationEpochs.end() ? 0 : found->second;
    found = invalidationEpochs.find({snapshot.tid, snapshot.pc});
    const uint64_t query_epoch = found == invalidationEpochs.end() ? 0 : found->second;
    return snapshot.contextEpoch == contextEpoch &&
        snapshot.invalidationEpoch == epoch &&
        snapshot.queryInvalidationEpoch == query_epoch;
}

const PCStateBase *
MultiLevelBTB::consumeSnapshot(const Snapshot &snapshot, BranchType type)
{
    ++stats.lookups[type];
    ++multilevelstats.levelHits[snapshot.level];
    auto *level = levels.at(snapshot.level);
    auto id = level->identities.find({snapshot.tid, snapshot.residentPC});
    auto *entry = level->btb.findEntry({snapshot.pc, snapshot.tid});
    if (id != level->identities.end() && id->second == snapshot.identity && entry &&
        entry->getBranchPC() == snapshot.residentPC) {
        level->btb.accessEntry(entry);
        for (size_t i = 0; i < snapshot.level; ++i)
            levels[i]->insertEntry(snapshot.tid, snapshot.pc,
                                   *snapshot.target, snapshot.inst);
    }
    return snapshot.target.get();
}

const PCStateBase *
MultiLevelBTB::lookup(ThreadID tid, Addr instPC, BranchType type)
{
    stats.lookups[type]++;

    Cycles hit_latency(0);
    unsigned hit_level = 0;
    auto *entry =
        levels.front()->multiLookup(tid, instPC, hit_latency, hit_level);
    if (entry) {
        multilevelstats.levelHits[hit_level]++;
        DPRINTF(BTB, "L%d BTB hit for PC %#x, latency=%d cycles\n",
                hit_level + 1, instPC, hit_latency);
        return entry->target.get();
    }

    // Miss all levels
    // Only the processor with coupled front-end can get here.
    stats.misses[type]++;
    DPRINTF(BTB, "BTB miss for PC %#x\n", instPC);
    return nullptr;
}

const StaticInstPtr
MultiLevelBTB::getInst(ThreadID tid, Addr instPC)
{
    for (auto *level : levels) {
        if (auto *entry = level->btb.findEntry({instPC, tid})) {
            return entry->inst;
        }
    }

    return nullptr;
}

void
MultiLevelBTB::update(ThreadID tid, Addr instPC, const PCStateBase &target,
                      BranchType type, StaticInstPtr inst)
{
    stats.updates[type]++;

    // The 12K main table owns the target population. Faster levels are only
    // replicas: main-table eviction invalidates every faster copy.
    std::optional<BTBEntry> victim;
    levels.back()->insertEntry(tid, instPC, target, inst, &victim);
    if (victim) {
        const auto key = victim->getTag();
        for (size_t i = 0; i + 1 < levels.size(); ++i)
            if (auto *entry = levels[i]->btb.findEntry(
                    {victim->getBranchPC(), key.tid})) {
                levels[i]->indexErase(key.tid, entry->getBranchPC());
                levels[i]->btb.invalidate(entry);
            }
    }
    for (size_t i = 0; i + 1 < levels.size(); ++i)
        levels[i]->insertEntry(tid, instPC, target, inst);
}
} // namespace gem5::branch_prediction

