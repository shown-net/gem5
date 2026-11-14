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

LSQUnit::WritebackEvent::WritebackEvent(const DynInstPtr &_inst,
        PacketPtr _pkt, LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr), request(_inst->savedRequest)
{
    assert(request);
    request->writebackScheduled();
}

void
LSQUnit::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    if (inst->isSquashed() || inst->savedRequest != request) {
        ++lsqPtr->stats.ignoredResponses;
        request->writebackDone();
        delete pkt;
        return;
    }
    if (lsqPtr->lsq->usesLoadCompletion(inst, request->mainReq())) {
        auto *unit = lsqPtr;
        const DynInstPtr load = inst;
        PacketPtr data = pkt;
        auto *pending = request;
        unit->lsq->enqueueLoadCompletion(load, [unit, load, pending, data] {
            if (!load->isSquashed() && load->savedRequest == pending)
                unit->writeback(load, data);
            else
                ++unit->stats.ignoredResponses;
            pending->writebackDone();
            delete data;
        });
        return;
    }
    lsqPtr->writeback(inst, pkt);

    request->writebackDone();
    delete pkt;
}

const char *
LSQUnit::WritebackEvent::description() const
{
    return "Store writeback";
}

bool
LSQUnit::recvTimingResp(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
    assert(request != nullptr);
    bool ret = true;
    /* Check that the request is still alive before any further action. */
    if (!request->isReleased()) {
        ret = request->recvTimingResp(pkt);
    }
    return ret;
}

void
LSQUnit::completeDataAccess(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest *>(pkt->senderState);
    DynInstPtr inst = request->instruction();

    // hardware transactional memory
    // sanity check
    if (pkt->isHtmTransactional() && !inst->isSquashed()) {
        assert(inst->getHtmTransactionUid() == pkt->getHtmTransactionUid());
    }

    // if in a HTM transaction, it's possible
    // to abort within the cache hierarchy.
    // This is signalled back to the processor
    // through responses to memory requests.
    if (pkt->htmTransactionFailedInCache()) {
        // cannot do this for write requests because
        // they cannot tolerate faults
        const HtmCacheFailure htm_rc =
            pkt->getHtmTransactionFailedInCacheRC();
        if (pkt->isWrite()) {
            DPRINTF(HtmCpu,
                "store notification (ignored) of HTM transaction failure "
                "in cache - addr=0x%lx - rc=%s - htmUid=%d\n",
                pkt->getAddr(), htmFailureToStr(htm_rc),
                pkt->getHtmTransactionUid());
        } else {
            HtmFailureFaultCause fail_reason =
                HtmFailureFaultCause::INVALID;

            if (htm_rc == HtmCacheFailure::FAIL_SELF) {
                fail_reason = HtmFailureFaultCause::SIZE;
            } else if (htm_rc == HtmCacheFailure::FAIL_REMOTE) {
                fail_reason = HtmFailureFaultCause::MEMORY;
            } else if (htm_rc == HtmCacheFailure::FAIL_OTHER) {
                // these are likely loads that were issued out of order
                // they are faulted here, but it's unlikely that these will
                // ever reach the commit head.
                fail_reason = HtmFailureFaultCause::OTHER;
            } else {
                panic("HTM error - unhandled return code from cache (%s)",
                      htmFailureToStr(htm_rc));
            }

            inst->fault =
            std::make_shared<GenericHtmFailureFault>(
                inst->getHtmTransactionUid(),
                fail_reason);

            DPRINTF(HtmCpu,
                "load notification of HTM transaction failure "
                "in cache - pc=%s - addr=0x%lx - "
                "rc=%u - htmUid=%d\n",
                inst->pcState(), pkt->getAddr(),
                htmFailureToStr(htm_rc), pkt->getHtmTransactionUid());
        }
    }

    cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (request->needWBToRegister()) {
            // Only loads, store conditionals and atomics perform the writeback
            // after receving the response from the memory
            assert(inst->isLoad() || inst->isStoreConditional() ||
                   inst->isAtomic());

            // hardware transactional memory
            if (pkt->htmTransactionFailedInCache()) {
                request->mainPacket()->setHtmTransactionFailedInCache(
                    pkt->getHtmTransactionFailedInCacheRC() );
            }

            if (lsq->usesLoadCompletion(inst, request->mainReq())) {
                // The LSQ request owns its main packet, including the
                // synthetic packet for a split access. Keep it alive until
                // the shared load-result resource delivers the value.
                request->writebackScheduled();
                lsq->enqueueLoadCompletion(inst, [this, inst, request] {
                    cpu->recordCompletedLoad(inst);
                    writeback(inst, request->mainPacket());
                    request->writebackDone();
                });
            } else {
                cpu->recordCompletedLoad(inst);
                writeback(inst, request->mainPacket());
            }
            if (inst->isStore() || inst->isAtomic()) {
                request->writebackDone();
                completeStore(request->instruction()->sqIt);
            }
        } else if (inst->isStore()) {
            // This is a regular store (i.e., not store conditionals and
            // atomics), so it can complete without writing back
            completeStore(request->instruction()->sqIt);
        }
    }
}

LSQUnit::LSQUnit(uint32_t lqEntries, uint32_t sqEntries)
    : lsqID(-1), storeQueue(sqEntries), loadQueue(lqEntries),
      storesToWB(0),
      htmStarts(0), htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0), stalled(false),
      isStoreBlocked(false), storeInFlight(false), stats(nullptr)
{
}

void
LSQUnit::init(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params,
        LSQ *lsq_ptr, unsigned id)
{
    lsqID = id;

    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    cpu->addStatGroup(csprintf("lsq%i", lsqID).c_str(), &stats);
    stats.storeBufferLogicalLive.functor(
        [this]() { return committedStoreCount(); });

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",lsqID);

    depCheckShift = params.LSQDepCheckShift;
    checkLoads = params.LSQCheckLoads;
    needsTSO = params.needsTSO;
    // The CPU constructs its pipeline before initializing cpu->checker.
    sprStoreBuffer = params.sprStoreBuffer && needsTSO && !params.checker;
    sprStoreDrainLineCredits = params.sprStoreDrainLineCredits;
    sprEarlyStorePrepare = params.sprEarlyStorePrepare && sprStoreBuffer;
    sprRetiredStorePrepare = params.sprRetiredStorePrepare;
    sprClflushMinCycles = params.sprClflushMinCycles;
    sprMemoryDependence = params.sprMemoryDependence;
    // SPR uses the fixed no-prediction profile. Keep the existing predictor
    // available only to explicitly configured non-SPR O3 models.
    if (!sprMemoryDependence && params.sprFastStoreForwarding && !params.checker)
        fsfp = std::make_unique<FastForwardingState>();
    sprStoreForwardingLatency = params.sprStoreForwardingLatency;
    sprMultiStoreForwarding = params.sprMultiStoreForwarding;
    sprContainedForwardingExtraLatency = params.sprContainedForwardingExtraLatency;
    iewStage->instQueue.memoryDependence(lsqID).setLSQ(lsq);

    resetState();
}


void
LSQUnit::resetState()
{
    while (!capacityReadLines.empty())
        releaseCapacityRead(capacityReadLines.begin()->first);
    panic_if(!committedStoreBuffer.empty(),
             "Cannot reset a nonempty committed store buffer");
    assert(committedStores == 0);
    assert(issuedStoreBufferEntries == 0);
    storeBufferLinesInFlight.clear();
    storesToWB = 0;
    storeInFlight = false;
    tsoStoresInFlight = 0;
    tsoInFlightSameLineEligible = false;
    tsoLinesInFlight.clear();
    selectiveBlockedLoads.clear();
    partialForwarding.clear();
    fullForwardingSource.clear();
    if (fsfp)
        *fsfp = FastForwardingState{};

    // hardware transactional memory
    // nesting depth
    htmStarts = htmStops = 0;

    storeWBIt = storeQueue.begin();

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);
}

std::string
LSQUnit::name() const
{
    if (MaxThreads == 1) {
        return iewStage->name() + ".lsq";
    } else {
        return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

LSQUnit::LSQUnitStats::LSQUnitStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(forwLoads, statistics::units::Count::get(),
               "Number of loads that had data forwarded from stores"),
      ADD_STAT(sameStartPartialLoads, statistics::units::Count::get(),
               "Same-start partial store forwarding merged with cache data"),
      ADD_STAT(sameStartMixedStoreProducers, statistics::units::Count::get(),
               "Same-start forwarding attempts with both SQ and committed-buffer producers"),
      ADD_STAT(sameStartMixedStoreRejected, statistics::units::Count::get(),
               "Mixed SQ and committed-buffer forwarding attempts rejected by the multi-store policy"),
      ADD_STAT(samePageSplitCandidates, statistics::units::Count::get(),
               "Validated complete same-start split-line SQ forwarding attempts"),
      ADD_STAT(samePageSplitForwarded, statistics::units::Count::get(),
               "Validated complete same-start split-line SQ forwards"),
      ADD_STAT(fastForwardPredictions, statistics::units::Count::get(),
               "FSFP values predicted before physical producer verification"),
      ADD_STAT(fastForwardLoadAddressPredictions, statistics::units::Count::get(),
               "FSFP predictions while the load address operands are unready"),
      ADD_STAT(fastForwardValidated, statistics::units::Count::get(),
               "FSFP proxy predictions with verified address and age"),
      ADD_STAT(fastForwardFailures, statistics::units::Count::get(),
               "Failed dynamic FSFP validations; accepted clears are counted separately"),
      ADD_STAT(noncontiguousTranslations, statistics::units::Count::get(),
               "Complete split requests with noncontiguous physical byte mapping"),
      ADD_STAT(repReadBatches, statistics::units::Count::get(),
               "Retired bounded REP read requests"),
      ADD_STAT(repWriteBatches, statistics::units::Count::get(),
               "Retired bounded REP write requests"),
      ADD_STAT(repReadBytes, statistics::units::Byte::get(),
               "Active bytes of retired bounded REP reads"),
      ADD_STAT(repWriteBytes, statistics::units::Byte::get(),
               "Active bytes of retired bounded REP writes"),
      ADD_STAT(squashedLoads, statistics::units::Count::get(),
               "Number of loads squashed"),
      ADD_STAT(ignoredResponses, statistics::units::Count::get(),
               "Number of memory responses ignored because the instruction is "
               "squashed"),
      ADD_STAT(localCapacityEvictions, statistics::units::Count::get(),
               "Private capacity eviction notifications; do not change coherent values"),
      ADD_STAT(coherenceInvalidations, statistics::units::Count::get(),
               "Invalidations without a private capacity replacement origin"),
      ADD_STAT(capacityReadRegistrations, statistics::units::Count::get(),
               "LQ line observations retained across private replacements"),
      ADD_STAT(capacityReadWrites, statistics::units::Count::get(),
               "Real writes observed after private cache replacement"),
      ADD_STAT(memOrderViolation, statistics::units::Count::get(),
               "Number of memory ordering violations"),
      ADD_STAT(returnedLoadOrderViolations, statistics::units::Count::get(),
               "Store violations after load data returned but before execution"),
      ADD_STAT(storeOrderCandidateExecuted, statistics::units::Count::get(),
               "Physical store-order candidates whose load has executed"),
      ADD_STAT(storeOrderCandidateForwarded, statistics::units::Count::get(),
               "Physical store-order candidates with unexecuted selected forwarding"),
      ADD_STAT(storeOrderCandidateReturned, statistics::units::Count::get(),
               "Physical store-order candidates with unexecuted returned data and no selected forwarding"),
      ADD_STAT(storeOrderCandidateOutstanding, statistics::units::Count::get(),
               "Other physical store-order candidates with an outstanding load packet"),
      ADD_STAT(squashedStores, statistics::units::Count::get(),
               "Number of stores squashed"),
      ADD_STAT(rescheduledLoads, statistics::units::Count::get(),
               "Number of loads that were rescheduled"),
      ADD_STAT(selectivePartialWaits, statistics::units::Count::get(),
               "Partial-overlap loads registered with a blocking store"),
      ADD_STAT(selectivePartialWakeups, statistics::units::Count::get(),
               "Tracked loads individually woken after their blocking store"),
      ADD_STAT(selectivePartialSquashedWaits, statistics::units::Count::get(),
               "Tracked partial-overlap loads removed by squash"),
      ADD_STAT(splitStoreAddressFirst, statistics::units::Count::get(),
               "Split stores whose address arrived before their data"),
      ADD_STAT(splitStoreDataFirst, statistics::units::Count::get(),
               "Split stores whose data arrived before their address"),
      ADD_STAT(splitStoreDataWaits, statistics::units::Count::get(),
               "Loads waiting for an older split store's data"),
      ADD_STAT(retainedStoreDataWaits, statistics::units::Count::get(),
               "Full-overlap loads waiting with their translated request"),
      ADD_STAT(retainedStoreDataResumes, statistics::units::Count::get(),
               "Retained loads resumed without a new IQ or AGU issue"),
      ADD_STAT(loadBankConflicts, statistics::units::Count::get(),
               "Load packet attempts delayed by the same-bank limit"),
      ADD_STAT(storeBufferOffloaded, statistics::units::Count::get(),
               "Retired stores transferred to the committed store buffer"),
      ADD_STAT(storeBufferMerged, statistics::units::Count::get(),
               "Retired stores merged into an unsent line entry"),
      ADD_STAT(storeBufferSent, statistics::units::Count::get(),
               "Masked line writes sent by the committed store buffer"),
      ADD_STAT(storeBufferCompleted, statistics::units::Count::get(),
               "Committed store buffer line responses"),
      ADD_STAT(storeBufferLogicalSent, statistics::units::Count::get(),
               "Logical committed stores represented by accepted line requests"),
      ADD_STAT(storeBufferLogicalCompleted, statistics::units::Count::get(),
               "Logical committed stores whose line response returned"),
      ADD_STAT(storeBufferLogicalLive, statistics::units::Count::get(),
               "Current uncompleted logical committed stores"),
      ADD_STAT(storeBufferForwardedLoads, statistics::units::Count::get(),
               "Loads fully forwarded from committed store buffer bytes"),
      ADD_STAT(storeBufferPartialWaits, statistics::units::Count::get(),
               "Loads waiting for incomplete committed-store coverage"),
      ADD_STAT(storeBufferEntryCycles, statistics::units::Count::get(),
               "Committed logical stores sampled each active LSQ cycle"),
      ADD_STAT(storeBufferLineEntryCycles, statistics::units::Count::get(),
               "Committed store buffer line entries sampled each active LSQ cycle"),
      ADD_STAT(sharedStoreEntryCycles, statistics::units::Count::get(),
               "SQ entries plus committed logical stores sampled each active LSQ cycle"),
      ADD_STAT(sharedStoreFullCycles, statistics::units::Cycle::get(),
               "Active LSQ cycles with the shared store budget full"),
      ADD_STAT(storeDrainCacheBlocked, statistics::units::Count::get(),
               "Committed store drain attempts stopped by a blocked cache port"),
      ADD_STAT(storeDrainPortBlocked, statistics::units::Count::get(),
               "Committed store drain attempts stopped by store port or width budget"),
      ADD_STAT(storeDrainLineCreditBlocked, statistics::units::Count::get(),
               "Committed store drain attempts stopped by configured in-flight line credits"),
      ADD_STAT(blockedByCache, statistics::units::Count::get(),
               "Number of times an access to memory failed due to the cache "
               "being blocked"),
      ADD_STAT(storePrepareIssued, statistics::units::Count::get(),
               "Buffer-mode exclusive permission prefetches issued"),
      ADD_STAT(storePrepareSpeculativeIssued, statistics::units::Count::get(),
               "Store preparation requests issued before retirement"),
      ADD_STAT(storePrepareBeforeData, statistics::units::Count::get(),
               "Store preparation requests issued before split data arrived"),
      ADD_STAT(storePrepareSquashed, statistics::units::Count::get(),
               "Squashed stores that had issued a preparation request"),
      ADD_STAT(storePrepareBlocked, statistics::units::Count::get(),
               "Buffer-mode permission prefetches rejected for retry"),
      ADD_STAT(storeOverwriteElisions, statistics::units::Count::get(),
               "Committed stores elided by a consecutive full overwrite"),
      ADD_STAT(storeSameLineReadyPairs, statistics::units::Count::get(),
               "Adjacent committed ordinary stores to one line at send time"),
      ADD_STAT(storeSameLineConcurrentIssued, statistics::units::Count::get(),
               "TSO stores sent while an older same-line store is in flight"),
      ADD_STAT(storeOrderedCrossLineIssued, statistics::units::Count::get(),
               "Ordered TSO stores sent with a prior different line in flight"),
      ADD_STAT(loadToUse, statistics::units::Cycle::get(),
               "Distribution of cycle latency between the "
               "first time a load is issued and its completion"),
      ADD_STAT(addedLoadsAndStores, statistics::units::Count::get(),
               "Number of loads and stores written to the Load Store Queue"),
      ADD_STAT(lqAvgOccupancy, statistics::units::Ratio::get(),
               "Average LQ Occupancy (UsedSlots/TotalSlots)"),
      ADD_STAT(sqAvgOccupancy, statistics::units::Ratio::get(),
               "Average SQ Occupancy (UsedSlots/TotalSlots)")
{
    loadToUse
        .init(0, 299, 10)
        .flags(statistics::nozero);

    lqAvgOccupancy.precision(2);

    sqAvgOccupancy.precision(2);
}

void
LSQUnit::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

void
LSQUnit::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.capacity(); ++i)
        assert(!loadQueue[i].valid());

    assert(storesToWB == 0);
    assert(tsoStoresInFlight == 0);
    assert(!storeInFlight);
    assert(tsoLinesInFlight.empty());
    assert(committedStoreBuffer.empty());
    assert(committedStores == 0);
    assert(issuedStoreBufferEntries == 0);
    assert(storeBufferLinesInFlight.empty());
    assert(selectiveBlockedLoads.empty());
    assert(partialForwarding.empty());
    assert(fullForwardingSource.empty());
    assert(!fsfp || fsfp->fastForwarding.empty());
    assert(capacityReadLines.empty());
    assert(capacityReadLineCounts.empty());
    assert(!retryPkt);
}

