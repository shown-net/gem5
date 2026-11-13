// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/spr_memory_predictor.hh"

#include <algorithm>

#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/physical_byte_coverage.hh"

namespace gem5::o3
{

void
SprMemoryPredictor::trackStore(InstSeqNum sequence)
{
    memDepHash.try_emplace(sequence, std::make_shared<StoreState>());
}

void
SprMemoryPredictor::untrackStore(InstSeqNum sequence)
{
    memDepHash.erase(sequence);
}

void
SprMemoryPredictor::squash(InstSeqNum sequence)
{
    for (auto it = loadObservations.begin(); it != loadObservations.end();)
        if (it->first > sequence)
            it = loadObservations.erase(it);
        else
            ++it;
    for (auto it = memDepHash.begin(); it != memDepHash.end();)
        if (it->first > sequence)
            it = memDepHash.erase(it);
        else
            ++it;
}

void
SprMemoryPredictor::clear()
{
    independence.fill(0);
    speculationCredit = 3;
    safePredictions = 0;
    loadObservations.clear();
    memDepHash.clear();
}

void
SprMemoryPredictor::violation(const DynInstPtr &,
                              const DynInstPtr &violating_load)
{
    auto found = loadObservations.find(violating_load->seqNum);
    if (found == loadObservations.end())
        return;
    auto &observation = found->second;
    observation.orderingFailed = true;
    trainLoad(observation);
    if (!observation.bypassAdmitted || observation.violationCharged)
        return;
    observation.violationCharged = true;
    ++stats.watchdogFailures;
    if (speculationCredit == 0)
        ++stats.watchdogDisables;
    speculationCredit = std::max(-1, speculationCredit - 1);
    safePredictions = 0;
}

void
SprMemoryPredictor::observeLoad(const DynInstPtr &inst)
{
    assert(lsq);
    // Called once for each new effective-address attempt, never for an LSU
    // continuation. Previous completed learning remains predictor state.
    auto &observation = loadObservations.insert_or_assign(
        inst->seqNum, LoadObservation{}).first->second;
    observation.load = inst;
    ++stats.localQueries;
    observation.localPrediction =
        independence[inst->pcState().instAddr() & 511] == 15;
    for (const auto &store : lsq->olderStores(inst)) {
        auto entry = memDepHash.find(store->seqNum);
        const bool member = entry != memDepHash.end() ?
            !entry->second->addressGenerated : !store->translationStarted();
        StoreObservation sample{store, member, false, {}};
        sample.orderingChecked = entry != memDepHash.end() ?
            entry->second->orderingChecked : store->effAddrValid();
        if (store->translationStarted() && store->savedRequest) {
            sample.physicalReady =
                store->savedRequest->physicalBytes(sample.bytes);
            if (sample.physicalReady)
                sample.secure = store->savedRequest->mainReq()->isSecure();
        }
        observation.hadUnresolvedStore |= member;
        observation.olderStores.push_back(std::move(sample));
    }
}

void
SprMemoryPredictor::trainLoad(LoadObservation &observation)
{
    if (observation.result != ObservationResult::Pending) {
        recoverWatchdog(observation);
        return;
    }
    if (!observation.physicalReady || observation.load->isSquashed() ||
        observation.load->fault != NoFault ||
        observation.load->strictlyOrdered())
        return;
    if (!observation.hadUnresolvedStore || observation.bytes.empty()) {
        observation.result = ObservationResult::Cancelled;
        return;
    }
    bool pending = false;
    bool intersection = false;
    bool dependent = false;
    const auto &stores = observation.olderStores;
    // The stores are in program order and physicalBytes() returns sorted,
    // unique byte addresses. Walk younger-to-older once so coverageScratch[j]
    // denotes the bytes already supplied by a younger, resolved store. This
    // retains the original U-set and unknown-younger-store semantics without
    // repeatedly subtracting every younger range for every older store.
    coverageScratch.assign(observation.bytes.size(), false);
    bool unknownYounger = false;
    for (auto it = stores.rbegin(); it != stores.rend(); ++it) {
        const auto &sample = *it;
        if (sample.store->isSquashed())
            continue;
        if (!sample.physicalReady) {
            if (sample.member)
                pending = true;
            unknownYounger = true;
            continue;
        }
        if (sample.secure != observation.secure)
            continue;
        const auto match = PhysicalByteCoverage::cover(
            observation.bytes, sample.bytes, coverageScratch);
        if (sample.member)
            intersection |= match.overlap;
        if (sample.member && match.uncovered) {
            if (unknownYounger)
                pending = true;
            else
                dependent = true;
        }
    }
    if (!dependent && pending)
        return;
    auto &counter = independence[observation.load->pcState().instAddr() & 511];
    if (dependent) {
        observation.result = ObservationResult::Dependent;
        counter = 0;
        ++stats.resolvedDependentLoads;
    } else {
        if (intersection) {
            observation.result = ObservationResult::Covered;
            ++stats.coveredLoads;
        } else {
            observation.result = ObservationResult::Independent;
            counter = std::min<unsigned>(15, counter + 1);
            ++stats.resolvedIndependentLoads;
        }
    }
    recoverWatchdog(observation);
}

void
SprMemoryPredictor::recoverWatchdog(LoadObservation &observation)
{
    // Local U learning may finish before an outside-U store's translation.
    // Global safety is known only after all older store ordering checks;
    // an independent local sample must not forgive a real outside-U failure.
    if (observation.watchdogObserved || !observation.localPrediction ||
        !observation.hadUnresolvedStore || observation.violationCharged ||
        observation.orderingFailed ||
        observation.load->isSquashed() || observation.load->fault != NoFault ||
        (observation.result != ObservationResult::Independent &&
         observation.result != ObservationResult::Covered))
        return;
    for (const auto &sample : observation.olderStores)
        if (!sample.store->isSquashed() &&
            (!sample.physicalReady || !sample.orderingChecked))
            return;
    observation.watchdogObserved = true;
    ++stats.watchdogSafeOpportunities;
    // These opportunities do not require actual speculative admission, so
    // inhibited loads can restore the watchdog after ordinary LSU waiting.
    if (++safePredictions == 64) {
        if (speculationCredit < 3) {
            ++speculationCredit;
            ++stats.watchdogRecoveries;
        }
        safePredictions = 0;
    }
}

bool
SprMemoryPredictor::canSpeculate(const DynInstPtr &inst)
{
    auto found = loadObservations.find(inst->seqNum);
    if (found == loadObservations.end())
        return false;
    auto &observation = found->second;
    if (observation.result == ObservationResult::Dependent)
        return false;
    // TODO(SPR MSR calibration): Audit already-admitted loads when SSBD changes;
    // WRMSR serializes after in the current decoder, but other state-transfer
    // paths and in-flight ordering still need validation. The dynamic control
    // is consulted only for new admissions below.
    if (observation.bypassAdmitted)
        return true;
    if (observation.localPrediction && speculationCredit >= 0 &&
        lsq->memorySpeculationAllowed(inst)) {
        observation.bypassAdmitted = true;
        if (observation.hadUnresolvedStore)
            ++stats.independencePredictions;
        return true;
    }
    return false;
}

const std::vector<Addr> *
SprMemoryPredictor::physicalReadBytes(const DynInstPtr &inst) const
{
    auto found = loadObservations.find(inst->seqNum);
    return found != loadObservations.end() && found->second.physicalReady ?
        &found->second.bytes : nullptr;
}

SprMemoryPredictor::StoreObservation *
SprMemoryPredictor::LoadObservation::findStore(const DynInstPtr &inst)
{
    const auto found = std::lower_bound(olderStores.begin(), olderStores.end(),
        inst->seqNum, [](const StoreObservation &sample, InstSeqNum sequence) {
            return sample.store->seqNum < sequence;
        });
    return found != olderStores.end() && found->store == inst ? &*found : nullptr;
}

void
SprMemoryPredictor::addressGenerated(const DynInstPtr &inst)
{
    if (inst->isLoad() && !inst->isDataPrefetch()) {
        observeLoad(inst);
    } else if (inst->isStore() || inst->isAtomic()) {
        auto entry = memDepHash.find(inst->seqNum);
        if (entry != memDepHash.end()) {
            entry->second->addressGenerated = true;
            entry->second->orderingChecked = false;
        }
        for (auto &[sequence, observation] : loadObservations)
            if (auto *sample = observation.findStore(inst)) {
                sample->physicalReady = false;
                sample->orderingChecked = false;
                sample->bytes.clear();
            }
    }
}

void
SprMemoryPredictor::addressReady(const DynInstPtr &inst)
{
    if (inst->isLoad()) {
        auto found = loadObservations.find(inst->seqNum);
        if (found != loadObservations.end()) {
            if (inst->savedRequest) {
                found->second.physicalReady = inst->savedRequest->physicalBytes(
                    found->second.bytes);
                if (found->second.physicalReady)
                    found->second.secure =
                        inst->savedRequest->mainReq()->isSecure();
            }
            trainLoad(found->second);
        }
        return;
    }
    bool captured = false;
    bool physical_ready = false;
    bool secure = false;
    std::vector<Addr> bytes;
    for (auto &[sequence, observation] : loadObservations) {
        if (auto *sample = observation.findStore(inst);
            sample && inst->savedRequest) {
            // One synchronous store update has one physical byte range.
            // Failed expansion leaves each observation's old bytes intact.
            if (!captured) {
                captured = true;
                physical_ready = inst->savedRequest->physicalBytes(bytes);
                if (physical_ready)
                    secure = inst->savedRequest->mainReq()->isSecure();
            }
            sample->physicalReady = physical_ready;
            if (physical_ready) {
                sample->bytes = bytes;
                sample->secure = secure;
            }
        }
        trainLoad(observation);
    }
}

void
SprMemoryPredictor::storeOrderChecked(const DynInstPtr &inst, InstSeqNum firstViolation)
{
    auto entry = memDepHash.find(inst->seqNum);
    if (entry != memDepHash.end())
        entry->second->orderingChecked = true;
    for (auto &[sequence, observation] : loadObservations) {
        if (auto *sample = observation.findStore(inst))
            sample->orderingChecked = true;
        if (firstViolation && sequence >= firstViolation)
            observation.orderingFailed = true;
        else
            recoverWatchdog(observation);
    }
}

void
SprMemoryPredictor::noteAddressWait(const DynInstPtr &load)
{
    auto found = loadObservations.find(load->seqNum);
    if (found == loadObservations.end() || found->second.addressWaitObserved)
        return;
    found->second.addressWaitObserved = true;
    ++stats.addressWaits;
    if (found->second.localPrediction && speculationCredit < 0)
        ++stats.watchdogBlocks;
}

void
SprMemoryPredictor::loadRetired(const DynInstPtr &load)
{
    auto found = loadObservations.find(load->seqNum);
    if (found != loadObservations.end()) {
        trainLoad(found->second);
        loadObservations.erase(found);
    }
}

} // namespace gem5::o3
