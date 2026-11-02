// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/pred/path_predictors.hh"

#include <algorithm>
#include <cstdlib>

namespace gem5::branch_prediction
{
namespace
{
constexpr std::array<unsigned, 3> directionLengths{{5, 44, 388}};
constexpr std::array<unsigned, 3> directionTags{{9, 9, 10}};
constexpr std::array<unsigned, 3> indirectLengths{{68, 132, 388}};

unsigned bit(Addr value, unsigned n) { return (value >> n) & 1; }

void signedUpdate(int8_t &counter, bool up, int minimum, int maximum)
{
    counter = std::clamp<int>(counter + (up ? 1 : -1), minimum, maximum);
}
}

BranchPathHistory::BranchPathHistory(const BranchPathHistoryParams &p)
    : SimObject(p), history(p.numThreads), prior(p.numThreads)
{}

void
BranchPathHistory::restore(ThreadID tid, const Bits &bits)
{
    history.at(tid) = bits;
    prior.at(tid) = bits;
}

void
BranchPathHistory::advance(ThreadID tid, Addr branch, Addr target, bool taken)
{
    auto &state = history.at(tid);
    prior.at(tid) = state;
    if (!taken) return;
    unsigned footprint = 0;
    const auto put = [&](unsigned position, unsigned value) {
        footprint |= (value & 1) << position;
    };
    put(0, bit(branch, 3) ^ bit(target, 0));
    put(1, bit(branch, 4) ^ bit(target, 1));
    for (unsigned i = 2; i <= 7; ++i) put(i, bit(branch, i + 3));
    for (unsigned i = 8; i <= 10; ++i)
        put(i, bit(branch, i - 8) ^ bit(target, i - 6));
    put(11, bit(branch, 11) ^ bit(target, 5));
    for (unsigned i = 12; i <= 15; ++i) put(i, bit(branch, i));
    state <<= 2;
    state ^= Bits(footprint);
}

PathTAGE::PathTAGE(const PathTAGEParams &p)
    : ConditionalPredictor(p), path(p.pathHistory)
{
    fatal_if(!path, "PathTAGE requires a path history");
    base.fill(1);
}

unsigned
PathTAGE::fold(const BranchPathHistory::Bits &bits,
               unsigned length, unsigned width)
{
    unsigned result = 0;
    for (unsigned i = 0; i < length; ++i)
        if (bits[i]) result ^= 1U << (i % width);
    return result;
}

bool
PathTAGE::lookup(ThreadID tid, Addr pc, void *&bp_history)
{
    assert(!bp_history);
    auto *h = new History;
    h->before = path->read(tid);
    h->lookedUp = true;
    bool alternate = base[pc & 8191] >= 2;
    for (unsigned table = 0; table < tables.size(); ++table) {
        h->index[table] = (bit(pc, 5) << 8) |
            fold(h->before, directionLengths[table], 8);
        const unsigned width = directionTags[table];
        h->tag[table] = (pc ^ fold(h->before, directionLengths[table], width) ^
            (fold(h->before, directionLengths[table], width - 1) << 1)) &
            ((1U << width) - 1);
        auto &set = tables[table][h->index[table]];
        for (unsigned way = 0; way < set.size(); ++way) {
            auto &entry = set[way];
            if (!entry.valid || entry.tag != h->tag[table]) continue;
            entry.lastUse = ++serial;
            h->way[table] = way;
            if (h->provider >= 0) {
                h->alternate = h->provider;
                alternate = h->longest;
            }
            h->provider = table;
            h->longest = entry.counter >= 0;
            break;
        }
    }
    h->alternatePrediction = alternate;
    h->predicted = h->provider < 0 ? alternate : h->longest;
    if (h->provider >= 0) {
        const auto &entry = tables[h->provider][h->index[h->provider]]
                                  [h->way[h->provider]];
        if (std::abs(2 * entry.counter + 1) <= 1 && useAlternate >= 0)
            h->predicted = alternate;
    }
    bp_history = h;
    return h->predicted;
}

void
PathTAGE::updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                          Addr target, const StaticInstPtr &inst, void *&history)
{
    if (!history) {
        auto *h = new History;
        h->before = path->read(tid);
        history = h;
    }
    path->advance(tid, pc, target, taken);
}