void
LSQUnit::takeOverFrom()
{
    resetState();
}

void
LSQUnit::insert(const DynInstPtr &inst)
{
    assert(inst->isMemRef());

    assert(inst->isLoad() || inst->isStore() || inst->isAtomic());

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    inst->setInLSQ();
}

void
LSQUnit::insertLoad(const DynInstPtr &load_inst)
{
    assert(!loadQueue.full());
    assert(loadQueue.size() < loadQueue.capacity());
    ++stats.addedLoadsAndStores;

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadQueue.tail(), load_inst->seqNum);

    /* Grow the queue. */
    loadQueue.advance_tail();

    load_inst->sqIt = storeQueue.end();

    assert(!loadQueue.back().valid());
    loadQueue.back().set(load_inst);
    load_inst->lqIdx = loadQueue.tail();
    assert(load_inst->lqIdx > 0);
    load_inst->lqIt = loadQueue.getIterator(load_inst->lqIdx);

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);

    // hardware transactional memory
    // transactional state and nesting depth must be tracked
    // in the in-order part of the core.
    if (load_inst->isHtmStart()) {
        htmStarts++;
        DPRINTF(HtmCpu, ">> htmStarts++ (%d) : htmStops (%d)\n",
                htmStarts, htmStops);

        const int htm_depth = htmStarts - htmStops;
        const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
        auto htm_uid = htm_cpt->getHtmUid();

        // for debugging purposes
        if (!load_inst->inHtmTransactionalState()) {
            htm_uid = htm_cpt->newHtmUid();
            DPRINTF(HtmCpu, "generating new htmUid=%u\n", htm_uid);
            if (htm_depth != 1) {
                DPRINTF(HtmCpu,
                    "unusual HTM transactional depth (%d)"
                    " possibly caused by mispeculation - htmUid=%u\n",
                    htm_depth, htm_uid);
            }
        }
        load_inst->setHtmTransactionalState(htm_uid, htm_depth);
    }

    if (load_inst->isHtmStop()) {
        htmStops++;
        DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops++ (%d)\n",
                htmStarts, htmStops);

        if (htmStops==1 && htmStarts==0) {
            DPRINTF(HtmCpu,
            "htmStops==1 && htmStarts==0. "
            "This generally shouldn't happen "
            "(unless due to misspeculation)\n");
        }
    }
}

void
LSQUnit::insertStore(const DynInstPtr& store_inst)
{
    // Make sure it is not full before inserting an instruction.
    assert(!storeQueue.full());
    assert(storeQueue.size() < storeQueue.capacity());
    ++stats.addedLoadsAndStores;

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeQueue.tail(), store_inst->seqNum);
    storeQueue.advance_tail();

    store_inst->sqIdx = storeQueue.tail();
    store_inst->sqIt = storeQueue.getIterator(store_inst->sqIdx);

    store_inst->lqIdx = loadQueue.tail() + 1;
    assert(store_inst->lqIdx > 0);
    store_inst->lqIt = loadQueue.end();

    storeQueue.back().set(store_inst);

    stats.sqAvgOccupancy = queueOccupancy(storeQueue);
}

void
LSQUnit::insertStoreData(const DynInstPtr &inst)
{
    auto it = storeQueue.end();
    while (it != storeQueue.begin()) {
        --it;
        const auto &address = it->instruction();
        if (!address->isStoreAddress() || it->dataInstruction())
            continue;
        panic_if(address->macroop != inst->macroop ||
                 address->pcState().instAddr() != inst->pcState().instAddr(),
                 "Store data does not match the pending store address");
        it->dataInstruction() = inst;
        inst->sqIdx = address->sqIdx;
        inst->sqIt = address->sqIt;
        return;
    }
    panic("Store data dispatched without a matching store address");
}

Fault
LSQUnit::writeStoreData(const DynInstPtr &inst, uint8_t *data, unsigned size)
{
    auto &entry = storeQueue[inst->sqIdx];
    panic_if(!entry.valid() || entry.dataInstruction() != inst ||
             !entry.instruction()->isStoreAddress(),
             "Store data writes a stale SQ entry");
    panic_if(size > SQEntry::DataSize ||
             (entry.addressReady() && entry.size() != size),
             "Store address and data sizes disagree");
    std::memcpy(entry.data(), data, size);
    if (!entry.dataReady() && !entry.addressReady())
        ++stats.splitStoreDataFirst;
    entry.dataBytes() = size;
    entry.dataReady() = true;
    wakeSelectiveBlockedLoads(entry.instruction()->seqNum, true);
    return NoFault;
}

DynInstPtr
LSQUnit::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

unsigned
LSQUnit::numFreeLoadEntries()
{
        DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n",
                loadQueue.capacity(), loadQueue.size());
        return loadQueue.capacity() - loadQueue.size();
}

unsigned
LSQUnit::numFreeStoreEntries()
{
    DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n",
            storeQueue.capacity(), storeQueue.size());
    const size_t used = storeQueue.size() + committedStoreCount();
    assert(used <= storeQueue.capacity());
    return storeQueue.capacity() - used;
}

void
LSQUnit::sampleStoreOccupancy()
{
    const auto used = storeQueue.size() + committedStoreCount();
    assert(used <= storeQueue.capacity());
    stats.storeBufferEntryCycles += committedStoreCount();
    stats.storeBufferLineEntryCycles += committedStoreBuffer.size();
    stats.sharedStoreEntryCycles += used;
    if (used == storeQueue.capacity())
        ++stats.sharedStoreFullCycles;
}

unsigned
LSQUnit::demandLoadStages() const
{
    unsigned stages = 0;
    for (const auto &entry : loadQueue) {
        const auto &inst = entry.instruction();
        auto *request = inst->savedRequest;
        if (inst->isSquashed() || inst->isExecuted() || inst->isDataPrefetch() ||
            inst->fault != NoFault || !request || request->isReleased())
            continue;
        if (!request->isTranslationComplete()) stages |= 1;
        else if (request->_numOutstandingPackets) stages |= 4;
        else if (request->isComplete()) stages |= 8;
        else stages |= 2;
    }
    return stages;
}

void
LSQUnit::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    const auto origin = pkt->req->getExtension<CacheEvictionOrigin>();
    const bool local_capacity = origin && origin->localCapacity;
    if (local_capacity)
        ++stats.localCapacityEvictions;
    else
        ++stats.coherenceInvalidations;
    DPRINTF(LSQUnit, "Got snoop for address %#x local capacity %d\n",
            pkt->getAddr(), local_capacity);

    const Addr snoop_line = pkt->getAddr() & cacheBlockMask;
    for (auto &entry : committedStoreBuffer) {
        if (entry.issued && !entry.completed && entry.line == snoop_line)
            entry.invalidated = true;
    }

    for (int x = 0; x < cpu->numContexts(); x++) {
        gem5::ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        tc->getIsaPtr()->handleLockedSnoop(pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    if (loadQueue.empty())
        return;

    auto iter = loadQueue.begin();

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

    DynInstPtr ld_inst = iter->instruction();
    assert(ld_inst);
    LSQRequest *request = iter->request();

    // Check that this snoop didn't just invalidate our lock flag
    if (ld_inst->effAddrValid() && request &&
        request->isCacheBlockHit(invalidate_addr, cacheBlockMask)
        && ld_inst->memReqFlags & Request::LLSC) {
        ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(ld_inst.get());
    }

    // The directory can forget a replaced private copy. Keep a bounded
    // observation for each affected live read, rather than replaying on
    // replacement itself. Real Ruby writes notify these observations.
    const bool local_read_loss = sprMemoryDependence && local_capacity;
    bool older_read_pending = !ld_inst->isExecuted();
    bool force_squash = false;

    while (++iter != loadQueue.end()) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        request = iter->request();
        const bool preserve_read_observation = older_read_pending;
        older_read_pending |= !ld_inst->isExecuted();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered() || !request)
            continue;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n",
                    ld_inst->seqNum, invalidate_addr);

        if (force_squash ||
            request->isCacheBlockHit(invalidate_addr, cacheBlockMask)) {
            if (local_read_loss) {
                if (preserve_read_observation)
                    rememberCapacityRead(ld_inst->seqNum, invalidate_addr);
                continue;
            }
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
                ld_inst->replayCause = RecoveryCause::Snoop;
                request->setStateToFault();
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC) {
                    ld_inst->tcBase()->getIsaPtr()->
                        handleLockedSnoopHit(ld_inst.get());
                }

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
    }
    return;
}

void
LSQUnit::rememberCapacityRead(InstSeqNum sequence, Addr line)
{
    if (!capacityReadLines[sequence].insert(line).second)
        return;
    if (capacityReadLineCounts[line]++ == 0)
        cpu->system->capacityReadObservations.subscribe(line, this);
    ++stats.capacityReadRegistrations;
}

void
LSQUnit::releaseCapacityRead(InstSeqNum sequence)
{
    auto found = capacityReadLines.find(sequence);
    if (found == capacityReadLines.end())
        return;
    for (const Addr line : found->second) {
        auto count = capacityReadLineCounts.find(line);
        assert(count != capacityReadLineCounts.end() && count->second);
        if (--count->second == 0) {
            cpu->system->capacityReadObservations.unsubscribe(line, this);
            capacityReadLineCounts.erase(count);
        }
    }
    capacityReadLines.erase(found);
}

void
LSQUnit::notifyCapacityReadWrite(Addr line, const RequestPtr &writer)
{
    // Own-thread buffered stores retain their LSQ byte-provenance checks.
    if (writer->hasContextId() &&
        writer->contextId() == cpu->tcBase(lsqID)->contextId())
        return;
    ++stats.capacityReadWrites;
    DPRINTF(LSQUnit, "Observed real write after capacity loss at %#x\n", line);
    auto request = std::make_shared<Request>(
        line, cpu->cacheLineSize(), 0, Request::funcRequestorId);
    Packet packet(request, MemCmd::InvalidateReq);
    checkSnoop(&packet);
}

