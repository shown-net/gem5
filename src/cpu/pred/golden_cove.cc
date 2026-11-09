// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/pred/golden_cove.hh"
#include <algorithm>
#include "base/logging.hh"

namespace gem5::branch_prediction
{
namespace {
unsigned bit(Addr value, unsigned n) { return (value >> n) & 1; }
void train(uint8_t &counter, bool taken)
{
    counter = taken ? std::min<unsigned>(3, counter + 1) :
                      (counter ? counter - 1 : 0);
}
unsigned rotate(unsigned value, unsigned count, unsigned width)
{
    return ((value << count) | (value >> (width - count))) & ((1U << width) - 1);
}
unsigned foldParity(const GoldenPath &h, unsigned length,
                    unsigned parity, unsigned width)
{
    unsigned folded = 0;
    for (unsigned n = 16 + parity; n < length; n += 2)
        folded ^= unsigned(h[n]) << (((n - 16 - parity) / 2) % width);
    return folded;
}
}

void updateGoldenPath(GoldenPath &h, Addr pc, Addr target)
{
    unsigned footprint = ((pc >> 3) & 0xff) | ((pc & 7) << 8) |
                         (pc & 0xf800);
    footprint ^= bit(target, 0) | (bit(target, 1) << 1) |
        (bit(target, 2) << 8) | (bit(target, 3) << 9) |
        (bit(target, 4) << 10) | (bit(target, 5) << 11);
    h <<= 2;
    h ^= GoldenPath(footprint);
}

GoldenCoveBP::GoldenCoveBP(const GoldenCoveBPParams &p)
    : ConditionalPredictor(p), paths(p.numThreads)
{
    fatal_if(p.historyLengths.size() != 3 || p.historyLengths.back() != 388,
             "Golden Cove proxy requires three histories ending at 388 bits");
    for (unsigned i = 0; i < 3; ++i) {
        lengths[i] = p.historyLengths[i];
        fatal_if(!lengths[i] || lengths[i] > 388 ||
                 (i && lengths[i] <= lengths[i-1]), "Invalid PHT history spans");
    }
    base.fill(1);
}

unsigned GoldenCoveBP::index(const GoldenPath &h, Addr pc, unsigned bank) const
{
    // Half&Half p9: the PC[5] partition is measured on Alder Lake.
    // The published exact 22/58/186-bit folding example is Skylake. Applying
    // the same parity-fold family to the longer GLC history is a model choice.
    unsigned folded = 0;
    if (!bank) {
        for (unsigned b = 0; b < 8; ++b) {
            if (6 + 2*b < lengths[bank]) folded ^= unsigned(h[6 + 2*b]) << b;
            if (1 + 2*b < lengths[bank]) folded ^= unsigned(h[1 + 2*b]) << b;
        }
    } else {
        for (unsigned n = 10; n < lengths[bank]; n += 2)
            folded ^= unsigned(h[n]) << (((n-10)/2) % 8);
        for (unsigned n = 1; n < lengths[bank]; n += 2)
            folded ^= unsigned(h[n]) << (((n+13)/2) % 8);
    }
    return folded | (bit(pc, 5) << 8);
}

uint16_t GoldenCoveBP::tag(const GoldenPath &h, Addr pc, unsigned bank) const
{
    // Engineering tag: retain the measured PC[11:0] input and distinguish
    // histories by a 16-bit XOR fold. Its collisions are not a hardware claim.
    uint16_t result = pc & 0xfff;
    for (unsigned n = 0; n < lengths[bank]; ++n)
        result ^= uint16_t(h[n]) << (n % 16);
    return result;
}

GoldenCoveBP::History *GoldenCoveBP::capture(ThreadID tid, Addr pc)
{
    auto *h = new History;
    h->before = paths[tid];
    for (unsigned b = 0; b < 3; ++b) {
        h->index[b] = index(h->before, pc, b);
        h->tag[b] = tag(h->before, pc, b);
    }
    return h;
}

bool GoldenCoveBP::lookup(ThreadID tid, Addr pc, void *&history)
{
    auto *h = capture(tid, pc);
    history = h;
    h->prediction = base[pc & 8191] >= 2;
    for (unsigned b = 0; b < 3; ++b)
        for (unsigned w = 0; w < 4; ++w) {
            auto &e = tables[b][h->index[b]][w];
            if (e.valid && e.tag == h->tag[b]) {
                h->provider = b;
                h->way = w;
                h->prediction = e.counter >= 2;
                e.age = ++age;
                break;
            }
        }
    return h->prediction;
}

void GoldenCoveBP::branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                                     void *&history)
{
    auto *h = capture(tid, pc);
    h->uncond = uncond;
    history = h;
}