void
PathTAGE::branchPlaceholder(ThreadID tid, Addr pc, bool uncond, void *&history)
{
    assert(!history);
    auto *h = new History;
    h->before = path->read(tid);
    h->placeholder = true;
    history = h;
}

void
PathTAGE::squash(ThreadID tid, void *&history)
{
    if (!history) return;
    auto *h = static_cast<History *>(history);
    if (!h->placeholder) path->restore(tid, h->before);
    delete h;
    history = nullptr;
}

void
PathTAGE::train(Addr pc, bool taken, const History &h)
{
    auto updateBase = [&] {
        auto &counter = base[pc & 8191];
        if (taken && counter < 3) ++counter;
        else if (!taken && counter > 0) --counter;
    };
    auto entryAt = [&](int table) -> Entry * {
        if (table < 0 || h.way[table] < 0) return nullptr;
        auto &entry = tables[table][h.index[table]][h.way[table]];
        return entry.valid && entry.tag == h.tag[table] ? &entry : nullptr;
    };
    auto *provider = entryAt(h.provider);
    auto *alternate = entryAt(h.alternate);
    bool allocate = h.predicted != taken && h.provider < 2;
    if (provider) {
        const bool weak = std::abs(2 * provider->counter + 1) <= 1;
        if (weak) {
            if (h.longest == taken) allocate = false;
            if (h.longest != h.alternatePrediction)
                signedUpdate(useAlternate, h.alternatePrediction == taken, -8, 7);
        }
        signedUpdate(provider->counter, taken, -4, 3);
        if (!provider->usefulness) {
            if (alternate) signedUpdate(alternate->counter, taken, -4, 3);
            else updateBase();
        }
        if (h.longest != h.alternatePrediction) {
            if (h.longest == taken && provider->usefulness < 3)
                ++provider->usefulness;
            else if (h.longest != taken && provider->usefulness > 0)
                --provider->usefulness;
        }
    } else {
        updateBase();
    }
    if (allocate) {
        for (int table = h.provider + 1; table < 3; ++table) {
            auto &set = tables[table][h.index[table]];
            auto victim = std::min_element(set.begin(), set.end(),
                [](const Entry &a, const Entry &b) {
                    if (a.valid != b.valid) return !a.valid;
                    if (a.usefulness != b.usefulness)
                        return a.usefulness < b.usefulness;
                    return a.lastUse < b.lastUse;
                });
            victim->valid = true;
            victim->tag = h.tag[table];
            victim->counter = taken ? 0 : -1;
            victim->usefulness = 0;
            victim->lastUse = ++serial;
            break;
        }
    }
    if (!(++updates & ((1ULL << 18) - 1))) {
        for (auto &table : tables)
            for (auto &set : table)
                for (auto &entry : set) entry.usefulness >>= 1;
    }
}

void
PathTAGE::update(ThreadID tid, Addr pc, bool taken, void *&history,
                 bool squashed, const StaticInstPtr &inst, Addr target)
{
    if (!history) return;
    auto *h = static_cast<History *>(history);
    if (squashed) {
        if (h->placeholder) {
            h->before = path->read(tid);
            h->placeholder = false;
        } else {
            path->restore(tid, h->before);
        }
        path->advance(tid, pc, target, taken);
        return;
    }
    if (h->lookedUp) train(pc, taken, *h);
    delete h;
    history = nullptr;
}

PathIndirectPredictor::PathIndirectPredictor(const PathIndirectPredictorParams &p)
    : IndirectPredictor(p), path(p.pathHistory), userMode(p.numThreads, false)
{
    fatal_if(!path, "PathIndirectPredictor requires a path history");
}

void
PathIndirectPredictor::reset()
{
    for (auto &table : tables)
        for (auto &set : table)
            for (auto &entry : set) {
                entry.valid = false;
                entry.target.reset();
            }
    serial = 0;
}

void
PathIndirectPredictor::setUserMode(ThreadID tid, bool user)
{
    userMode.at(tid) = user;
}