Fault
LSQUnit::checkViolations(typename LoadQueue::iterator& loadIt,
        const DynInstPtr& inst)
{
    // Store/load ordering uses complete physical bytes on SPR. The generic
    // load/load snoop policy retains its independently configured granularity.
    const bool physical = sprMemoryDependence && !inst->isLoad();
    std::vector<Addr> storeBytes;
    if (physical && (!inst->savedRequest ||
        !inst->savedRequest->physicalBytes(storeBytes)))
        return NoFault;
    const unsigned shift = depCheckShift;
    Addr inst_eff_addr1 = inst->effAddr >> shift;
    Addr inst_eff_addr2 = (inst->effAddr + inst->effSize - 1) >> shift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (loadIt != loadQueue.end()) {
        DynInstPtr ld_inst = loadIt->instruction();
        // FSFP owns validation/recovery for its predicted loads. In
        // particular a correct prediction must not be charged as an MDP
        // violation when its supplying store finally resolves its address.
        if (fsfp && !inst->isLoad() &&
            fsfp->fastForwarding.count(ld_inst->seqNum)) {
            ++loadIt;
            continue;
        }
        auto full = fullForwardingSource.find(ld_inst->seqNum);
        if (!inst->isLoad() && full != fullForwardingSource.end() &&
            full->second >= inst->seqNum) {
            ++loadIt;
            continue;
        }
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            ++loadIt;
            continue;
        }

        Addr ld_eff_addr1 = ld_inst->effAddr >> shift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> shift;

        bool overlap = inst_eff_addr2 >= ld_eff_addr1 &&
                       inst_eff_addr1 <= ld_eff_addr2;
        if (physical) {
            const auto *loadBytes = iewStage->instQueue.memoryDependence(
                lsqID).physicalReadBytes(ld_inst);
            if (!loadBytes) {
                ++loadIt;
                continue;
            }
            auto *request = loadIt->request();
            if (request && request->mainReq()->isSecure() !=
                    inst->savedRequest->mainReq()->isSecure()) {
                ++loadIt;
                continue;
            }
            const bool supplied = ld_inst->isExecuted() ||
                fullForwardingSource.count(ld_inst->seqNum) ||
                partialForwarding.count(ld_inst->seqNum) ||
                (request && (request->_numOutstandingPackets ||
                             request->hasReturnedData()));
            // Returned cache data has already sampled memory, even while its
            // register result waits for the shared completion resource. Split
            // loads retain this responsibility after any fragment responds.
            // A translated load parked in LSU has not consumed a stale value.
            if (!supplied) {
                ++loadIt;
                continue;
            }
            overlap = PhysicalByteCoverage::intersects(*loadBytes, storeBytes);
        }
        if (overlap) {
            auto partial = partialForwarding.find(ld_inst->seqNum);
            if (!inst->isLoad() && partial != partialForwarding.end()) {
                bool shadowed = true;
                for (unsigned byte = 0; byte < ld_inst->effSize; ++byte) {
                    const Addr addr = physical ?
                        partial->second.physicalStart + byte :
                        ld_inst->effAddr + byte;
                    const bool intersects = physical ?
                        std::binary_search(storeBytes.begin(), storeBytes.end(), addr) :
                        addr >= inst->effAddr && addr < inst->effAddr + inst->effSize;
                    if (intersects &&
                        (!partial->second.mask[byte] ||
                         partial->second.sourceSequence[byte] < inst->seqNum))
                        shadowed = false;
                }
                if (shadowed) {
                    ++loadIt;
                    continue;
                }
            }
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                            ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;
                        ld_inst->replayCause = RecoveryCause::Snoop;

                        ++stats.memOrderViolation;

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // Otherwise, mark the load has a possible load violation and
                // if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
                        " between instructions [sn:%lli] and [sn:%lli]\n",
                        inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                DPRINTF(MemoryOrdering,
                    "store-sn=%llu store-pc=%s store-va=%#llx store-size=%u "
                    "load-sn=%llu load-pc=%s load-va=%#llx load-size=%u "
                    "executed=%u outstanding=%u returned=%u "
                    "full-forward=%u full-source=%llu "
                    "partial-forward=%u\n",
                    inst->seqNum, inst->pcState(), inst->effAddr,
                    inst->effSize, ld_inst->seqNum, ld_inst->pcState(),
                    ld_inst->effAddr, ld_inst->effSize, ld_inst->isExecuted(),
                    loadIt->request() ?
                        loadIt->request()->_numOutstandingPackets : 0,
                    loadIt->request() &&
                        loadIt->request()->hasReturnedData(),
                    fullForwardingSource.count(ld_inst->seqNum) != 0,
                    full != fullForwardingSource.end() ? full->second : 0,
                    partialForwarding.count(ld_inst->seqNum) != 0);
                memDepViolator = ld_inst;

                ++stats.memOrderViolation;
                if (physical) {
                    if (ld_inst->isExecuted())
                        ++stats.storeOrderCandidateExecuted;
                    else if (fullForwardingSource.count(ld_inst->seqNum) ||
                             partialForwarding.count(ld_inst->seqNum))
                        ++stats.storeOrderCandidateForwarded;
                    else if (loadIt->request() &&
                             loadIt->request()->hasReturnedData())
                        ++stats.storeOrderCandidateReturned;
                    else
                        ++stats.storeOrderCandidateOutstanding;
                }
                if (!ld_inst->isExecuted() && loadIt->request() &&
                    loadIt->request()->hasReturnedData())
                    ++stats.returnedLoadOrderViolations;

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }

        ++loadIt;
    }
    return NoFault;
}




void
LSQUnit::noteSplitTranslation(const std::vector<RequestPtr> &fragments)
{
    for (size_t i = 1; i < fragments.size(); ++i) {
        const auto &previous = fragments[i - 1];
        const auto &next = fragments[i];
        if (next->getPaddr() != previous->getPaddr() +
            next->getVaddr() - previous->getVaddr()) {
            ++stats.noncontiguousTranslations;
            break;
        }
    }
}

Fault
LSQUnit::executeLoad(const DynInstPtr &inst)
{
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    if (inst->isExecuted()) {
        DPRINTF(LSQUnit, "Load [sn:%lli] already executed\n", inst->seqNum);
        return NoFault;
    }

    load_fault = inst->initiateAcc();

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        assert(inst->readPredicate());
        inst->setExecuted();
        inst->completeAcc(nullptr);
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return NoFault;
    }

    if (inst->isTranslationDelayed() && load_fault == NoFault)
        return load_fault;

    if (load_fault != NoFault && inst->translationCompleted() &&
            inst->savedRequest->isPartialFault()
            && !inst->savedRequest->isComplete()) {
        assert(inst->savedRequest->isSplit());
        // If we have a partial fault where the mem access is not complete yet
        // then the cache must have been blocked. This load will be re-executed
        // when the cache gets unblocked. We will handle the fault when the
        // mem access is complete.
        return NoFault;
    }

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
    } else {
        if (fsfp)
            validateFastForwarding();
        if (inst->effAddrValid()) {
            iewStage->instQueue.memoryDependence(lsqID).addressReady(inst);
            auto it = inst->lqIt;
            ++it;

            if (checkLoads)
                return checkViolations(it, inst);
        }
    }

    return load_fault;
}

Fault
LSQUnit::executeStore(const DynInstPtr &store_inst)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);

    ssize_t store_idx = store_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli]\n",
            store_inst->pcState(), store_inst->seqNum);

    assert(!store_inst->isSquashed());

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = store_inst->lqIt;

    Fault store_fault = store_inst->initiateAcc();

    if (store_inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    if (storeQueue[store_idx].size() == 0) {
        DPRINTF(LSQUnit,"Fault on Store PC %s, [sn:%lli], Size = 0\n",
                store_inst->pcState(), store_inst->seqNum);

        if (store_inst->isAtomic()) {
            // If the instruction faulted, then we need to send it along
            // to commit without the instruction completing.
            if (!(store_inst->hasRequest() && store_inst->strictlyOrdered()) ||
                store_inst->isAtCommit()) {
                store_inst->setExecuted();
            }
            iewStage->instToCommit(store_inst);
            iewStage->activityThisCycle();
        }

        return store_fault;
    }

    assert(store_fault == NoFault);

    // Younger instructions can already have been decoded while this store's
    // address was unresolved. Mark same-line code for precise re-execution;
    // the store's own remaining fused members must stay intact.
    if (storeQueue[store_idx].hasRequest()) {
        for (const auto &request : storeQueue[store_idx].request()->_reqs) {
            if (!request || !request->hasPaddr() || !request->getSize())
                continue;
            const Addr first = request->getPaddr() & cacheBlockMask;
            const Addr last = (request->getPaddr() + request->getSize() - 1) &
                              cacheBlockMask;
            for (Addr block = first;; block += cacheLineSize()) {
                cpu->noteCodeWrite(store_inst->threadNumber,
                                   store_inst->retirementEnd(), block);
                if (block == last) break;
            }
        }
    }

    if (store_inst->isStoreConditional() || store_inst->isAtomic()) {
        // Store conditionals and Atomics need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB() = true;

        ++storesToWB;
    }

    iewStage->instQueue.memoryDependence(lsqID).addressReady(store_inst);
    wakeSelectiveBlockedLoads(store_inst->seqNum, false, true);
    if (fsfp)
        validateFastForwarding();

    const Fault ordering_fault = checkViolations(loadIt, store_inst);
    iewStage->instQueue.memoryDependence(lsqID).storeOrderChecked(
        store_inst, ordering_fault == NoFault ? 0 : memDepViolator->seqNum);
    return ordering_fault;

}

bool
LSQUnit::hasPendingCodeWrite(Addr physical_block)
{
    const Addr block = physical_block & cacheBlockMask;
    for (auto &entry : storeQueue) {
        if (!entry.valid() || entry.completed() || !entry.hasRequest())
            continue;
        for (const auto &request : entry.request()->_reqs) {
            if (!request || !request->hasPaddr() || !request->getSize())
                continue;
            const Addr first = request->getPaddr() & cacheBlockMask;
            const Addr last = (request->getPaddr() + request->getSize() - 1) &
                              cacheBlockMask;
            if (first <= block && block <= last)
                return true;
        }
    }
    for (const auto &entry : committedStoreBuffer) {
        if (!entry.completed && entry.line == block)
            return true;
    }
    return false;
}

void
LSQUnit::commitLoad()
{
    assert(loadQueue.front().valid());

    DynInstPtr inst = loadQueue.front().instruction();
    releaseCapacityRead(inst->seqNum);
    if (!inst->isSquashed() && (inst->memReqFlags & Request::REP_BULK)) {
        ++stats.repReadBatches;
        stats.repReadBytes += inst->effSize;
    }
    if (fsfp) {
        auto predicted = fsfp->fastForwarding.find(inst->seqNum);
        panic_if(predicted != fsfp->fastForwarding.end() &&
                 (!predicted->second.validated || predicted->second.failed),
                 "FSFP load retired without successful validation");
        trainForwarding(inst);
        fsfp->fastForwarding.erase(inst->seqNum);
    }
    iewStage->instQueue.memoryDependence(lsqID).loadRetired(inst);
    partialForwarding.erase(inst->seqNum);
    fullForwardingSource.erase(inst->seqNum);

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s\n",
            inst->pcState());

    // Update histogram with memory latency from load
    // Only take latency from load demand that where issued and did not fault
    if (!inst->isInstPrefetch() && !inst->isDataPrefetch()
            && inst->firstIssue != -1
            && inst->lastWakeDependents != -1) {
        stats.loadToUse.sample(cpu->ticksToCycles(
                    inst->lastWakeDependents - inst->firstIssue));
    }

    loadQueue.front().clear();
    loadQueue.pop_front();

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);
}

void
LSQUnit::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loadQueue.size() == 0 || loadQueue.front().valid());

    while (loadQueue.size() != 0 && loadQueue.front().instruction()->seqNum
            <= youngest_inst) {
        commitLoad();
    }
}

void
LSQUnit::commitStores(InstSeqNum &youngest_inst)
{
    assert(storeQueue.size() == 0 || storeQueue.front().valid());

    /* Forward iterate the store queue (age order). */
    for (auto& x : storeQueue) {
        assert(x.valid());
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!x.canWB()) {
            InstSeqNum publication_seq = x.instruction()->seqNum;
            if (x.instruction()->isStoreAddress()) {
                if (!x.dataInstruction())
                    break;
                publication_seq = x.dataInstruction()->seqNum;
            }
            if (publication_seq > youngest_inst) {
                break;
            }
            panic_if(x.instruction()->isStoreAddress() && x.size() != 0 &&
                     (!x.addressReady() || !x.dataReady()),
                     "Retired split store lacks address or data");
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(),
                    x.instruction()->seqNum);

            x.canWB() = true;

            if (x.instruction()->memReqFlags & Request::REP_BULK) {
                ++stats.repWriteBatches;
                stats.repWriteBytes += x.size();
            }

            ++storesToWB;
        }
    }
}

void
LSQUnit::writebackBlockedStore()
{
    assert(isStoreBlocked);
    storeWBIt->request()->sendPacketToCache();
    if (storeWBIt->request()->isSent()){
        storePostSend();
    }
}

bool
LSQUnit::isOrdinaryStore(StoreQueue::iterator entry)
{
    if (!entry.dereferenceable() || !entry->valid() ||
        !entry->size() || !entry->hasRequest() || entry->isAllZeros())
        return false;
    const auto &inst = entry->instruction();
    auto *request = entry->request();
    if (!inst->isStore() || inst->isStoreConditional() || inst->isAtomic() ||
        inst->isDataPrefetch() || inst->inHtmTransactionalState() ||
        inst->isReadBarrier() || inst->isWriteBarrier() || request->isSplit())
        return false;
    const auto req = request->mainReq();
    // A locked x86 RMW store may not carry the DynInst atomic flag. Its
    // request flags still require Ruby's paired locked read/write protocol;
    // a synthetic masked line write cannot replace that original request.
    return req->hasPaddr() && req->hasVaddr() && !req->hasNoAddr() &&
        !req->isMasked() && !req->isLLSC() && !req->isAtomic() &&
        !req->isReadModifyWrite() &&
        !req->isSwap() && !req->isMemMgmt() && !req->isRelease() &&
        !req->isLocalAccess() &&
        !req->isUncacheable() && !req->isStrictlyOrdered() &&
        !req->isCacheMaintenance() && !(req->getFlags() & Request::STORE_NO_DATA);
}

