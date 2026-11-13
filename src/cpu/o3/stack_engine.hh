// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_STACK_ENGINE_HH__
#define __CPU_O3_STACK_ENGINE_HH__

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "params/StackEngine.hh"
#include "sim/sim_object.hh"

namespace gem5::o3
{

/**
 * Unaccepted research prototype: its design needs further review.
 * Physical RSP mappings remain owned by Rename.
 */
class StackEngine : public SimObject
{
  public:
    enum class SyncReason : uint8_t { None, Overflow, Unsupported };
    struct Decision
    {
        bool sync = false;
        bool trackedWrite = false;
        bool absorbedUpdate = false;
        bool fallback = false;
        int64_t increment = 0;
        uint64_t sourceMask = 0;
        SyncReason reason = SyncReason::None;
    };

  private:
    struct Change { InstSeqNum sequence; int64_t before, after; };
    struct ThreadState
    {
        int64_t speculative = 0;
        int64_t committed = 0;
        std::deque<Change> changes;
        std::map<InstSeqNum, int64_t> checkpoints;
    };
    bool active;
    const int64_t minimum, maximum, immediateMinimum, immediateMaximum;
    bool syncZero;
    std::vector<ThreadState> threads;

    struct Stats : statistics::Group
    {
        statistics::Scalar tracked, fallback, syncCreated, syncExecuted,
            syncRetired, syncSquashed, overflowSync, unsupportedSync,
            branchCheckpoints, branchRestores, otherRestores;
        explicit Stats(statistics::Group *parent);
    } stats;

  public:
    explicit StackEngine(const StackEngineParams &p);
    void configure(unsigned num_threads);
    bool enabled() const { return active; }
    int64_t offset(ThreadID tid) const { return threads.at(tid).speculative; }
    int64_t committedOffset(ThreadID tid) const { return threads.at(tid).committed; }
    bool inRange(int64_t value) const { return value >= minimum && value <= maximum; }
    bool tracksImmediate(int64_t value) const
    { return value >= immediateMinimum && value <= immediateMaximum; }
    bool synchronizeZero() const { return syncZero; }
    Decision classify(const DynInstPtr &inst) const;
    DynInstPtr makeSync(const DynInstPtr &before, SyncReason reason);
    void apply(ThreadID tid, InstSeqNum sequence, int64_t value);
    void checkpoint(ThreadID tid, InstSeqNum sequence);
    void squash(ThreadID tid, InstSeqNum through);
    void retire(ThreadID tid, InstSeqNum through, bool changed, int64_t value);
    void reset(ThreadID tid);
    void noteTracked() { ++stats.tracked; }
    void noteFallback() { ++stats.fallback; }
    void noteSyncCreated(SyncReason reason);
    void noteSyncExecuted() { ++stats.syncExecuted; }
    void noteSyncRetired() { ++stats.syncRetired; }
    uint64_t retiredSynchronizations() const { return stats.syncRetired.value(); }
    void noteSyncSquashed() { ++stats.syncSquashed; }
};

} // namespace gem5::o3
#endif
