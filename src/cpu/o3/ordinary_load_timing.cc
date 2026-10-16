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
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{
namespace o3
{

bool
LSQUnit::ordinaryLoad(LSQRequest *request) const
{
    const auto &inst = request->instruction();
    const auto req = request->mainReq();
    return inst->isLoad() && !inst->isAtomic() && !inst->isDataPrefetch() &&
        !inst->isVector() && !inst->isFloating() &&
        !inst->inHtmTransactionalState() && inst->fault == NoFault &&
        !request->isSplit() && req->getSize() == 8 && !req->isLocalAccess() &&
        !req->isUncacheable() && !req->isStrictlyOrdered() && !req->isLLSC() &&
        !req->isLockedRMW() && !req->isReadModifyWrite() &&
        !req->isCacheMaintenance();
}

LSQUnit::LoadServiceRequestEvent::LoadServiceRequestEvent(
        LSQRequest *request, LSQUnit *owner)
    : Event(Default_Pri, AutoDelete), request(request), owner(owner)
{
    assert(!request->serviceScheduled());
    request->scheduleService();
    ++owner->pendingLoadServiceRequests;
}

void
LSQUnit::LoadServiceRequestEvent::process()
{
    assert(!owner->cpu->switchedOut());
    owner->cpu->wakeCPU();
    if (request->isReleased() || request->instruction()->isSquashed()) {
        ++owner->serviceStats->serviceSquashed;
        --owner->pendingLoadServiceRequests;
        request->finishService();
        return;
    }
    if (!owner->lsq->cacheBlocked()) {
        auto decision = owner->loadService.accept(request->getVaddr(),
            request->mainReq()->getSize(), owner->cpu->clockEdge());
        if (decision == OrdinaryLoadService::Decision::AdmissionRetry) {
            ++owner->serviceStats->serviceAdmissionRetries;
        } else if (decision == OrdinaryLoadService::Decision::ArbitrationRetry) {
            ++owner->serviceStats->serviceArbitrationRetries;
        } else {
            ++owner->serviceStats->serviceAccepted;
            request->sendPacketToCache();
            if (request->isSent()) {
                --owner->pendingLoadServiceRequests;
                request->finishService();
                return;
            }
            ++owner->serviceStats->serviceCacheRetries;
        }
    }
    // Capacity rejection and native Ruby backpressure retain the SAME request.
    // No early wakeup, duplicate packet or IQ replay is manufactured.
    owner->schedule(*this, owner->cpu->clockEdge(Cycles(1)));
}

LSQUnit::LoadResultEvent::LoadResultEvent(
        const DynInstPtr &inst, LSQRequest *request, LSQUnit *owner)
    : Event(Default_Pri, AutoDelete), inst(inst), request(request), owner(owner)
{
    request->writebackScheduled();
    ++owner->pendingLoadResults;
}

void
LSQUnit::LoadResultEvent::process()
{
    assert(!owner->cpu->switchedOut());
    owner->writeback(inst, request->mainPacket());
    --owner->pendingLoadResults;
    // The request owns its response packet and survives squash until this
    // event releases its outstanding-writeback reference.
    request->writebackDone();
    owner->cpu->wakeCPU();
}

} // namespace o3
} // namespace gem5