bool
LSQUnit::isCoalescibleStore(StoreQueue::iterator entry)
{
    return entry.dereferenceable() && entry->canWB() && isOrdinaryStore(entry);
}

void
LSQUnit::offloadCommittedStores()
{
    if (fsfp)
        validateFastForwarding();
    unsigned offloaded = 0;
    while (offloaded < 4 && storesToWB > 0 &&
           storeWBIt.dereferenceable() && storeWBIt == storeQueue.begin() &&
           isCoalescibleStore(storeWBIt) && !storeWBIt->committed() &&
           !storeWBIt->request()->isAnyOutstandingRequest()) {
        auto *request = storeWBIt->request();
        const auto req = request->mainReq();
        const auto inst = storeWBIt->instruction();
        const unsigned line_size = cpu->cacheLineSize();
        const Addr line = req->getPaddr() & cacheBlockMask;
        const unsigned offset = req->getPaddr() - line;
        const unsigned size = req->getSize();
        panic_if(offset + size > line_size || !req->hasContextId(),
                 "Invalid committed store buffer request");

        CommittedStoreEntry *target = nullptr;
        if (!committedStoreBuffer.empty()) {
            auto &tail = committedStoreBuffer.back();
            if (!tail.issued && tail.line == line &&
                tail.context == req->contextId() &&
                tail.requestor == req->requestorId() &&
                tail.flags == req->getFlags()) {
                target = &tail;
                target->lastSeq = inst->seqNum;
                ++target->logicalStores;
                ++stats.storeBufferMerged;
            }
        }
        if (!target) {
            committedStoreBuffer.push_back({
                nextStoreBufferId++, line, req->contextId(), req->requestorId(),
                req->getFlags(), req->hasPC() ? req->getPC() : 0,
                inst->seqNum, inst->seqNum,
                std::vector<uint8_t>(line_size, 0),
                std::vector<bool>(line_size, false),
                std::vector<Addr>(line_size, 0),
                std::vector<InstSeqNum>(line_size, 0),
                std::vector<Addr>(fsfp ? line_size : 0, 0),
                std::vector<bool>(fsfp ? line_size : 0, false)});
            target = &committedStoreBuffer.back();
        }
        ++committedStores;
        // Translation-sync ownership follows the store when it leaves the SQ.
        target->staleTranslation |= request->hasStaleTranslation();
        for (unsigned byte = 0; byte < size; ++byte) {
            target->data[offset + byte] = storeWBIt->isAllZeros() ?
                0 : static_cast<uint8_t>(storeWBIt->data()[byte]);
            target->mask[offset + byte] = true;
            target->sourceStart[offset + byte] = req->getPaddr();
            target->sourceSequence[offset + byte] = inst->seqNum;
            if (fsfp) {
                target->sourcePC[offset + byte] = inst->pcState().instAddr();
                target->sourceFastForwardingAddress[offset + byte] =
                    inst->staticInst->hasFastForwardingAddress();
            }
        }

        // The original retired store has been absorbed into the line request;
        // the synthetic packet has its own Ruby transaction identity.
        request->noteNativeTerminal(TaoTraceNativeTerminalReason::StoreBufferCombined);
        completeStore(storeWBIt++);
        ++stats.storeBufferOffloaded;
        ++offloaded;
    }
}

unsigned
LSQUnit::committedStoreCount() const
{
    return committedStores;
}

void
LSQUnit::drainCommittedStoreBuffer()
{
    while (issuedStoreBufferEntries < committedStoreBuffer.size()) {
        auto &entry = committedStoreBuffer[issuedStoreBufferEntries];
        assert(!entry.issued);
        // The configured window bounds distinct accepted write lines.
        // Multiple in-flight entries for an existing line share
        // one credit. Completion removes the line only on its last response.
        if (sprStoreDrainLineCredits &&
            storeBufferLinesInFlight.size() >= sprStoreDrainLineCredits &&
            storeBufferLinesInFlight.find(entry.line) ==
                storeBufferLinesInFlight.end()) {
            ++stats.storeDrainLineCreditBlocked;
            return;
        }
        const unsigned service_bytes = std::count(
            entry.mask.begin(), entry.mask.end(), true);
        if (lsq->cacheBlocked()) {
            ++stats.storeDrainCacheBlocked;
            return;
        }
        if (!lsq->cachePortAvailable(false, service_bytes)) {
            ++stats.storeDrainPortBlocked;
            return;
        }
        auto req = std::make_shared<Request>(
            entry.line, entry.data.size(), entry.flags, entry.requestor);
        req->setContext(entry.context);
        req->setPC(entry.pc);
        req->setByteEnable(entry.mask);
        auto ordered = std::make_shared<TSOOrderedStore>();
        ordered->logicalStores = entry.logicalStores;
        req->setExtension(ordered);
        PacketPtr pkt = Packet::createWrite(req);
        auto *data = new uint8_t[entry.data.size()];
        std::memcpy(data, entry.data.data(), entry.data.size());
        pkt->dataDynamic(data);
        pkt->senderState = new LSQ::StoreBufferSenderState(lsqID, entry.id);
        if (!dcachePort->sendTimingReq(pkt)) {
            delete pkt->popSenderState();
            delete pkt;
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
            return;
        }
        fatal_if(!ordered->acceptedBySequencer,
                 "Committed TSO store buffer requires MESI_Three_Level Ruby");
        entry.issued = true;
        ++issuedStoreBufferEntries;
        ++storeBufferLinesInFlight[entry.line];
        lsq->cachePortBusy(false, service_bytes);
        ++stats.storeBufferSent;
        stats.storeBufferLogicalSent += entry.logicalStores;
    }
}

void
LSQUnit::completeStoreBufferWrite(uint64_t entry_id)
{
    auto it = std::find_if(committedStoreBuffer.begin(), committedStoreBuffer.end(),
        [entry_id](const auto &entry) { return entry.id == entry_id; });
    panic_if(it == committedStoreBuffer.end() || !it->issued || it->completed,
             "Invalid committed store buffer response");
    it->completed = true;
    assert(committedStores >= it->logicalStores);
    committedStores -= it->logicalStores;
    it->staleTranslation = false;
    auto line_it = storeBufferLinesInFlight.find(it->line);
    assert(line_it != storeBufferLinesInFlight.end() && line_it->second > 0);
    if (--line_it->second == 0)
        storeBufferLinesInFlight.erase(line_it);
    std::vector<SelectiveBlockedLoad> ready;
    for (auto wait = selectiveBlockedLoads.begin();
         wait != selectiveBlockedLoads.end();) {
        if (wait->second.storeSeq < it->firstSeq ||
            wait->second.storeSeq > it->lastSeq) {
            ++wait;
            continue;
        }
        ready.push_back(wait->second);
        wait = selectiveBlockedLoads.erase(wait);
    }
    if (stalled && stallingStoreIsn >= it->firstSeq &&
        stallingStoreIsn <= it->lastSeq) {
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }
    ++stats.storeBufferCompleted;
    stats.storeBufferLogicalCompleted += it->logicalStores;
    while (!committedStoreBuffer.empty() &&
           committedStoreBuffer.front().completed) {
        assert(issuedStoreBufferEntries);
        --issuedStoreBufferEntries;
        committedStoreBuffer.pop_front();
    }
    for (const auto &wait : ready)
        resumeSelectiveBlockedLoad(wait);
    iewStage->updateLSQNextCycle = true;
    cpu->wakeCPU();
    cpu->activityThisCycle();
}

void
LSQUnit::wakeSelectiveBlockedLoads(InstSeqNum store_seq, bool data_only,
                                  bool address_only)
{
    std::vector<SelectiveBlockedLoad> ready;
    for (auto it = selectiveBlockedLoads.begin();
         it != selectiveBlockedLoads.end();) {
        if (it->second.storeSeq != store_seq ||
            (data_only && !it->second.waitForData) ||
            (address_only && !it->second.waitForAddress)) {
            ++it;
            continue;
        }
        ready.push_back(it->second);
        it = selectiveBlockedLoads.erase(it);
    }
    for (const auto &wait : ready)
        resumeSelectiveBlockedLoad(wait);
}

void
LSQUnit::rescheduleRetainedLoad(const DynInstPtr &load)
{
    auto &entry = loadQueue[load->lqIdx];
    auto *request = entry.request();
    assert(entry.valid() && entry.instruction() == load && request);
    iewStage->rescheduleMemInst(load);
    load->clearIssued();
    load->effAddrValid(false);
    ++stats.rescheduledLoads;
    request->discard();
    entry.setRequest(nullptr);
    iewStage->replayOneMemInst(load);
}

bool
LSQUnit::canRetainLoadRequest(LSQRequest *request) const
{
    const auto req = request->mainReq();
    const auto &load = request->_inst;
    return needsTSO && !cpu->checker && !request->hasStaleTranslation() &&
        !request->isAnyOutstandingRequest() && !request->isPartialFault() &&
        !req->isLLSC() && !req->isReadModifyWrite() && !req->isAcquire() &&
        !req->isLocalAccess() && !req->isUncacheable() &&
        !req->isStrictlyOrdered() && !req->isCacheMaintenance() &&
        !req->isMemMgmt() && !(req->getFlags() & Request::NO_ACCESS) &&
        !load->isAtomic() && !load->isDataPrefetch() &&
        !load->isReadBarrier() && !load->isWriteBarrier() &&
        !load->inHtmTransactionalState();
}

void
LSQUnit::resumeSelectiveBlockedLoad(const SelectiveBlockedLoad &wait)
{
    const auto &load = wait.load;
    if (load->isSquashed())
        return;
    if (!wait.retainRequest) {
        if (iewStage->replayOneMemInst(load))
            ++stats.selectivePartialWakeups;
        return;
    }
    auto &entry = loadQueue[load->lqIdx];
    auto *request = entry.request();
    assert(entry.valid() && entry.instruction() == load && request);
    if (request->hasStaleTranslation() || !load->effAddrValid()) {
        rescheduleRetainedLoad(load);
        return;
    }
    ++stats.retainedStoreDataResumes;
    ++stats.selectivePartialWakeups;
    const Fault fault = read(request, load->lqIdx);
    if (fault != NoFault) {
        load->fault = fault;
        load->setExecuted();
        iewStage->instToCommit(load);
    }
    cpu->wakeCPU();
    iewStage->activityThisCycle();
}

bool
LSQUnit::canSendTSOStore(StoreQueue::iterator entry)
{
    if (!needsTSO || !storeInFlight)
        return true;
    if (cpu->checker || !tsoInFlightSameLineEligible ||
        !isCoalescibleStore(entry) || entry->committed())
        return false;
    const auto req = entry->request()->mainReq();
    if (req->isSecure() != tsoInFlightSecure)
        return false;
    // Port, cache admission and ordered visibility own the remaining limits.
    return true;
}

bool
LSQUnit::canElideOverwrittenStore(StoreQueue::iterator older,
                                  StoreQueue::iterator younger)
{
    if (cpu->checker || !isCoalescibleStore(older) ||
        !isCoalescibleStore(younger) || older->committed() ||
        younger->committed() || older->request()->isAnyOutstandingRequest() ||
        younger->request()->isAnyOutstandingRequest())
        return false;
    const auto first = older->request()->mainReq();
    const auto second = younger->request()->mainReq();
    return first->getPaddr() == second->getPaddr() &&
           first->getVaddr() == second->getVaddr() &&
           first->getSize() == second->getSize() &&
           first->isSecure() == second->isSecure();
}

