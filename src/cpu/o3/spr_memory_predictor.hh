// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_SPR_MEMORY_PREDICTOR_HH__
#define __CPU_O3_SPR_MEMORY_PREDICTOR_HH__

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5::o3
{

class LSQ;

// Detached research component. The SPR calibration profile does not create
// this object. Reconnection requires supplying store lifetime and LSQ events.
class SprMemoryPredictor
{
  public:
    explicit SprMemoryPredictor(LSQ *queue) : lsq(queue) {}

    void trackStore(InstSeqNum sequence);
    void untrackStore(InstSeqNum sequence);
    void squash(InstSeqNum sequence);
    void clear();
    void addressGenerated(const DynInstPtr &inst);
    void addressReady(const DynInstPtr &inst);
    void storeOrderChecked(const DynInstPtr &inst, InstSeqNum firstViolation = 0);
    void noteAddressWait(const DynInstPtr &load);
    void loadRetired(const DynInstPtr &load);
    void violation(const DynInstPtr &store, const DynInstPtr &load);
    bool canSpeculate(const DynInstPtr &load);
    const std::vector<Addr> *physicalReadBytes(const DynInstPtr &load) const;

  private:
    LSQ *lsq;
    std::array<uint8_t, 512> independence{};
    int speculationCredit = 3;
    unsigned safePredictions = 0;
    enum class ObservationResult { Pending, Independent, Dependent, Covered,
                                   Cancelled };
    struct StoreState {
        bool addressGenerated = false;
        bool orderingChecked = false;
    };
    std::unordered_map<InstSeqNum, std::shared_ptr<StoreState>> memDepHash;
    struct StoreObservation {
        DynInstPtr store;
        bool member = false;
        bool physicalReady = false;
        std::vector<Addr> bytes;
        bool orderingChecked = false;
        bool secure = false;
    };
    struct LoadObservation {
        DynInstPtr load;
        std::vector<StoreObservation> olderStores;
        std::vector<Addr> bytes;
        bool physicalReady = false;
        bool secure = false;
        ObservationResult result = ObservationResult::Pending;
        bool localPrediction = false;
        bool hadUnresolvedStore = false;
        bool bypassAdmitted = false;
        bool addressWaitObserved = false;
        bool violationCharged = false;
        bool orderingFailed = false;
        bool watchdogObserved = false;
        StoreObservation *findStore(const DynInstPtr &inst);
    };
    std::unordered_map<InstSeqNum, LoadObservation> loadObservations;
    std::vector<bool> coverageScratch;
    struct Counters {
        uint64_t localQueries = 0, independencePredictions = 0, addressWaits = 0;
        uint64_t resolvedIndependentLoads = 0, resolvedDependentLoads = 0;
        uint64_t coveredLoads = 0, watchdogBlocks = 0, watchdogDisables = 0;
        uint64_t watchdogRecoveries = 0, watchdogFailures = 0;
        uint64_t watchdogSafeOpportunities = 0;
    } stats;
    void observeLoad(const DynInstPtr &inst);
    void trainLoad(LoadObservation &observation);
    void recoverWatchdog(LoadObservation &observation);
};

} // namespace gem5::o3
#endif