unsigned
PathIndirectPredictor::index(const BranchPathHistory::Bits &bits, unsigned length)
{
    unsigned even = 0, odd = 0;
    for (unsigned i = 16; i < length; ++i) {
        if (!bits[i]) continue;
        if (i & 1) odd ^= 1U << (((i - 17) / 2) % 9);
        else even ^= 1U << (((i - 16) / 2) % 9);
    }
    const unsigned rotated = ((odd << 5) | (odd >> 4)) & 511;
    return even ^ rotated;
}

unsigned
PathIndirectPredictor::tag(const BranchPathHistory::Bits &bits,
                           unsigned length, Addr pc)
{
    unsigned even = 0, odd = 0;
    for (unsigned i = 16; i < length; ++i) {
        if (!bits[i]) continue;
        if (i & 1) odd ^= 1U << (((i - 17) / 2) % 11);
        else even ^= 1U << (((i - 16) / 2) % 11);
    }
    // Figure 9 maps output bit 0 to odd bit 5.
    unsigned result = even ^ (((odd >> 5) | (odd << 6)) & 2047);
    const int pcBits[10] = {10, 12, 14, 6, 8, 9, 11, 13, 15, 7};
    const int lowEven[10] = {4, 6, 8, 0, 2, 12, 14, -1, -1, 10};
    const int lowOdd[10] = {15, -1, -1, 11, 13, 3, 5, 7, 9, 1};
    for (unsigned out = 0; out < 10; ++out) {
        unsigned value = bit(pc, pcBits[out]);
        if (lowEven[out] >= 0) value ^= bits[lowEven[out]];
        if (lowOdd[out] >= 0) value ^= bits[lowOdd[out]];
        result ^= value << out;
    }
    return result;
}

const PCStateBase *
PathIndirectPredictor::lookup(ThreadID tid, InstSeqNum sn, Addr pc,
                              void *&i_history)
{
    assert(!i_history);
    auto *h = new History;
    h->user = userMode.at(tid);
    h->lookedUp = true;
    for (unsigned table = 0; table < 3; ++table) {
        h->index[table] = index(path->read(tid), indirectLengths[table]);
        h->tag[table] = tag(path->read(tid), indirectLengths[table], pc);
        auto &set = tables[table][h->index[table]];
        for (unsigned way = 0; way < set.size(); ++way) {
            auto &entry = set[way];
            if (entry.valid && entry.user == h->user &&
                entry.tag == h->tag[table]) {
                h->provider = table;
                h->way = way;
                entry.lastUse = ++serial;
                break;
            }
        }
    }
    i_history = h;
    if (h->provider < 0) return nullptr;
    return tables[h->provider][h->index[h->provider]][h->way].target->clone();
}

void
PathIndirectPredictor::allocate(unsigned table, const History &h,
                                const PCStateBase &target)
{
    auto &candidates = tables[table][h.index[table]];
    auto victim = std::min_element(candidates.begin(), candidates.end(),
        [](const Entry &a, const Entry &b) {
            if (a.valid != b.valid) return !a.valid;
            return a.lastUse < b.lastUse;
        });
    victim->valid = true;
    victim->user = h.user;
    victim->tag = h.tag[table];
    victim->lastUse = ++serial;
    set(victim->target, target);
}

void
PathIndirectPredictor::update(ThreadID tid, InstSeqNum sn, Addr pc, bool squashed,
        bool taken, const PCStateBase &target, BranchType type, void *&i_history)
{
    if (!squashed || !taken || !i_history) return;
    auto *h = static_cast<History *>(i_history);
    if (!h->lookedUp) return;
    if (h->provider >= 0) {
        auto &entry = tables[h->provider][h->index[h->provider]][h->way];
        if (entry.valid && entry.user == h->user &&
            entry.tag == h->tag[h->provider]) {
            set(entry.target, target);
            entry.lastUse = ++serial;
        }
    }
    const int next = h->provider + 1;
    if (next < 3) allocate(next, *h, target);
}

void
PathIndirectPredictor::squash(ThreadID tid, InstSeqNum sn, void *&history)
{
    delete static_cast<History *>(history);
    history = nullptr;
}

void
PathIndirectPredictor::commit(ThreadID tid, InstSeqNum sn, void *&history)
{
    delete static_cast<History *>(history);
    history = nullptr;
}
}