void
LSQUnit::writebackStores()
{
    if (sprStoreBuffer) {
        if (isStoreBlocked)
            writebackBlockedStore();
        // Do not extend the legacy SQ stream while waiting to hand over to
        // the committed buffer. Otherwise steady traffic can starve it.
        if (isStoreBlocked || storeInFlight)
            return;
        offloadCommittedStores();
        drainCommittedStoreBuffer();
        if (!committedStoreBuffer.empty()) {
            issueStorePrepareRequests();
            return;
        }
    }

    unsigned overwrite_elisions = 0;
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Writing back  blocked store\n");
        writebackBlockedStore();
    }

    while (storesToWB > 0 &&
           storeWBIt.dereferenceable() &&
           storeWBIt->valid() &&
           storeWBIt->canWB() &&
           canSendTSOStore(storeWBIt) &&
           lsq->cachePortAvailable(false)) {

        if (isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                    " is blocked!\n");
            break;
        }

        // Both writes must already be committed and consecutive in the SQ.
        // Thus all intervening loads have retired; the younger full write is
        // still in the SQ to serve subsequent loads until its ordered send.
        // Limit this proxy to two elisions per writeback invocation.
        auto younger = storeWBIt;
        ++younger;
        if (needsTSO && overwrite_elisions < 2 &&
            canElideOverwrittenStore(storeWBIt, younger)) {
            storeWBIt->request()->noteNativeTerminal(
                TaoTraceNativeTerminalReason::StoreOverwriteElided);
            ++stats.storeOverwriteElisions;
            ++overwrite_elisions;
            completeStore(storeWBIt++);
            continue;
        }

        // Store didn't write any data so no need to write it back to
        // memory.
        if (storeWBIt->size() == 0) {
            if (storeWBIt->hasRequest()) {
                storeWBIt->request()->noteNativeTerminal(
                    TaoTraceNativeTerminalReason::ZeroSize);
            }
            /* It is important that the preincrement happens at (or before)
             * the call, as the the code of completeStore checks
             * storeWBIt. */
            completeStore(storeWBIt++);
            continue;
        }

        if (storeWBIt->instruction()->isDataPrefetch()) {
            if (storeWBIt->hasRequest()) {
                storeWBIt->request()->noteNativeTerminal(
                    TaoTraceNativeTerminalReason::PrefetchSkipped);
            }
            storeWBIt++;
            continue;
        }

        assert(storeWBIt->hasRequest());
        assert(!storeWBIt->committed());

        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest* request = storeWBIt->request();

        // Process store conditionals or store release after all previous
        // stores are completed
        if ((request->mainReq()->isLLSC() ||
             request->mainReq()->isRelease()) &&
             (storeWBIt.idx() != storeQueue.head())) {
            DPRINTF(LSQUnit, "Store idx:%i PC:%s to Addr:%#x "
                "[sn:%lli] is %s%s and not head of the queue\n",
                storeWBIt.idx(), inst->pcState(),
                request->mainReq()->getPaddr(), inst->seqNum,
                request->mainReq()->isLLSC() ? "SC" : "",
                request->mainReq()->isRelease() ? "/Release" : "");
            break;
        }

        if (needsTSO &&
            !cpu->checker &&
            isCoalescibleStore(storeWBIt)) {
            request->mainReq()->setExtension(
                std::make_shared<TSOOrderedStore>());
        }
        storeWBIt->committed() = true;

        assert(!inst->memData);
        inst->memData = new uint8_t[request->_size];

        if (storeWBIt->isAllZeros())
            memset(inst->memData, 0, request->_size);
        else
            memcpy(inst->memData, storeWBIt->data(), request->_size);

        request->buildPackets();

        DPRINTF(LSQUnit, "D-Cache: Writing back store idx:%i PC:%s "
                "to Addr:%#x, data:%#x [sn:%lli]\n",
                storeWBIt.idx(), inst->pcState(),
                request->mainReq()->getPaddr(), (int)*(inst->memData),
                inst->seqNum);

        // @todo: Remove this SC hack once the memory system handles it.
        if (inst->isStoreConditional()) {
            // Disable recording the result temporarily.  Writing to
            // misc regs normally updates the result, but this is not
            // the desired behavior when handling store conditionals.
            inst->recordResult(false);
            bool success = inst->tcBase()->getIsaPtr()->handleLockedWrite(
                    inst.get(), request->mainReq(), cacheBlockMask);
            inst->recordResult(true);
            request->packetSent();

            if (!success) {
                request->noteNativeTerminal(
                    TaoTraceNativeTerminalReason::FailedStoreConditional);
                request->complete();
                // Instantly complete this store.
                DPRINTF(LSQUnit, "Store conditional [sn:%lli] failed.  "
                        "Instantly completing it.\n",
                        inst->seqNum);
                PacketPtr new_pkt = new Packet(*request->packet());
                WritebackEvent *wb = new WritebackEvent(inst,
                        new_pkt, this);
                cpu->schedule(wb, curTick() + 1);
                completeStore(storeWBIt);
                if (!storeQueue.empty())
                    storeWBIt++;
                else
                    storeWBIt = storeQueue.end();
                continue;
            }
        }

        if (request->mainReq()->isLocalAccess()) {
            request->noteNativeTerminal(
                TaoTraceNativeTerminalReason::LocalAccess);
            assert(!inst->isStoreConditional());
            assert(!inst->inHtmTransactionalState());
            gem5::ThreadContext *thread = cpu->tcBase(lsqID);
            PacketPtr main_pkt = new Packet(request->mainReq(),
                                            MemCmd::WriteReq);
            main_pkt->dataStatic(inst->memData);
            request->mainReq()->localAccessor(thread, main_pkt);
            delete main_pkt;
            completeStore(storeWBIt);
            storeWBIt++;
            continue;
        }
        /* Send to cache */
        request->sendPacketToCache();

        /* If successful, do the post send */
        if (request->isSent()) {
            storePostSend();
        } else {
            DPRINTF(LSQUnit, "D-Cache became blocked when writing [sn:%lli], "
                    "will retry later\n",
                    inst->seqNum);
        }
    }
    assert(storesToWB >= 0);

    if (needsTSO && !isStoreBlocked) {
        issueStorePrepareRequests();
    }
}

void
LSQUnit::issueStorePrepareRequests()
{
    if (lsq->cacheBlocked()) {
        return;
    }

    for (auto store_it = storeQueue.begin();
         store_it != storeQueue.end(); ++store_it) {
        if (!store_it->valid() ||
            (!store_it->canWB() && !sprEarlyStorePrepare)) {
            break;
        }
        const DynInstPtr& inst = store_it->instruction();
        // Do not prepare across an ordering or atomic operation. A missing
        // address for an ordinary speculative store does not block younger
        // independently translated ordinary stores in this candidate.
        if (sprEarlyStorePrepare &&
            (!inst->isStore() || inst->isStoreConditional() ||
             inst->isAtomic() || inst->inHtmTransactionalState() ||
             inst->isDataPrefetch() || inst->isReadBarrier() ||
             inst->isWriteBarrier())) {
            break;
        }
        if (store_it->size() == 0 || !store_it->hasRequest()) {
            continue;
        }

        LSQRequest* request = store_it->request();
        RequestPtr main_req = request->mainReq();
        if (inst->isSquashed())
            continue;
        if (inst->fault != NoFault)
            break;
        // An independent younger ordinary store may proceed while this
        // store's translation is unresolved. Never issue on a partial or
        // stale translation, even if a physical address has been populated.
        if (!request->isTranslationComplete() || !main_req->hasPaddr() ||
            request->hasStaleTranslation())
            continue;
        if (!inst->isStore() || inst->isStoreConditional() ||
            inst->isAtomic() || inst->inHtmTransactionalState() ||
            inst->isDataPrefetch() ||
            main_req->isLLSC() || main_req->isRelease() ||
            main_req->isLocalAccess() || main_req->isUncacheable() ||
            main_req->isStrictlyOrdered() ||
            main_req->isCacheMaintenance()) {
            break;
        }
        if (request->isSplit())
            continue;
        if (sprEarlyStorePrepare &&
            (main_req->hasNoAddr() ||
             main_req->isAtomic() || main_req->isReadModifyWrite() ||
             main_req->isSwap() || main_req->isMemMgmt() ||
             main_req->isMasked() ||
             (main_req->getFlags() & Request::STORE_NO_DATA))) {
            break;
        }
        if (store_it->committed() || store_it->storePrepareIssued()) {
            continue;
        }
        if (store_it->canWB() && !sprRetiredStorePrepare) {
            continue;
        }

        Request::Flags flags = Request::STORE_PREPARE;
        if (main_req->isSecure()) {
            flags.set(Request::SECURE);
        }
        RequestPtr prepare_req = std::make_shared<Request>(
            main_req->getPaddr(), 0, flags, main_req->requestorId());
        if (main_req->hasPC()) {
            prepare_req->setPC(main_req->getPC());
        }
        if (main_req->hasContextId()) {
            prepare_req->setContext(main_req->contextId());
        }

        PacketPtr prepare_pkt = new Packet(prepare_req, MemCmd::StoreOwnershipReq);
        prepare_pkt->senderState = new LSQ::StorePrepareSenderState();

        if (!dcachePort->sendTimingReq(prepare_pkt)) {
            delete prepare_pkt->popSenderState();
            delete prepare_pkt;
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
            ++stats.storePrepareBlocked;
            break;
        }

        store_it->storePrepareIssued() = true;
        ++stats.storePrepareIssued;
        if (!store_it->canWB())
            ++stats.storePrepareSpeculativeIssued;
        if (inst->isStoreAddress() && !store_it->dataReady())
            ++stats.storePrepareBeforeData;
    }
}

void
LSQUnit::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loadQueue.size(),
            storeQueue.size());

    const auto first_squashed = selectiveBlockedLoads.upper_bound(squashed_num);
    for (auto it = first_squashed; it != selectiveBlockedLoads.end(); ++it)
        ++stats.selectivePartialSquashedWaits;
    selectiveBlockedLoads.erase(first_squashed, selectiveBlockedLoads.end());
    partialForwarding.erase(partialForwarding.upper_bound(squashed_num),
                            partialForwarding.end());
    fullForwardingSource.erase(fullForwardingSource.upper_bound(squashed_num),
                               fullForwardingSource.end());
    if (fsfp) {
        fsfp->fastForwarding.erase(fsfp->fastForwarding.upper_bound(squashed_num),
                                   fsfp->fastForwarding.end());
        fsfp->forwardingTraining.erase(
            fsfp->forwardingTraining.upper_bound(squashed_num),
            fsfp->forwardingTraining.end());
    }

    while (loadQueue.size() != 0 &&
            loadQueue.back().instruction()->seqNum > squashed_num) {
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue.back().instruction()->pcState(),
                loadQueue.back().instruction()->seqNum);

        if (stalled && loadQueue.tail() == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
        }

        // hardware transactional memory
        // Squashing instructions can alter the transaction nesting depth
        // and must be corrected before fetching resumes.
        if (loadQueue.back().instruction()->isHtmStart())
        {
            htmStarts = (--htmStarts < 0) ? 0 : htmStarts;
            DPRINTF(HtmCpu, ">> htmStarts-- (%d) : htmStops (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().instruction()->isHtmStop())
        {
            htmStops = (--htmStops < 0) ? 0 : htmStops;
            DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops-- (%d)\n",
              htmStarts, htmStops);
        }
        // Clear the smart pointer to make sure it is decremented.
        releaseCapacityRead(loadQueue.back().instruction()->seqNum);
        loadQueue.back().instruction()->setSquashed();
        loadQueue.back().clear();

        loadQueue.pop_back();
        ++stats.squashedLoads;
    }

    stats.lqAvgOccupancy = queueOccupancy(loadQueue);

    // hardware transactional memory
    // scan load queue (from oldest to youngest) for most recent valid htmUid
    auto scan_it = loadQueue.begin();
    uint64_t in_flight_uid = 0;
    while (scan_it != loadQueue.end()) {
        if (scan_it->instruction()->isHtmStart() &&
            !scan_it->instruction()->isSquashed()) {
            in_flight_uid = scan_it->instruction()->getHtmTransactionUid();
            DPRINTF(HtmCpu, "loadQueue[%d]: found valid HtmStart htmUid=%u\n",
                scan_it._idx, in_flight_uid);
        }
        scan_it++;
    }
    // If there's a HtmStart in the pipeline then use its htmUid,
    // otherwise use the most recently committed uid
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    if (htm_cpt) {
        const uint64_t old_local_htm_uid = htm_cpt->getHtmUid();
        uint64_t new_local_htm_uid;
        if (in_flight_uid > 0)
            new_local_htm_uid = in_flight_uid;
        else
            new_local_htm_uid = lastRetiredHtmUid;

        if (old_local_htm_uid != new_local_htm_uid) {
            DPRINTF(HtmCpu, "flush: lastRetiredHtmUid=%u\n",
                lastRetiredHtmUid);
            DPRINTF(HtmCpu, "flush: resetting localHtmUid=%u\n",
                new_local_htm_uid);

            htm_cpt->setHtmUid(new_local_htm_uid);
        }
    }

    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
        memDepViolator = NULL;
    }

    while (storeQueue.size() != 0 &&
           (storeQueue.back().instruction()->seqNum > squashed_num ||
            (storeQueue.back().dataInstruction() &&
             storeQueue.back().dataInstruction()->seqNum > squashed_num))) {
        // Instructions marked as can WB are already committed.
        if (storeQueue.back().canWB()) {
            break;
        }

        if (storeQueue.back().storePrepareIssued())
            ++stats.storePrepareSquashed;

        DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli] address-valid:%u address:%#llx\n",
                storeQueue.back().instruction()->pcState(),
                storeQueue.tail(), storeQueue.back().instruction()->seqNum,
                storeQueue.back().instruction()->effAddrValid(),
                storeQueue.back().instruction()->effAddr);

        // I don't think this can happen.  It should have been cleared
        // by the stalling load.
        if (stalled &&
            storeQueue.back().instruction()->seqNum == stallingStoreIsn) {
            panic("Is stalled should have been cleared by stalling load!\n");
            stalled = false;
            stallingStoreIsn = 0;
        }

        // Clear the smart pointer to make sure it is decremented.
        storeQueue.back().instruction()->setSquashed();
        if (storeQueue.back().dataInstruction())
            storeQueue.back().dataInstruction()->setSquashed();

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        storeQueue.back().clear();

        storeQueue.pop_back();
        ++stats.squashedStores;
    }
    stats.sqAvgOccupancy = queueOccupancy(storeQueue);
    cpu->mmu->squashTiming(cpu->tcBase(lsqID));
}

uint64_t
LSQUnit::getLatestHtmUid() const
{
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    return htm_cpt->getHtmUid();
}

void
LSQUnit::storePostSend()
{
    if (needsTSO && !cpu->checker)
        wakeSelectiveBlockedLoads(storeWBIt->instruction()->seqNum);
    auto younger = storeWBIt;
    ++younger;
    if (needsTSO &&
        isCoalescibleStore(storeWBIt) && isCoalescibleStore(younger) &&
        !younger->committed() &&
        !younger->request()->isAnyOutstandingRequest()) {
        const auto first = storeWBIt->request()->mainReq();
        const auto second = younger->request()->mainReq();
        if ((first->getPaddr() & cacheBlockMask) ==
                (second->getPaddr() & cacheBlockMask) &&
            first->isSecure() == second->isSecure())
            ++stats.storeSameLineReadyPairs;
    }

    if (stalled &&
        storeWBIt->instruction()->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    if (!storeWBIt->instruction()->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
        storeWBIt->instruction()->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeWBIt->instruction());
        }
    }

    if (needsTSO) {
        assert(!storeWBIt->tsoIssued());
        if (storeInFlight) {
            assert(tsoInFlightSameLineEligible);
            const Addr line = storeWBIt->request()->mainReq()->getPaddr() &
                cacheBlockMask;
            if (line != tsoLastIssuedLine)
                ++stats.storeOrderedCrossLineIssued;
            else
                ++stats.storeSameLineConcurrentIssued;
        } else {
            tsoInFlightSameLineEligible = isCoalescibleStore(storeWBIt);
            if (tsoInFlightSameLineEligible) {
                const auto req = storeWBIt->request()->mainReq();
                tsoInFlightLine = req->getPaddr() & cacheBlockMask;
                tsoInFlightSecure = req->isSecure();
            }
        }
        if (!cpu->checker &&
            isCoalescibleStore(storeWBIt)) {
            const Addr line = storeWBIt->request()->mainReq()->getPaddr() &
                cacheBlockMask;
            ++tsoLinesInFlight[line];
            tsoLastIssuedLine = line;
            storeWBIt->tsoOrderTracked() = true;
        }
        storeWBIt->tsoIssued() = true;
        ++tsoStoresInFlight;
        storeInFlight = true;
    }

    storeWBIt++;
}