void GoldenCoveBP::updateHistories(ThreadID tid, Addr pc, bool uncond,
        bool taken, Addr target, const StaticInstPtr &inst, void *&history)
{
    if (!history) history = capture(tid, pc);
    auto *h = static_cast<History *>(history);
    h->uncond = uncond;
    if (taken) updateGoldenPath(paths[tid], pc, target);
}

void GoldenCoveBP::squash(ThreadID tid, void *&history)
{
    if (!history) return;
    auto *h = static_cast<History *>(history);
    paths[tid] = h->before;
    delete h;
    history = nullptr;
}

void GoldenCoveBP::update(ThreadID tid, Addr pc, bool taken, void *&history,
        bool squashed, const StaticInstPtr &inst, Addr target)
{
    if (!history) return;
    auto *h = static_cast<History *>(history);
    if (squashed) {
        paths[tid] = h->before;
        if (taken) updateGoldenPath(paths[tid], pc, target);
        return;
    }
    if (!h->uncond) {
        train(base[pc & 8191], taken);
        if (h->provider >= 0) {
            auto &e = tables[h->provider][h->index[h->provider]][h->way];
            if (e.valid && e.tag == h->tag[h->provider]) train(e.counter, taken);
        }
        if (h->prediction != taken) {
            // Allocate one longer-history entry; LRU and 2-bit confidence
            // are explicit defaults, not undocumented hardware recovered here.
            const unsigned b = std::min(2, h->provider + 1);
            auto &set = tables[b][h->index[b]];
            auto victim = std::min_element(set.begin(), set.end(),
                [](const Entry &a, const Entry &b) {
                    return (!a.valid ? 0 : a.age) < (!b.valid ? 0 : b.age);
                });
            *victim = {true, h->tag[b], uint8_t(taken ? 2 : 1), ++age};
        }
    }
    delete h;
    history = nullptr;
}

GoldenCoveIndirectPredictor::GoldenCoveIndirectPredictor(
        const GoldenCoveIndirectPredictorParams &p)
    : IndirectPredictor(p), paths(p.numThreads), userMode(p.numThreads, false)
{}

void GoldenCoveIndirectPredictor::reset()
{
    for (auto &h : paths) h.reset();
    for (auto &bank : tables)
        for (auto &set : bank)
            for (auto &e : set) e = Entry{};
    age = 0;
}

unsigned GoldenCoveIndirectPredictor::index(const GoldenPath &h, unsigned length)
{
    // Indirector Fig.8: ignore the low 16 history bits, fold the even/odd
    // streams over nine bits, rotate the odd stream left by five, then XOR.
    return foldParity(h, length, 0, 9) ^ rotate(foldParity(h, length, 1, 9), 5, 9);
}

