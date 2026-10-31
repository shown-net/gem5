// SPDX-License-Identifier: BSD-3-Clause
// Behavioral candidate: shared taken-path history, direction and target tables.
#ifndef __CPU_PRED_PATH_PREDICTORS_HH__
#define __CPU_PRED_PATH_PREDICTORS_HH__

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

#include "cpu/pred/conditional.hh"
#include "cpu/pred/indirect.hh"
#include "params/BranchPathHistory.hh"
#include "params/PathIndirectPredictor.hh"
#include "params/PathTAGE.hh"

namespace gem5::branch_prediction
{

class BranchPathHistory : public SimObject
{
  public:
    using Bits = std::bitset<388>;
    BranchPathHistory(const BranchPathHistoryParams &p);
    const Bits &read(ThreadID tid) const { return history.at(tid); }
    const Bits &previous(ThreadID tid) const { return prior.at(tid); }
    void restore(ThreadID tid, const Bits &bits);
    void advance(ThreadID tid, Addr branch, Addr target, bool taken);

  private:
    std::vector<Bits> history;
    std::vector<Bits> prior;
};

class PathTAGE : public ConditionalPredictor
{
  public:
    PathTAGE(const PathTAGEParams &p);
    bool lookup(ThreadID tid, Addr pc, void *&bp_history) override;
    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void *&bp_history) override;
    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                           void *&bp_history) override;
    void squash(ThreadID tid, void *&bp_history) override;
    void update(ThreadID tid, Addr pc, bool taken, void *&bp_history,
                bool squashed, const StaticInstPtr &inst, Addr target) override;

  private:
    struct Entry
    {
        uint16_t tag = 0;
        int8_t counter = 0;
        uint8_t usefulness = 0;
        bool valid = false;
        uint64_t lastUse = 0;
    };
    struct History
    {
        BranchPathHistory::Bits before;
        std::array<unsigned, 3> index{};
        std::array<unsigned, 3> tag{};
        std::array<int, 3> way{{-1, -1, -1}};
        int provider = -1;
        int alternate = -1;
        bool predicted = false;
        bool longest = false;
        bool alternatePrediction = false;
        bool lookedUp = false;
        bool placeholder = false;
    };
    using Set = std::array<Entry, 4>;
    BranchPathHistory *path;
    std::array<uint8_t, 8192> base{};
    std::array<std::array<Set, 512>, 3> tables{};
    int8_t useAlternate = 0;
    uint64_t serial = 0;
    uint64_t updates = 1ULL << 17;

    static unsigned fold(const BranchPathHistory::Bits &bits,
                         unsigned length, unsigned width);
    void train(Addr pc, bool taken, const History &history);
};

class PathIndirectPredictor : public IndirectPredictor
{
  public:
    PathIndirectPredictor(const PathIndirectPredictorParams &p);
    void reset() override;
    void setUserMode(ThreadID tid, bool user) override;
    const PCStateBase *lookup(ThreadID tid, InstSeqNum sn, Addr pc,
                              void *&i_history) override;
    void update(ThreadID tid, InstSeqNum sn, Addr pc, bool squash, bool taken,
                const PCStateBase &target, BranchType type,
                void *&i_history) override;
    void squash(ThreadID tid, InstSeqNum sn, void *&i_history) override;
    void commit(ThreadID tid, InstSeqNum sn, void *&i_history) override;

  private:
    struct Entry
    {
        uint16_t tag = 0;
        bool valid = false;
        bool user = false;
        uint64_t lastUse = 0;
        std::unique_ptr<PCStateBase> target;
    };
    struct History
    {
        std::array<unsigned, 3> index{};
        std::array<unsigned, 3> tag{};
        int provider = -1;
        int way = -1;
        bool user = false;
        bool lookedUp = false;
    };
    using Set = std::array<Entry, 2>;
    BranchPathHistory *path;
    std::vector<bool> userMode;
    std::array<std::array<Set, 512>, 3> tables{};
    uint64_t serial = 0;

    static unsigned index(const BranchPathHistory::Bits &bits, unsigned length);
    static unsigned tag(const BranchPathHistory::Bits &bits,
                        unsigned length, Addr pc);
    void allocate(unsigned table, const History &history,
                  const PCStateBase &target);
};
}
#endif