void
LSQUnit::writeback(const DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert (!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
    }

    if (!inst->isExecuted()) {
        auto forwarded = partialForwarding.find(inst->seqNum);
        if (forwarded != partialForwarding.end() && pkt && pkt->hasData()) {
            auto *data = pkt->getPtr<uint8_t>();
            for (unsigned byte = 0; byte < forwarded->second.mask.size(); ++byte)
                if (forwarded->second.mask[byte])
                    data[byte] = forwarded->second.data[byte];
        }
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
            // This is the completed load value, including ordinary verified
            // store forwarding. Split requests arrive here only when their
            // combined main packet is ready. Speculative fast forwarding uses
            // a different path and must not masquerade as a data return.
            if (inst->isLoad() && inst->fault == NoFault)
                inst->dataReturnTick = curTick();
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec or - in case of a SplitRequest - a partial
            // translation fault

            // Unless it's a hardware transactional memory fault
            auto htm_fault = std::dynamic_pointer_cast<
                GenericHtmFailureFault>(inst->fault);

            if (!htm_fault) {
                assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr ||
                       inst->savedRequest->isPartialFault());

            } else if (!pkt->htmTransactionFailedInCache()) {
                // Situation in which the instruction has a hardware
                // transactional memory fault but not the packet itself. This
                // can occur with ldp_uop microops since access is spread over
                // multiple packets.
                DPRINTF(HtmCpu,
                        "%s writeback with HTM failure fault, "
                        "however, completing packet is not aware of "
                        "transaction failure. cause=%s htmUid=%u\n",
                        inst->staticInst->getName(),
                        htmFailureToStr(htm_fault->getHtmFailureFaultCause()),
                        htm_fault->getHtmUid());
            }

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }
    }

    // Need to insert instruction into queue to commit
    iewStage->instToCommit(inst);

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

void
LSQUnit::completeStore(typename StoreQueue::iterator store_idx)
{
    assert(store_idx->valid());
    const auto &inst = store_idx->instruction();
    if (sprClflushMinCycles > Cycles(0) && inst->macroop &&
        inst->macroop->getName() == "clflush" &&
        inst->firstIssue != Tick(-1)) {
        const Tick ready = inst->firstIssue +
            cpu->cyclesToTicks(sprClflushMinCycles);
        if (curTick() < ready) {
            // Keep the ordinary SQ/TSO reservation until maintenance
            // completes. Earlier ownership work overlaps this interval.
            cpu->schedule(new EventFunctionWrapper(
                [this, store_idx] { completeStore(store_idx); },
                "architectural CLFLUSH completion", true), ready);
            return;
        }
    }
    const bool tso_issued = store_idx->tsoIssued();
    const bool order_tracked = store_idx->tsoOrderTracked();
    const Addr ordered_line = order_tracked ?
        (store_idx->request()->mainReq()->getPaddr() & cacheBlockMask) : 0;
    store_idx->completed() = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    /* We 'need' a copy here because we may clear the entry from the
     * store queue. */
    DynInstPtr store_inst = store_idx->instruction();
    if (needsTSO && !cpu->checker)
        wakeSelectiveBlockedLoads(store_inst->seqNum);
    if (store_idx == storeQueue.begin()) {
        do {
            storeQueue.front().clear();
            storeQueue.pop_front();
        } while (!storeQueue.empty() && storeQueue.front().completed());

        iewStage->updateLSQNextCycle = true;
    }

    stats.sqAvgOccupancy = queueOccupancy(storeQueue);

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

    store_inst->storeTick = curTick() - store_inst->fetchTick;

    if (stalled &&
        store_inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    store_inst->setCompleted();

    if (needsTSO && tso_issued) {
        if (order_tracked) {
            auto it = tsoLinesInFlight.find(ordered_line);
            assert(it != tsoLinesInFlight.end() && it->second > 0);
            if (--it->second == 0)
                tsoLinesInFlight.erase(it);
        }
        assert(tsoStoresInFlight > 0);
        --tsoStoresInFlight;
        storeInFlight = tsoStoresInFlight != 0;
        if (!storeInFlight)
            tsoInFlightSameLineEligible = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.
    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker &&  !store_inst->isStoreConditional()) {
        cpu->checker->verify(store_inst);
    }
}

bool
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt)
{
    bool ret = true;
    bool cache_got_blocked = false;

    LSQRequest *request = dynamic_cast<LSQRequest*>(data_pkt->senderState);
    const bool bank_access = isLoad && request->instruction()->isLoad() &&
        !request->instruction()->isDataPrefetch() &&
        request->instruction()->effSize < 64 &&
        !data_pkt->req->isUncacheable() && !data_pkt->req->isStrictlyOrdered();

    if (!lsq->cacheBlocked() &&
        lsq->cachePortAvailable(isLoad, data_pkt->getSize())) {
        if (bank_access && !lsq->loadBankAvailable(data_pkt->getAddr())) {
            ++stats.loadBankConflicts;
            ret = false;
        } else if (!dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            cache_got_blocked = true;
        }
    } else {
        ret = false;
    }

    if (ret) {
        if (auto ordered = data_pkt->req->getExtension<TSOOrderedStore>()) {
            fatal_if(!ordered->acceptedBySequencer,
                     "ordered TSO stores require a directly connected "
                     "MESI_Three_Level Ruby Sequencer");
        }
        if (!isLoad) {
            isStoreBlocked = false;
        }
        lsq->cachePortBusy(isLoad, data_pkt->getSize());
        if (bank_access)
            lsq->noteLoadBank(data_pkt->getAddr());
        request->packetSent();
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        if (!isLoad) {
            assert(request == storeWBIt->request());
            isStoreBlocked = true;
        }
        request->packetNotSent();
    }
    DPRINTF(LSQUnit, "Memory request (pkt: %s) from inst [sn:%llu] was"
            " %ssent (cache is blocked: %d, cache_got_blocked: %d)\n",
            data_pkt->print(), request->instruction()->seqNum,
            ret ? "": "not ", lsq->cacheBlocked(), cache_got_blocked);
    return ret;
}

void
LSQUnit::startStaleTranslationFlush()
{
    DPRINTF(LSQUnit, "Unit %p marking stale translations %d %d\n", this,
        storeQueue.size(), loadQueue.size());
    // Retained loads have no outstanding packet whose completion could clear
    // the stale translation. Return them to ordinary translation replay
    // before waiting for the translation flush to drain.
    std::vector<DynInstPtr> retained;
    for (auto it = selectiveBlockedLoads.begin();
         it != selectiveBlockedLoads.end();) {
        if (it->second.retainRequest) {
            retained.push_back(it->second.load);
            it = selectiveBlockedLoads.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &load : retained)
        if (!load->isSquashed())
            rescheduleRetainedLoad(load);

    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
    for (auto &entry : committedStoreBuffer)
        entry.staleTranslation = !entry.completed;
}

bool
LSQUnit::checkStaleTranslations() const
{
    DPRINTF(LSQUnit, "Unit %p checking stale translations\n", this);
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    for (const auto &entry : committedStoreBuffer) {
        if (entry.staleTranslation)
            return true;
    }
    DPRINTF(LSQUnit, "Unit %p found no stale translations\n", this);
    return false;
}

void
LSQUnit::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

void
LSQUnit::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loadQueue.size());
    cprintf("Load queue: ");

    for (const auto& e: loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (const auto& e: storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }

    cprintf("\n");

    cprintf("Selective load waits:%u\n", selectiveBlockedLoads.size());
    for (const auto &[sequence, wait] : selectiveBlockedLoads)
        cprintf("Load [sn:%llu] waits for store [sn:%llu] "
                "data:%d address:%d retained:%d\n",
                sequence, wait.storeSeq, wait.waitForData,
                wait.waitForAddress, wait.retainRequest);
    if (fsfp) {
        cprintf("Fast forwarding predictions:%u\n", fsfp->fastForwarding.size());
        for (const auto &[sequence, prediction] : fsfp->fastForwarding)
            cprintf("Load [sn:%llu] predicted store [sn:%llu] "
                    "value_ready:%d validated:%d failed:%d\n",
                    sequence, prediction.store->seqNum, prediction.valueReady,
                    prediction.validated, prediction.failed);
    }
    cprintf("Committed store lines:%u in_flight:%d cache_blocked:%d\n",
            committedStoreBuffer.size(), storeInFlight, lsq->cacheBlocked());
}

void LSQUnit::schedule(Event& ev, Tick when) { cpu->schedule(ev, when); }

BaseMMU *LSQUnit::getMMUPtr() { return cpu->mmu; }

unsigned int
LSQUnit::cacheLineSize()
{
    return cpu->cacheLineSize();
}

bool
LSQUnit::memorySpeculationAllowed() const
{
    // Used by the detached predictor component if it is explicitly reconnected;
    // the SPR fixed profile does not call this dynamic gate.
    return !sprMemoryDependence || !(cpu->tcBase(lsqID)->readMiscRegNoEffect(
        X86ISA::misc_reg::SpecCtrl) & (uint64_t(1) << 2)); // SSBD
}

void
LSQUnit::rememberForwarding(const DynInstPtr &load, const DynInstPtr &store)
{
    fullForwardingSource.insert_or_assign(load->seqNum, store->seqNum);
    if (fsfp)
        recordForwardingTraining(load, store);
}

bool
LSQUnit::collectSameStartForwarding(LSQRequest *request, ssize_t load_idx)
{
    // The single-producer policy requires complete coverage by one store;
    // partial store bytes may not be combined with a cache response either.
    if (!sprMultiStoreForwarding)
        return false;
    const auto &load = loadQueue[load_idx].instruction();
    if (partialForwarding.count(load->seqNum))
        return true;
    const auto req = request->mainReq();
    if (!needsTSO ||
        cpu->checker || request->isSplit() ||
        load->isDataPrefetch() || load->isAtomic() ||
        load->isReadBarrier() || load->isWriteBarrier() ||
        load->inHtmTransactionalState() || req->isMasked() || req->isLLSC() ||
        req->isReadModifyWrite() || req->isAcquire() || req->isLocalAccess() ||
        req->isUncacheable() || req->isStrictlyOrdered() ||
        req->isCacheMaintenance() || req->isMemMgmt())
        return false;
    const unsigned size = req->getSize();
    const Addr address = req->getPaddr();
    PartialForwarding bytes{std::vector<uint8_t>(size, 0),
                            std::vector<bool>(size, false),
                            std::vector<InstSeqNum>(size, 0), address};
    bool partial = false;
    bool sqProducer = false;
    bool bufferProducer = false;
    auto store = load->sqIt;
    while (store != storeWBIt) {
        --store;
        if (!store->instruction()->effAddrValid())
            return false; // Unknown younger producers must not be skipped.
        if (!store->size())
            continue;
        if (store->request()->isSplit() ||
            store->request()->mainReq()->isSecure() != req->isSecure())
            return false;
        const Addr start = store->request()->mainReq()->getPaddr();
        const Addr end = start + store->size();
        if (start >= address + size || end <= address)
            continue;
        bool uncovered = false;
        for (unsigned byte = 0; byte < size; ++byte)
            uncovered |= !bytes.mask[byte] && address + byte >= start &&
                         address + byte < end;
        if (!uncovered)
            continue; // A younger store already owns every overlapping byte.
        if (!isOrdinaryStore(store) || start != address ||
            (store->instruction()->isStoreAddress() && !store->dataReady()))
            return false;
        if (!partial && store->size() >= size)
            return false; // Preserve the ordinary single-store full path.
        partial = true;
        for (unsigned byte = 0; byte < std::min(size, store->size()); ++byte)
            if (!bytes.mask[byte]) {
                bytes.data[byte] = store->data()[byte];
                bytes.mask[byte] = true;
                bytes.sourceSequence[byte] = store->instruction()->seqNum;
                sqProducer = true;
            }
    }
    for (auto entry = committedStoreBuffer.rbegin();
         entry != committedStoreBuffer.rend(); ++entry) {
        if (entry->completed || entry->firstSeq >= load->seqNum ||
            entry->flags.isSet(Request::SECURE) != req->isSecure())
            continue;
        for (unsigned byte = 0; byte < size; ++byte) {
            const Addr physical = req->getPaddr() + byte;
            if (bytes.mask[byte] || physical < entry->line ||
                physical - entry->line >= entry->mask.size())
                continue;
            const unsigned offset = physical - entry->line;
            if (!entry->mask[offset])
                continue;
            if (entry->invalidated ||
                entry->sourceStart[offset] != req->getPaddr())
                return false;
            bytes.mask[byte] = true;
            bytes.data[byte] = entry->data[offset];
            bytes.sourceSequence[byte] = entry->sourceSequence[offset];
            bufferProducer = true;
        }
    }
    const unsigned supplied = std::count(bytes.mask.begin(), bytes.mask.end(), true);
    if (!supplied || (!partial && supplied == size))
        return false;
    if (sqProducer && bufferProducer)
        ++stats.sameStartMixedStoreProducers;
    // The same-start path may combine producers from both the SQ and the
    // committed buffer. Apply the multi-producer policy to this path too;
    // a single partial producer may still merge its bytes with cache data.
    if (!sprMultiStoreForwarding) {
        bool found = false;
        InstSeqNum producer = 0;
        for (unsigned byte = 0; byte < size; ++byte) {
            if (!bytes.mask[byte])
                continue;
            if (found && bytes.sourceSequence[byte] != producer) {
                if (sqProducer && bufferProducer)
                    ++stats.sameStartMixedStoreRejected;
                return false;
            }
            producer = bytes.sourceSequence[byte];
            found = true;
        }
    }
    partialForwarding.emplace(load->seqNum, std::move(bytes));
    ++stats.sameStartPartialLoads;
    ++stats.forwLoads;
    return true;
}