uint16_t GoldenCoveIndirectPredictor::tag(const GoldenPath &h, Addr pc,
                                        unsigned length)
{
    // Indirector Fig.9. Bit positions are an equivalent fixed permutation
    // of the set/tag output labels; no extra address bits are introduced.
    unsigned result = foldParity(h, length, 0, 11) ^
                      rotate(foldParity(h, length, 1, 11), 6, 11);
    // The low 16 history bits have separate, non-periodic contributions.
    static constexpr unsigned evenPositions[8] = {3, 4, 0, 1, 2, 9, 5, 6};
    static constexpr unsigned oddPositions[8] = {9, 5, 6, 7, 8, 3, 4, 0};
    static constexpr unsigned pcPositions[10] = {3, 9, 4, 5, 0, 6, 1, 7, 2, 8};
    for (unsigned i = 0; i < 8; ++i) {
        result ^= unsigned(h[2*i]) << evenPositions[i];
        result ^= unsigned(h[2*i+1]) << oddPositions[i];
    }
    for (unsigned i = 0; i < 10; ++i)
        result ^= bit(pc, 6+i) << pcPositions[i];
    return result & 0x7ff;
}

const PCStateBase *GoldenCoveIndirectPredictor::lookup(ThreadID tid,
        InstSeqNum sn, Addr pc, void *&history)
{
    auto *h = new History;
    h->before = paths[tid];
    h->pc = pc;
    h->user = userMode[tid];
    h->indirect = true;
    history = h;
    // TODO(SPR-MSR): These static thread/user tags are not an IBRS, STIBP,
    // IPRED, RRSBA or BHI control model; first establish effective states
    // and affected prediction populations for the calibration workload.
    for (unsigned b = 0; b < 3; ++b)
        for (auto &e : tables[b][index(h->before, lengths[b])])
            if (e.target && e.tag == tag(h->before, pc, lengths[b]) &&
                e.thread == tid && e.user == h->user) {
                h->provider = b;
                h->predicted.reset(e.target->clone());
                e.age = ++age;
                break;
            }
    // The BPredUnit owns the returned clone.
    return h->predicted ? h->predicted->clone() : nullptr;
}

void GoldenCoveIndirectPredictor::update(ThreadID tid, InstSeqNum sn,
        Addr pc, bool squash, bool taken, const PCStateBase &target,
        BranchType type, void *&history)
{
    if (!history) {
        auto *h = new History;
        h->before = paths[tid];
        h->pc = pc;
        h->user = userMode[tid];
        history = h;
    }
    auto *h = static_cast<History *>(history);
    if (squash) paths[tid] = h->before;
    h->indirect = type == BranchType::IndirectCond ||
                  type == BranchType::IndirectUncond ||
                  type == BranchType::CallIndirect;
    h->taken = taken;
    h->target.reset(target.clone());
    if (taken) updateGoldenPath(paths[tid], pc, target.instAddr());
}

void GoldenCoveIndirectPredictor::squash(ThreadID tid, InstSeqNum sn, void *&history)
{
    if (!history) return;
    auto *h = static_cast<History *>(history);
    paths[tid] = h->before;
    delete h;
    history = nullptr;
}

void GoldenCoveIndirectPredictor::commit(ThreadID tid, InstSeqNum sn, void *&history)
{
    if (!history) return;
    auto *h = static_cast<History *>(history);
    if (h->indirect && h->taken && h->target) {
        const bool correct = h->predicted && *h->predicted == *h->target;
        const unsigned b = correct ? h->provider : std::min(2, h->provider + 1);
        auto &set = tables[b][index(h->before, lengths[b])];
        const auto wanted = tag(h->before, h->pc, lengths[b]);
        auto entry = std::find_if(set.begin(), set.end(), [&](const Entry &e) {
            return e.target && e.tag == wanted && e.thread == tid && e.user == h->user;
        });
        if (entry == set.end()) entry = std::min_element(set.begin(), set.end(),
            [](const Entry &a, const Entry &b) {
                return (!a.target ? 0 : a.age) < (!b.target ? 0 : b.age);
            });
        entry->tag = wanted;
        entry->thread = tid;
        entry->user = h->user;
        entry->target.reset(h->target->clone());
        entry->age = ++age;
    }
    delete h;
    history = nullptr;
}
}
