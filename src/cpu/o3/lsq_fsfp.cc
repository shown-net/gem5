/*
 * Copyright (c) 2010-2014, 2017-2021, 2025 Arm Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
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

#include "cpu/o3/lsq_unit.hh"

#include "arch/generic/debugfaults.hh"
#include "arch/x86/regs/misc.hh"
#include "base/logging.hh"
#include "base/str.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/physical_byte_coverage.hh"
#include "debug/Activity.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/MemoryOrdering.hh"
#include "mem/cache_eviction.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "mem/tso_ordered_store.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

void
LSQUnit::advanceForwarding()
{
    refreshForwardingContext();
    predictLoadValues();
    validateFastForwarding();
}

bool
LSQUnit::fastForwardingAllowed() const
{
    // TODO(SPR MSR calibration): Compare PSFD/SSBD gate effects, predictor
    // training, and fence/context boundaries with controlled native results.
    if (!fsfp)
        return false;
    const auto control = cpu->tcBase(lsqID)->readMiscRegNoEffect(
        X86ISA::misc_reg::SpecCtrl);
    return !(control & ((uint64_t(1) << 2) | (uint64_t(1) << 7)));
}

uint64_t
LSQUnit::forwardingContext() const
{
    // CR3 includes the address-space/PCID identity. Its read value does not
    // retain the write-only no-flush bit, leaving that bit for privilege.
    return cpu->tcBase(lsqID)->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) |
        (cpu->inUserMode(lsqID) ? (uint64_t(1) << 63) : 0);
}

void
LSQUnit::refreshForwardingContext()
{
    if (!fsfp)
        return;
    const auto context = forwardingContext();
    if (!fsfp->forwardingContextValid || context != fsfp->lastForwardingContext ||
        !fastForwardingAllowed()) {
        fsfp->forwardingHistory.clear();
        fsfp->forwardingTraining.clear();
        fsfp->lastForwardingContext = context;
        fsfp->forwardingContextValid = true;
    }
}

bool
LSQUnit::hasFastForwardingPrediction(const DynInstPtr &load) const
{
    if (!fastForwardingAllowed() ||
        !load->staticInst->earlyForwardingSize() || load->isDataPrefetch() ||
        load->isAtomic() || load->isReadBarrier() || load->isWriteBarrier() ||
        load->inHtmTransactionalState())
        return false;
    auto history = fsfp->forwardingHistory.find(
        {forwardingContext(), load->pcState().instAddr() & 0xffff});
    if (history == fsfp->forwardingHistory.end() || history->second.confidence != 7)
        return false;
    for (const auto &entry : storeQueue) {
        const auto &store = entry.instruction();
        if (store->seqNum >= load->seqNum)
            break;
        if (store->isStoreAddress() && !store->isSquashed() &&
            store->staticInst->hasFastForwardingAddress() &&
            (store->pcState().instAddr() & 0xffff) == history->second.storePC)
            return true;
    }
    return false;
}

void
LSQUnit::recordForwardingTraining(const DynInstPtr &load, const DynInstPtr &store)
{
    refreshForwardingContext();
    if (fastForwardingAllowed() && store->isStoreAddress() &&
        load->staticInst->earlyForwardingSize() &&
        store->staticInst->hasFastForwardingAddress() &&
        store->savedRequest && load->savedRequest &&
        !store->savedRequest->isSplit() && !load->savedRequest->isSplit() &&
        store->savedRequest->mainReq()->getPaddr() ==
            load->savedRequest->mainReq()->getPaddr())
        fsfp->forwardingTraining.insert_or_assign(
            load->seqNum, store->pcState().instAddr() & 0xffff);
}

void
LSQUnit::trainForwarding(const DynInstPtr &load, bool failed)
{
    refreshForwardingContext();
    if (!fastForwardingAllowed()) {
        fsfp->forwardingTraining.erase(load->seqNum);
        return;
    }
    const auto key = std::make_pair(
        forwardingContext(), load->pcState().instAddr() & 0xffff);
    const auto observed = fsfp->forwardingTraining.find(load->seqNum);
    auto history = fsfp->forwardingHistory.find(key);
    if (!failed && observed != fsfp->forwardingTraining.end()) {
        if (history == fsfp->forwardingHistory.end() ||
            history->second.storePC != observed->second)
            fsfp->forwardingHistory.insert_or_assign(
                key, ForwardingHistory{observed->second, 0});
        else if (history->second.confidence < 7)
            ++history->second.confidence;
    } else if (history != fsfp->forwardingHistory.end() &&
               history->second.confidence) {
        --history->second.confidence;
    }
    fsfp->forwardingTraining.erase(load->seqNum);
}

bool
LSQUnit::startFastForwarding(const DynInstPtr &load, unsigned size)
{
    refreshForwardingContext();
    if (!size || cpu->checker || !hasFastForwardingPrediction(load) ||
        fsfp->fastForwarding.count(load->seqNum))
        return false;
    const Addr store_pc = fsfp->forwardingHistory.at(
        {forwardingContext(), load->pcState().instAddr() & 0xffff}).storePC;
    auto candidate = storeQueue.end();
    // The load already snapshots the SQ tail at insertion. Search backward
    // from that boundary to find the youngest matching older store.
    auto it = std::min(load->sqIt, storeQueue.end());
    while (it > storeQueue.begin()) {
        --it;
        if (it->instruction()->isStoreAddress() &&
            !it->instruction()->isSquashed() &&
            (it->instruction()->pcState().instAddr() & 0xffff) == store_pc) {
            candidate = it;
            break;
        }
    }
    if (candidate == storeQueue.end() ||
        !candidate->instruction()->staticInst->hasFastForwardingAddress() ||
        (candidate->addressReady() && load->effAddrValid()))
        return false;
    if (!candidate->dataReady() || candidate->dataBytes() < size)
        return false;
    std::vector<uint8_t> value(size);
    std::memcpy(value.data(), candidate->data(), size);
    FastForwarding prediction{load, candidate->instruction(), {},
                              forwardingContext(), false, false, false,
                              size, std::move(value), {}};
    // MDP's observation and FSFP's verification both derive their store
    // population from the LSQ's thread-local older-than-load snapshot.
    // Each predictor still owns its own membership and training policy.
    for (const auto &entry : storeQueue) {
        const auto &store = entry.instruction();
        if (store->seqNum >= load->seqNum)
            break;
        if (store->seqNum >= candidate->instruction()->seqNum)
            prediction.interveningStores.push_back(store);
    }
    fsfp->fastForwarding.emplace(load->seqNum, std::move(prediction));
    // Preserve resolved physical ranges before an older store can leave SQ.
    validateFastForwarding();
    DPRINTF(LSQUnit, "FSFP prediction load [sn:%llu] store [sn:%llu] "
            "issued %d translated %d\n", load->seqNum,
            candidate->instruction()->seqNum, load->isIssued(),
            load->translationStarted());
    ++stats.fastForwardPredictions;
    auto deliver = [this, load] {
        auto complete = [this, load] {
            auto found = fsfp->fastForwarding.find(load->seqNum);
            if (found != fsfp->fastForwarding.end() && !load->isSquashed() &&
                !found->second.failed && load->fault == NoFault &&
                !load->isExecuted()) {
                auto &prediction = found->second;
                // This packet only invokes the functional value operation.
                // It is never translated, sent, or attributed to a cache.
                auto value_req = std::make_shared<Request>(
                    0, prediction.size, 0, Request::funcRequestorId);
                Packet pkt(value_req, MemCmd::ReadReq);
                pkt.dataStatic(prediction.value.data());
                load->completeAcc(&pkt);
                found->second.valueReady = true;
                DPRINTF(LSQUnit, "FSFP value ready load [sn:%llu] "
                        "store [sn:%llu]\n", load->seqNum,
                        prediction.store->seqNum);
                if (load->fault == NoFault)
                    iewStage->instQueue.wakeDependents(load, false);
                validateFastForwarding();
                iewStage->activityThisCycle();
            }
        };
        if (load->isSquashed()) {
            complete();
            return;
        }
        if (lsq->usesLoadCompletion(load, std::make_shared<Request>(
                0, load->staticInst->earlyForwardingSize(), 0,
                Request::funcRequestorId)))
            lsq->enqueueLoadCompletion(load, std::move(complete), true);
        else
            complete();
    };
    auto *event = new EventFunctionWrapper(
        std::move(deliver), name() + ".fastForwardValue", true);
    cpu->schedule(event, cpu->clockEdge(Cycles(1)));
    return true;
}

void
LSQUnit::predictLoadValues()
{
    if (!fastForwardingAllowed() || cpu->checker) return;
    for (auto &entry : loadQueue) {
        const auto &load = entry.instruction();
        if (load->isSquashed() || load->fault != NoFault ||
            fsfp->fastForwarding.count(load->seqNum))
            continue;
        bool operands_unready = false;
        for (int source = 0; source < load->numSrcRegs(); ++source)
            operands_unready |= !load->readySrcIdx(source);
        if (load->isInIQ() && !load->isIssued() && operands_unready &&
            !load->translationStarted() &&
            load->fault == NoFault &&
            startFastForwarding(load, load->staticInst->earlyForwardingSize())) {
            ++stats.fastForwardLoadAddressPredictions;
        }
    }
}

bool
LSQUnit::tryFastForwarding(LSQRequest *request, ssize_t load_idx)
{
    const auto &load = loadQueue[load_idx].instruction();
    const auto req = request->mainReq();
    const bool ordinary = !request->isSplit() && !req->isMasked() &&
        !req->isLLSC() && !req->isReadModifyWrite() && !req->isAcquire() &&
        !req->isLocalAccess() && !req->isUncacheable() &&
        !req->isStrictlyOrdered() && !req->isCacheMaintenance() &&
        !req->isMemMgmt() && !(req->getFlags() & Request::NO_ACCESS);
    if (fsfp->fastForwarding.count(load->seqNum)) {
        validateFastForwarding();
        return true;
    }
    if (!ordinary || partialForwarding.count(load->seqNum) ||
        request->isAnyOutstandingRequest() ||
        !load->staticInst->earlyForwardingSize())
        return false;
    return startFastForwarding(load, req->getSize());
}

void
LSQUnit::validateFastForwarding()
{
    for (auto &[sequence, prediction] : fsfp->fastForwarding) {
        const auto &load = prediction.load;
        if (prediction.validated || prediction.failed || load->isSquashed())
            continue;
        for (auto it = storeQueue.begin(); it != storeQueue.end(); ++it) {
            const auto &store = it->instruction();
            if (store->seqNum < prediction.store->seqNum ||
                store->seqNum >= load->seqNum || !store->effAddrValid() ||
                !it->hasRequest() ||
                prediction.physicalStores.count(store->seqNum))
                continue;
            FastForwarding::StoreBytes bytes;
            if (!it->request()->physicalBytes(bytes.bytes)) continue;
            bytes.secure = it->request()->mainReq()->isSecure();
            bytes.ordinary = isOrdinaryStore(it);
            prediction.physicalStores.emplace(store->seqNum, std::move(bytes));
        }
        if (!prediction.valueReady || !load->savedRequest ||
            !load->effAddrValid() || std::any_of(
            prediction.interveningStores.begin(),
            prediction.interveningStores.end(), [](const auto &store) {
                return !store->effAddrValid() && !store->isSquashed();
            }))
            continue;
        const auto &source = prediction.store;
        const auto req = load->savedRequest->mainReq();
        std::vector<Addr> load_bytes;
        const bool ordinary = !load->savedRequest->isSplit() &&
            !req->isMasked() && !req->isLLSC() && !req->isReadModifyWrite() &&
            !req->isAcquire() && !req->isLocalAccess() &&
            !req->isUncacheable() && !req->isStrictlyOrdered() &&
            !req->isCacheMaintenance() && !req->isMemMgmt() &&
            !(req->getFlags() & Request::NO_ACCESS);
        const auto producer = prediction.physicalStores.find(source->seqNum);
        bool correct = prediction.context == forwardingContext() &&
            fastForwardingAllowed() && ordinary &&
            !source->isSquashed() &&
            source->fault == NoFault && load->fault == NoFault &&
            load->effSize == prediction.size &&
            load->savedRequest->physicalBytes(load_bytes) &&
            producer != prediction.physicalStores.end() &&
            producer->second.ordinary && !producer->second.bytes.empty() &&
            producer->second.secure == req->isSecure() &&
            producer->second.bytes.front() == req->getPaddr();
        if (correct)
            correct = PhysicalByteCoverage::contains(producer->second.bytes,
                                                     load_bytes);
        for (const auto &store : prediction.interveningStores) {
            if (store == source || store->isSquashed())
                continue;
            auto bytes = prediction.physicalStores.find(store->seqNum);
            if (bytes == prediction.physicalStores.end()) {
                correct = false;
                continue;
            }
            if (bytes->second.secure != req->isSecure()) continue;
            if (PhysicalByteCoverage::intersects(load_bytes,
                                                 bytes->second.bytes))
                correct = false;
        }
        if (!correct) {
            prediction.failed = true;
            trainForwarding(load, true);
            ++stats.fastForwardFailures;
            iewStage->squashFastForwarding(load);
            continue;
        }
        prediction.validated = true;
        DPRINTF(LSQUnit, "FSFP validated load [sn:%llu] store [sn:%llu]\n",
                load->seqNum, source->seqNum);
        load->savedRequest->noteNativeTerminal(
            TaoTraceNativeTerminalReason::StoreForward);
        ++stats.fastForwardValidated;
        ++stats.forwLoads;
        rememberForwarding(load, source);
        // Results were delivered earlier. Complete memory ordering exactly
        // once, then make the verified instruction eligible for retirement.
        load->setExecuted();
        iewStage->instQueue.wakeDependents(load, true);
        writeback(load, nullptr);
    }
}

} // namespace o3
} // namespace gem5
