// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_PRED_GOLDEN_COVE_HH__
#define __CPU_PRED_GOLDEN_COVE_HH__

#include <array>
#include <bitset>
#include <vector>
#include "cpu/pred/conditional.hh"
#include "cpu/pred/indirect.hh"
#include "params/GoldenCoveBP.hh"
#include "params/GoldenCoveIndirectPredictor.hh"

namespace gem5::branch_prediction
{
using GoldenPath = std::bitset<388>;

/** Half&Half Fig.4b/Table3, independently reproduced on Golden Cove by Jia.
 * Only taken branches shift history; PC is the branch instruction address.
 */
void updateGoldenPath(GoldenPath &history, Addr pc, Addr target);

/** SPR proxy using the measured Alder Lake CBP geometry/history.
 * Short history spans, tag hashing, counter and allocation/replacement policy
 * are explicit engineering defaults, not recovered Golden Cove circuits.
 */
class GoldenCoveBP : public ConditionalPredictor
{
  public:
    explicit GoldenCoveBP(const GoldenCoveBPParams &p);
    bool lookup(ThreadID tid, Addr pc, void *&history) override;
    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
        Addr target, const StaticInstPtr &inst, void *&history) override;
    void update(ThreadID tid, Addr pc, bool taken, void *&history,
        bool squashed, const StaticInstPtr &inst, Addr target) override;
    void squash(ThreadID tid, void *&history) override;
    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
        void *&history) override;

  private:
    struct Entry {
        bool valid = false;
        uint16_t tag = 0;
        uint8_t counter = 1;
        uint64_t age = 0;
    };
    struct History {
        GoldenPath before;
        bool uncond = false, prediction = false;
        int provider = -1, way = -1;
        std::array<unsigned, 3> index{};
        std::array<uint16_t, 3> tag{};
    };
    std::array<unsigned, 3> lengths;
    std::vector<GoldenPath> paths;
    std::array<uint8_t, 8192> base{};
    std::array<std::array<std::array<Entry, 4>, 512>, 3> tables{};
    uint64_t age = 0;
    unsigned index(const GoldenPath &h, Addr pc, unsigned bank) const;
    uint16_t tag(const GoldenPath &h, Addr pc, unsigned bank) const;
    History *capture(ThreadID tid, Addr pc);
};

/** Indirector Sec.3.2/Fig.8-9 geometry and hashes, used as a SPR proxy.
 * Measured on Raptor Cove/Alder Lake, not certified on this SPR machine.
 * Confidence, allocation and LRU replacement remain engineering defaults.
 */
class GoldenCoveIndirectPredictor : public IndirectPredictor
{
  public:
    explicit GoldenCoveIndirectPredictor(const GoldenCoveIndirectPredictorParams &p);
    void reset() override;
    void setUserMode(ThreadID tid, bool user) override { userMode[tid] = user; }
    const PCStateBase *lookup(ThreadID tid, InstSeqNum sn, Addr pc,
        void *&history) override;
    void update(ThreadID tid, InstSeqNum sn, Addr pc, bool squash, bool taken,
        const PCStateBase &target, BranchType type, void *&history) override;
    void squash(ThreadID tid, InstSeqNum sn, void *&history) override;
    void commit(ThreadID tid, InstSeqNum sn, void *&history) override;

  private:
    static constexpr std::array<unsigned, 3> lengths = {68, 132, 388};
    struct Entry {
        uint16_t tag = 0;
        ThreadID thread = 0;
        bool user = false;
        uint64_t age = 0;
        std::unique_ptr<PCStateBase> target;
    };
    struct History {
        GoldenPath before;
        Addr pc = 0;
        bool indirect = false, taken = false, user = false;
        int provider = -1;
        std::unique_ptr<PCStateBase> predicted, target;
    };
    std::vector<GoldenPath> paths;
    std::vector<bool> userMode;
    std::array<std::array<std::array<Entry, 2>, 512>, 3> tables{};
    uint64_t age = 0;
    static unsigned index(const GoldenPath &h, unsigned length);
    static uint16_t tag(const GoldenPath &h, Addr pc, unsigned length);
};
}
#endif