bool
LSQUnit::forwardFromStoreBuffer(LSQRequest *request, ssize_t load_idx)
{
    if (committedStoreBuffer.empty())
        return false;
    LQEntry &load_entry = loadQueue[load_idx];
    const DynInstPtr load = load_entry.instruction();
    const auto req = request->mainReq();
    const unsigned size = req->getSize();
    const bool ordinary = !request->isSplit() && !req->isLLSC() &&
        !req->isLocalAccess() && !req->isUncacheable() &&
        !req->isStrictlyOrdered() && !req->isCacheMaintenance() &&
        !req->isMasked() && !load->isAtomic() &&
        !load->isReadBarrier() && !load->isWriteBarrier() &&
        !load->inHtmTransactionalState();

    std::vector<uint8_t> data(size, 0);
    std::vector<bool> covered(size, false);
    std::vector<InstSeqNum> producers(size, 0);
    std::vector<Addr> producerPC(fsfp ? size : 0, 0);
    std::vector<Addr> producerStart(size, 0);
    std::vector<bool> producerFastForwardingAddress(fsfp ? size : 0, false);
    bool overlap = false;
    auto oldest_pending = std::find_if(
        committedStoreBuffer.begin(), committedStoreBuffer.end(),
        [](const auto &entry) { return !entry.completed; });
    assert(oldest_pending != committedStoreBuffer.end());
    InstSeqNum wait_seq = oldest_pending->lastSeq;

    // A normal split load is not an ordering barrier. The main request's
    // physical address is only a placeholder for split operations, so use
    // the translated fragments; adjacent virtual pages need not be adjacent
    // in physical memory. Keep the conservative wait for special accesses.
    const bool ordinary_split = request->isSplit() && !req->isLLSC() &&
        !req->isReadModifyWrite() && !req->isAtomic() && !req->isAcquire() &&
        !req->isLocalAccess() && !req->isUncacheable() &&
        !req->isStrictlyOrdered() && !req->isCacheMaintenance() &&
        !req->isMemMgmt() && !(req->getFlags() & Request::NO_ACCESS) &&
        !req->isMasked() && !load->isAtomic() && !load->isReadBarrier() &&
        !load->isWriteBarrier() && !load->inHtmTransactionalState() &&
        !request->_reqs.empty() && std::none_of(
            request->_reqs.begin(), request->_reqs.end(),
            [](const auto &fragment) {
                return fragment->isLocalAccess() || fragment->isAcquire() ||
                    (fragment->getFlags() & Request::NO_ACCESS);
            });
    if (ordinary_split) {
        bool unknown_translation = false;
        for (auto entry = committedStoreBuffer.rbegin();
             entry != committedStoreBuffer.rend(); ++entry) {
            if (entry->completed || entry->firstSeq >= load->seqNum)
                continue;
            for (const auto &fragment : request->_reqs) {
                if (!fragment->hasPaddr()) {
                    unknown_translation = true;
                    continue;
                }
                if (entry->flags.isSet(Request::SECURE) != fragment->isSecure())
                    continue;
                const auto &enabled = fragment->getByteEnable();
                for (unsigned byte = 0; byte < fragment->getSize(); ++byte) {
                    const Addr addr = fragment->getPaddr() + byte;
                    if (enabled[byte] && addr >= entry->line &&
                        addr - entry->line < entry->mask.size() &&
                        entry->mask[addr - entry->line]) {
                        overlap = true;
                        wait_seq = entry->lastSeq;
                        break;
                    }
                }
                if (overlap)
                    break;
            }
            if (overlap)
                break;
        }
        // An untranslated/faulting fragment cannot establish non-overlap.
        if (!overlap && !unknown_translation)
            return false;
    }
    if (ordinary) {
        const Addr address = req->getPaddr();
        for (auto entry = committedStoreBuffer.rbegin();
             entry != committedStoreBuffer.rend(); ++entry) {
            if (entry->completed || entry->firstSeq >= load->seqNum ||
                entry->flags.isSet(Request::SECURE) != req->isSecure())
                continue;
            for (unsigned byte = 0; byte < size; ++byte) {
                const Addr byte_addr = address + byte;
                if (covered[byte] || byte_addr < entry->line ||
                    byte_addr >= entry->line + entry->data.size())
                    continue;
                const unsigned offset = byte_addr - entry->line;
                if (entry->mask[offset]) {
                    if (entry->invalidated) {
                        wait_seq = entry->lastSeq;
                        overlap = true;
                        covered.assign(size, false);
                        break;
                    }
                    if (!overlap)
                        wait_seq = entry->lastSeq;
                    overlap = true;
                    covered[byte] = true;
                    data[byte] = entry->data[offset];
                    producers[byte] = entry->sourceSequence[offset];
                    if (fsfp) {
                        producerPC[byte] = entry->sourcePC[offset];
                        producerFastForwardingAddress[byte] =
                            entry->sourceFastForwardingAddress[offset];
                    }
                    producerStart[byte] = entry->sourceStart[offset];
                }
            }
            if (entry->invalidated && overlap &&
                wait_seq == entry->lastSeq)
                break;
        }
        if (!overlap)
            return false;
        const bool single_producer = size &&
            std::all_of(producers.begin(), producers.end(),
                [&](auto sequence) { return sequence == producers.front(); });
        if (std::all_of(covered.begin(), covered.end(), [](bool v) { return v; }) &&
            (sprMultiStoreForwarding || single_producer)) {
            // The value is already selected even if its writeback event has
            // not run.  Include it in violation checks against unresolved
            // younger stores.  The oldest byte producer is a conservative
            // bound when several committed stores supply the load.
            fullForwardingSource.insert_or_assign(
                load->seqNum, *std::min_element(producers.begin(), producers.end()));
            DPRINTF(LSQUnit, "Store buffer forwards load [sn:%llu] "
                    "vaddr %#x paddr %#x oldest producer [sn:%llu]\n",
                    load->seqNum, req->getVaddr(), address,
                    fullForwardingSource.at(load->seqNum));
            if (fsfp && single_producer && fastForwardingAllowed() &&
                load->staticInst->earlyForwardingSize() &&
                producerFastForwardingAddress.front() &&
                producerStart.front() == address &&
                producers.front())
                fsfp->forwardingTraining.insert_or_assign(
                    load->seqNum, producerPC.front() & 0xffff);
            request->noteNativeTerminal(TaoTraceNativeTerminalReason::StoreForward);
            if (!load->memData)
                load->memData = new uint8_t[size];
            std::memcpy(load->memData, data.data(), size);
            PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
            pkt->dataStatic(load->memData);
            if (request->isAnyOutstandingRequest()) {
                request->discard();
                load_entry.setRequest(nullptr);
            }
            const Cycles service = sprStoreForwardingLatency +
                (single_producer && producerStart.front() != address ?
                    sprContainedForwardingExtraLatency : Cycles(0));
            cpu->schedule(new WritebackEvent(load, pkt, this),
                service > Cycles(0) ? cpu->clockEdge(service) : curTick());
            ++stats.forwLoads;
            ++stats.storeBufferForwardedLoads;
            return true;
        }
    }

    const bool retain = canRetainLoadRequest(request);
    selectiveBlockedLoads.insert_or_assign(
        load->seqNum, SelectiveBlockedLoad{wait_seq, load, false, false, retain});
    ++stats.storeBufferPartialWaits;
    if (retain)
        return true;
    iewStage->rescheduleMemInst(load);
    load->clearIssued();
    load->effAddrValid(false);
    ++stats.rescheduledLoads;
    request->discard();
    load_entry.setRequest(nullptr);
    return true;
}

Fault
LSQUnit::read(LSQRequest *request, ssize_t load_idx)
{
    LQEntry& load_entry = loadQueue[load_idx];
    const DynInstPtr& load_inst = load_entry.instruction();

    load_entry.setRequest(request);
    assert(load_inst);

    assert(!load_inst->isExecuted());

    // Make sure this isn't a strictly ordered load
    // A bit of a hackish way to get strictly ordered accesses to work
    // only if they're at the head of the LSQ and are ready to commit
    // (at the head of the ROB too).

    if (request->mainReq()->isStrictlyOrdered() &&
        (load_idx != loadQueue.head() || !load_inst->isAtCommit())) {
        // Tell IQ/mem dep unit that this instruction will need to be
        // rescheduled eventually
        iewStage->rescheduleMemInst(load_inst);
        load_inst->clearIssued();
        load_inst->effAddrValid(false);
        ++stats.rescheduledLoads;
        DPRINTF(LSQUnit, "Strictly ordered load [sn:%lli] PC %s\n",
                load_inst->seqNum, load_inst->pcState());

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        load_entry.setRequest(nullptr);
        request->discard();
        return std::make_shared<GenericISA::M5PanicFault>(
            "Strictly ordered load [sn:%llx] PC %s\n",
            load_inst->seqNum, load_inst->pcState());
    }

    DPRINTF(LSQUnit, "Read called, load idx: %i, store idx: %i, "
            "storeHead: %i addr: %#x%s\n",
            load_idx - 1, load_inst->sqIt._idx, storeQueue.head() - 1,
            request->mainReq()->getPaddr(), request->isSplit() ? " split" :
            "");

    // The fixed SPR profile never speculates past any older unresolved
    // store, even when a younger known store fully covers this load. Check
    // before *any* forwarding, local access or cache request is performed.
    if (sprMemoryDependence && !load_inst->isDataPrefetch()) {
        auto older = load_inst->sqIt;
        assert(older >= storeWBIt);
        while (older != storeWBIt) {
            --older;
            const auto &store = older->instruction();
            if (store->effAddrValid())
                continue;
            const bool retain = canRetainLoadRequest(request);
            iewStage->instQueue.memoryDependence(lsqID).noteAddressWait(load_inst);
            selectiveBlockedLoads.insert_or_assign(load_inst->seqNum,
                SelectiveBlockedLoad{store->seqNum, load_inst, false, true,
                                     retain});
            if (!retain) {
                iewStage->rescheduleMemInst(load_inst);
                load_inst->clearIssued();
                load_inst->effAddrValid(false);
                ++stats.rescheduledLoads;
                request->discard();
                load_entry.setRequest(nullptr);
            }
            return NoFault;
        }
    }

    if (request->mainReq()->isLLSC()) {
        // Disable recording the result temporarily.  Writing to misc
        // regs normally updates the result, but this is not the
        // desired behavior when handling store conditionals.
        load_inst->recordResult(false);
        load_inst->tcBase()->getIsaPtr()->handleLockedRead(load_inst.get(),
                request->mainReq());
        load_inst->recordResult(true);
    }

    if (request->mainReq()->isLocalAccess()) {
        request->noteNativeTerminal(
            TaoTraceNativeTerminalReason::LocalAccess);
        assert(!load_inst->memData);
        load_inst->memData = new uint8_t[MaxDataBytes];

        gem5::ThreadContext *thread = cpu->tcBase(lsqID);
        PacketPtr main_pkt = new Packet(request->mainReq(), MemCmd::ReadReq);

        main_pkt->dataStatic(load_inst->memData);

        Cycles delay = request->mainReq()->localAccessor(thread, main_pkt);

        WritebackEvent *wb = new WritebackEvent(load_inst, main_pkt, this);
        cpu->schedule(wb, cpu->clockEdge(delay));
        return NoFault;
    }

    if (fsfp && tryFastForwarding(request, load_idx))
        return NoFault;

    const bool partial_forward = collectSameStartForwarding(request, load_idx);

    // Check the SQ for any previous stores that might lead to forwarding
    auto store_it = load_inst->sqIt;
    assert (store_it >= storeWBIt);
    // Translation state cannot change during this synchronous SQ scan.
    // Reuse the load's physical bytes across older-store comparisons.
    std::vector<Addr> readBytes;
    bool readBytesResolved = false;
    bool readBytesComplete = false;
    // End once we've reached the top of the LSQ
    while (store_it != storeWBIt &&
           !load_inst->isDataPrefetch() && !partial_forward) {
        // Move the index to one younger
        store_it--;
        assert(store_it->valid());
        assert(store_it->instruction()->seqNum < load_inst->seqNum);
        int store_size = store_it->size();

        // Cache maintenance instructions go down via the store
        // path but they carry no data and they shouldn't be
        // considered for forwarding
        if (store_size != 0 && !store_it->instruction()->strictlyOrdered() &&
            !(store_it->request()->mainReq() &&
              store_it->request()->mainReq()->isCacheMaintenance())) {
            assert(store_it->instruction()->effAddrValid());

            // Check if the store data is within the lower and upper bounds of
            // addresses that the request needs.
            auto req_s = request->mainReq()->getVaddr();
            auto req_e = req_s + request->mainReq()->getSize();
            auto st_s = store_it->instruction()->effAddr;
            auto st_e = st_s + store_size;

            bool store_has_lower_limit = req_s >= st_s;
            bool store_has_upper_limit = req_e <= st_e;
            bool lower_load_has_store_part = req_s < st_e;
            bool upper_load_has_store_part = req_e > st_s;

            auto coverage = AddrRangeCoverage::NoAddrRangeCoverage;

            // If the store entry is not atomic (atomic does not have valid
            // data), the store has all of the data needed, and
            // the load is not LLSC, then
            // we can forward data from the store to the load
            if (!store_it->instruction()->isAtomic() &&
                store_has_lower_limit && store_has_upper_limit &&
                !request->mainReq()->isLLSC()) {

                const auto& store_req = store_it->request()->mainReq();
                coverage = store_req->isMasked() ?
                    AddrRangeCoverage::PartialAddrRangeCoverage :
                    AddrRangeCoverage::FullAddrRangeCoverage;
            } else if (
                // This is the partial store-load forwarding case where a store
                // has only part of the load's data and the load isn't LLSC
                (!request->mainReq()->isLLSC() &&
                 ((store_has_lower_limit && lower_load_has_store_part) ||
                  (store_has_upper_limit && upper_load_has_store_part) ||
                  (lower_load_has_store_part && upper_load_has_store_part))) ||
                // The load is LLSC, and the store has all or part of the
                // load's data
                (request->mainReq()->isLLSC() &&
                 ((store_has_lower_limit || upper_load_has_store_part) &&
                  (store_has_upper_limit || lower_load_has_store_part))) ||
                // The store entry is atomic and has all or part of the load's
                // data
                (store_it->instruction()->isAtomic() &&
                 ((store_has_lower_limit || upper_load_has_store_part) &&
                  (store_has_upper_limit || lower_load_has_store_part)))) {

                coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
            }

            if (sprMemoryDependence) {
                if (!readBytesResolved) {
                    readBytesComplete = request->physicalBytes(readBytes);
                    readBytesResolved = true;
                }
                std::vector<Addr> writeBytes;
                auto *store_request = store_it->request();
                const bool complete = readBytesComplete &&
                    store_request->physicalBytes(writeBytes);
                coverage = AddrRangeCoverage::NoAddrRangeCoverage;
                if (!complete) {
                    // A faulting prefix cannot prove non-overlap. Wait for
                    // older writes before issuing that prefix's memory work.
                    coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
                } else if (store_request->mainReq()->isSecure() ==
                           request->mainReq()->isSecure()) {
                    const bool overlap = PhysicalByteCoverage::intersects(
                        readBytes, writeBytes);
                    if (overlap) {
                        coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
                        const auto &load_req = request->mainReq();
                        const auto &store_req = store_request->mainReq();
                        // A split main request has a dummy paddr. Compare the
                        // fully translated enabled bytes from its fragments;
                        // equality of virtual addresses alone is insufficient.
                        // Limit the candidate to identical starts and sizes
                        // within one minimum x86 page: no cross-page shortcut,
                        // masked bytes, special accesses or already sampled
                        // cache fragments. Younger SQ stores have already
                        // been checked by this youngest-to-oldest search.
                        auto ordinary = [](const RequestPtr &r) {
                            return !r->isMasked() && !r->isLLSC() &&
                                !r->isAtomic() && !r->isReadModifyWrite() &&
                                !r->isAcquire() && !r->isRelease() &&
                                !r->isSwap() &&
                                !r->isLocalAccess() && !r->isUncacheable() &&
                                !r->isStrictlyOrdered() &&
                                !r->isCacheMaintenance() && !r->isMemMgmt() &&
                                !(r->getFlags() & (Request::NO_ACCESS |
                                                  Request::STORE_NO_DATA));
                        };
                        const auto load_size = load_req->getSize();
                        const bool split_candidate = request->isSplit() &&
                            store_request->isSplit() && load_size &&
                            load_req->hasVaddr() && store_req->hasVaddr() &&
                            load_req->getVaddr() == store_req->getVaddr() &&
                            load_size == store_req->getSize() &&
                            (load_req->getVaddr() & 4095) + load_size <= 4096 &&
                            readBytes.size() == load_size &&
                            writeBytes.size() == load_size &&
                            readBytes == writeBytes &&
                            ordinary(load_req) && ordinary(store_req) &&
                            !cpu->checker &&
                            !load_inst->isAtomic() &&
                            !store_it->instruction()->isAtomic() &&
                            !store_it->instruction()->isStoreConditional() &&
                            !load_inst->inHtmTransactionalState() &&
                            !store_it->instruction()->inHtmTransactionalState() &&
                            !request->hasBuiltPackets() &&
                            !request->isAnyOutstandingRequest() &&
                            !request->hasReturnedData();
                        if (split_candidate)
                            ++stats.samePageSplitCandidates;
                        if (!request->isSplit() && !store_request->isSplit() &&
                            !store_it->instruction()->isAtomic() &&
                            !request->mainReq()->isLLSC() &&
                            request->mainReq()->getPaddr() >=
                                store_request->mainReq()->getPaddr() &&
                            request->mainReq()->getPaddr() -
                                store_request->mainReq()->getPaddr() +
                                request->mainReq()->getSize() <= unsigned(store_size) &&
                            PhysicalByteCoverage::contains(writeBytes, readBytes))
                            coverage = AddrRangeCoverage::FullAddrRangeCoverage;
                        if (split_candidate)
                            coverage = AddrRangeCoverage::FullAddrRangeCoverage;
                    }
                }
            }

            if (coverage != AddrRangeCoverage::NoAddrRangeCoverage &&
                store_it->instruction()->isStoreAddress() &&
                !store_it->dataReady() &&
                !request->mainReq()->isLLSC()) {
                const bool retain = canRetainLoadRequest(request);
                selectiveBlockedLoads.insert_or_assign(
                    load_inst->seqNum,
                    SelectiveBlockedLoad{store_it->instruction()->seqNum,
                                         load_inst, true, false, retain});
                ++stats.splitStoreDataWaits;
                if (retain) {
                    ++stats.retainedStoreDataWaits;
                    return NoFault;
                }
                iewStage->rescheduleMemInst(load_inst);
                load_inst->clearIssued();
                load_inst->effAddrValid(false);
                ++stats.rescheduledLoads;
                request->discard();
                load_entry.setRequest(nullptr);
                return NoFault;
            }

            if (coverage == AddrRangeCoverage::FullAddrRangeCoverage) {
                if (request->isSplit())
                    ++stats.samePageSplitForwarded;
                rememberForwarding(load_inst, store_it->instruction());
                request->noteNativeTerminal(
                    TaoTraceNativeTerminalReason::StoreForward);
                // Get shift amount for offset into the store's data.
                int shift_amt = request->isSplit() ? 0 : sprMemoryDependence ?
                    request->mainReq()->getPaddr() -
                        store_it->request()->mainReq()->getPaddr() :
                    request->mainReq()->getVaddr() - store_it->instruction()->effAddr;

                // Allocate memory if this is the first time a load is issued.
                if (!load_inst->memData) {
                    load_inst->memData =
                        new uint8_t[request->mainReq()->getSize()];
                }
                if (store_it->isAllZeros())
                    memset(load_inst->memData, 0,
                            request->mainReq()->getSize());
                else
                    memcpy(load_inst->memData,
                        store_it->data() + shift_amt,
                        request->mainReq()->getSize());

                DPRINTF(LSQUnit, "Forwarding from store idx %i to load to "
                        "addr %#x\n", store_it._idx,
                        request->mainReq()->getVaddr());

                PacketPtr data_pkt = new Packet(request->mainReq(),
                        MemCmd::ReadReq);
                data_pkt->dataStatic(load_inst->memData);

                // hardware transactional memory
                // Store to load forwarding within a transaction
                // This should be okay because the store will be sent to
                // the memory subsystem and subsequently get added to the
                // write set of the transaction. The write set has a stronger
                // property than the read set, so the load doesn't necessarily
                // have to be there.
                assert(!request->mainReq()->isHTMCmd());
                if (load_inst->inHtmTransactionalState()) {
                    assert (!storeQueue[store_it._idx].completed());
                    assert (
                        storeQueue[store_it._idx].instruction()->
                          inHtmTransactionalState());
                    assert (
                        load_inst->getHtmTransactionUid() ==
                        storeQueue[store_it._idx].instruction()->
                          getHtmTransactionUid());
                    data_pkt->setHtmTransactional(
                        load_inst->getHtmTransactionUid());
                    DPRINTF(HtmCpu, "HTM LD (ST2LDF) "
                      "pc=0x%lx - vaddr=0x%lx - "
                      "paddr=0x%lx - htmUid=%u\n",
                      load_inst->pcState().instAddr(),
                      data_pkt->req->hasVaddr() ?
                        data_pkt->req->getVaddr() : 0lu,
                      data_pkt->getAddr(),
                      load_inst->getHtmTransactionUid());
                }

                if (request->isAnyOutstandingRequest()) {
                    assert(request->_numOutstandingPackets > 0);
                    // There are memory requests packets in flight already.
                    // This may happen if the store was not complete the
                    // first time this load got executed. Signal the senderSate
                    // that response packets should be discarded.
                    request->discard();
                    // Avoid checking snoops on this discarded request.
                    load_entry.setRequest(nullptr);
                }

                WritebackEvent *wb = new WritebackEvent(load_inst, data_pkt,
                        this);

                // Same-address forwarding and contained-offset service have
                // separate timing controls.  Neither bypasses producer or
                // physical-byte validation.
                const Cycles service = sprStoreForwardingLatency +
                    (shift_amt ? sprContainedForwardingExtraLatency : Cycles(0));
                cpu->schedule(wb, service > Cycles(0) ?
                    cpu->clockEdge(service) : curTick());

                // Don't need to do anything special for split loads.
                ++stats.forwLoads;

                return NoFault;
            } else if (
                    coverage == AddrRangeCoverage::PartialAddrRangeCoverage) {
                // If it's already been written back, then don't worry about
                // stalling on it.
                if (store_it->completed()) {
                    panic("Should not check one of these");
                    continue;
                }

                // Special accesses retain the conservative oldest-load wakeup.
                const bool selective = canRetainLoadRequest(request);
                if (selective) {
                    const bool retain = canRetainLoadRequest(request);
                    selectiveBlockedLoads.insert_or_assign(
                        load_inst->seqNum,
                        SelectiveBlockedLoad{store_it->instruction()->seqNum,
                                             load_inst, false, false, retain});
                    ++stats.selectivePartialWaits;
                    if (retain)
                        return NoFault;
                } else if (!stalled ||
                    (stalled &&
                     load_inst->seqNum <
                     loadQueue[stallingLoadIdx].instruction()->seqNum)) {
                    stalled = true;
                    stallingStoreIsn = store_it->instruction()->seqNum;
                    stallingLoadIdx = load_idx;
                }

                // Tell IQ/mem dep unit that this instruction will need to be
                // rescheduled eventually
                iewStage->rescheduleMemInst(load_inst);
                load_inst->clearIssued();
                load_inst->effAddrValid(false);
                ++stats.rescheduledLoads;

                // Do not generate a writeback event as this instruction is not
                // complete.
                DPRINTF(LSQUnit, "Load-store forwarding mis-match. "
                        "Store idx %i to load addr %#x\n",
                        store_it._idx, request->mainReq()->getVaddr());

                // Must discard the request.
                request->discard();
                load_entry.setRequest(nullptr);
                return NoFault;
            }
        }
    }

    if (!partial_forward && !load_inst->isDataPrefetch() &&
        forwardFromStoreBuffer(request, load_idx))
        return NoFault;

    // If there's no forwarding case, then go access memory
    DPRINTF(LSQUnit, "Doing memory access for inst [sn:%lli] PC %s "
            "vaddr %#x paddr %#x size %u\n",
            load_inst->seqNum, load_inst->pcState(),
            request->mainReq()->hasVaddr() ?
                request->mainReq()->getVaddr() : 0,
            request->mainReq()->hasPaddr() ?
                request->mainReq()->getPaddr() : 0,
            request->mainReq()->getSize());

    // Allocate memory if this is the first time a load is issued.
    if (!load_inst->memData) {
        load_inst->memData = new uint8_t[request->mainReq()->getSize()];
    }


    // hardware transactional memory
    if (request->mainReq()->isHTMCmd()) {
        // this is a simple sanity check
        // the Ruby cache controller will set
        // memData to 0x0ul if successful.
        *load_inst->memData = (uint64_t) 0x1ull;
    }

    // Execution-port admission models AGU issue; packet admission below
    // models the separate L1 data service, charged only on successful send.

    // if we the cache is not blocked, do cache access
    // if the request is not sent and cache is unblocked
    // then put the instruction into retry queue so we do not need
    // an exta cycle to re-issue and execute
    request->buildPackets();
    request->sendPacketToCache();
    if (!request->isSent()) {
        if (!lsq->cacheBlocked()) {
            iewStage->retryMemInst(load_inst);
       } else {
            iewStage->blockMemInst(load_inst);
       }
    }

    return NoFault;
}

Fault
LSQUnit::write(LSQRequest *request, uint8_t *data, ssize_t store_idx)
{
    assert(storeQueue[store_idx].valid());

    DPRINTF(LSQUnit, "Doing write to store idx %i, addr %#x | storeHead:%i "
            "[sn:%llu]\n",
            store_idx - 1, request->req()->getPaddr(), storeQueue.head() - 1,
            storeQueue[store_idx].instruction()->seqNum);

    storeQueue[store_idx].setRequest(request);
    unsigned size = request->_size;
    storeQueue[store_idx].size() = size;
    if (storeQueue[store_idx].instruction()->isStoreAddress()) {
        if (!storeQueue[store_idx].addressReady() &&
            !storeQueue[store_idx].dataReady())
            ++stats.splitStoreAddressFirst;
        storeQueue[store_idx].addressReady() = true;
        panic_if(storeQueue[store_idx].dataReady() &&
                 storeQueue[store_idx].dataBytes() != size,
                 "Store address and data sizes disagree");
        return NoFault;
    }
    bool store_no_data =
        request->mainReq()->getFlags() & Request::STORE_NO_DATA;
    storeQueue[store_idx].isAllZeros() = store_no_data;
    assert(size <= SQEntry::DataSize || store_no_data);

    // copy data into the storeQueue only if the store request has valid data
    if (!(request->req()->getFlags() & Request::CACHE_BLOCK_ZERO) &&
        !request->req()->isCacheMaintenance() &&
        !request->req()->isAtomic())
        memcpy(storeQueue[store_idx].data(), data, size);

    // This function only writes the data to the store queue, so no fault
    // can happen here.
    return NoFault;
}

InstSeqNum
LSQUnit::getLoadHeadSeqNum()
{
    if (loadQueue.front().valid())
        return loadQueue.front().instruction()->seqNum;
    else
        return 0;
}

InstSeqNum
LSQUnit::getStoreHeadSeqNum()
{
    if (storeQueue.front().valid())
        return storeQueue.front().instruction()->seqNum;
    else
        return 0;
}

} // namespace o3
} // namespace gem5
