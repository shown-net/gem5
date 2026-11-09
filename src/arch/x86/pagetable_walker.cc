/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved.
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
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
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

#include "arch/x86/pagetable_walker.hh"

#include <memory>
#include <utility>

#include "arch/x86/faults.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/tlb.hh"
#include "base/bitfield.hh"
#include "base/trie.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/PageTableWalker.hh"
#include "mem/functional_translation.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"

namespace gem5
{

namespace X86ISA {

Walker::ItlbStats::ItlbStats(Walker *parent)
    : statistics::Group(parent, "ITLB_MISSES"), owner(parent),
      ADD_STAT(WALK_COMPLETED_4K, statistics::units::Count::get(),
               "Completed instruction walks with an identified 4 KiB leaf"),
      ADD_STAT(WALK_COMPLETED_2M_4M, statistics::units::Count::get(),
               "Completed instruction walks with an identified 2 or 4 MiB leaf"),
      ADD_STAT(WALK_COMPLETED, statistics::units::Count::get(),
               "Completed modeled instruction walks including faults"),
      ADD_STAT(WALK_COMPLETED_1G, statistics::units::Count::get(),
               "Completed instruction walks with an identified 1 GiB leaf"),
      ADD_STAT(unclassifiedCompleted, statistics::units::Count::get(),
               "Completed instruction walks ending before leaf size was established"),
      ADD_STAT(WALK_ACTIVE, statistics::units::Cycle::get(),
               "Cycles with an instruction walk in service"),
      ADD_STAT(WALK_PENDING, statistics::units::Cycle::get(),
               "Integral of instruction walks in service; excludes queue"),
      ADD_STAT(COALESCED_REQUESTS, statistics::units::Count::get(),
               "Model instruction translations joined to an in-flight walk")
{}

Walker::DtlbWalkStats::DtlbWalkStats(Walker *parent, const char *name)
    : statistics::Group(parent, name), owner(parent),
      ADD_STAT(WALK_COMPLETED, statistics::units::Count::get(),
               "Completed timing demand walks, including faults and completed speculation"),
      ADD_STAT(WALK_COMPLETED_4K, statistics::units::Count::get(),
               "Completed demand walks with an identified 4 KiB leaf"),
      ADD_STAT(WALK_COMPLETED_2M_4M, statistics::units::Count::get(),
               "Completed demand walks with an identified 2 or 4 MiB leaf"),
      ADD_STAT(WALK_COMPLETED_1G, statistics::units::Count::get(),
               "Completed demand walks with an identified 1 GiB leaf"),
      ADD_STAT(unclassifiedCompleted, statistics::units::Count::get(),
               "Completed demand walks ending before leaf size was established"),
      ADD_STAT(WALK_ACTIVE, statistics::units::Cycle::get(),
               "Cycles with at least one demand walk in service"),
      ADD_STAT(WALK_PENDING, statistics::units::Cycle::get(),
               "Integral of demand walks in service; excludes queued requests"),
      ADD_STAT(COALESCED_REQUESTS, statistics::units::Count::get(),
               "Model translations joined to an in-flight walk")
{}

void
Walker::accountDataWalks()
{
    const uint64_t now = uint64_t(curCycle());
    const uint64_t elapsed = now - lastDataWalkCycle;
    if (activeDataLoads) dtlbLoadStats.WALK_ACTIVE += elapsed;
    if (activeDataStores) dtlbStoreStats.WALK_ACTIVE += elapsed;
    dtlbLoadStats.WALK_PENDING += elapsed * activeDataLoads;
    dtlbStoreStats.WALK_PENDING += elapsed * activeDataStores;
    lastDataWalkCycle = now;
}

void Walker::DtlbWalkStats::preDumpStats()
{
    owner->accountDataWalks();
    statistics::Group::preDumpStats();
}

void Walker::DtlbWalkStats::resetStats()
{
    owner->accountDataWalks();
    statistics::Group::resetStats();
}

void
Walker::accountInstructionWalks()
{
    const uint64_t now = uint64_t(curCycle());
    const uint64_t elapsed = now - lastInstructionWalkCycle;
    if (activeInstructionWalks) itlbStats.WALK_ACTIVE += elapsed;
    itlbStats.WALK_PENDING += elapsed * activeInstructionWalks;
    lastInstructionWalkCycle = now;
}

void Walker::ItlbStats::preDumpStats()
{
    owner->accountInstructionWalks();
    statistics::Group::preDumpStats();
}

void Walker::ItlbStats::resetStats()
{
    owner->accountInstructionWalks();
    statistics::Group::resetStats();
}

Fault
Walker::start(ThreadContext * _tc, BaseMMU::Translation *_translation,
              const RequestPtr &_req, BaseMMU::Mode _mode)
{
    WalkerState * newState = new WalkerState(this, _translation, _req);
    newState->initState(_tc, _mode, sys->isTimingMode());
    if (!newState->isTiming()) {
        assert(currStates.empty());
        Fault fault = newState->startWalk();
        delete newState;
        return fault;
    }
    // A walk may be shared only while its translation context is unchanged.
    // Limit joining to a 4 KiB subpage even for a later large-page leaf.
    // Keep prefetches separate so a prefetch leader cannot conceal a demand
    // walk from the demand activity accounting.
    const uint64_t cr3 = _tc->readMiscRegNoEffect(misc_reg::Cr3);
    const uint64_t cr4 = _tc->readMiscRegNoEffect(misc_reg::Cr4);
    const uint64_t efer = _tc->readMiscRegNoEffect(misc_reg::Efer);
    for (auto *walk : currStates) {
        // recvPacket marks a completed walk Ready before invoking callbacks.
        // A callback may reenter start() before recvTimingResp removes it;
        // never attach a follower to that already-completed leader.
        if (!walk->wasStarted() || walk->state == WalkerState::Ready ||
            walk->leader || walk->squashed ||
            walk->tc != _tc || walk->requestMode != _mode ||
            walk->req->isPrefetch() != _req->isPrefetch() ||
            walk->translationGeneration != tlb->generation() ||
            walk->walkCr3 != cr3 || walk->walkCr4 != cr4 ||
            walk->walkEfer != efer ||
            (walk->req->getVaddr() >> 12) != (_req->getVaddr() >> 12))
            continue;
        newState->leader = walk;
        walk->followers.push_back(newState);
        currStates.push_back(newState);
        if (_mode == BaseMMU::Execute)
            ++itlbStats.COALESCED_REQUESTS;
        else if (_mode == BaseMMU::Read)
            ++dtlbLoadStats.COALESCED_REQUESTS;
        else
            ++dtlbStoreStats.COALESCED_REQUESTS;
        return NoFault;
    }
    currStates.push_back(newState);
    startWalkWrapper();
    return NoFault;
}

Fault
Walker::startFunctional(ThreadContext * _tc, Addr &addr, unsigned &logBytes,
              BaseMMU::Mode _mode)
{
    funcState.initState(_tc, _mode);
    return funcState.startFunctional(addr, logBytes);
}

bool
Walker::WalkerPort::recvTimingResp(PacketPtr pkt)
{
    return walker->recvTimingResp(pkt);
}

bool
Walker::recvTimingResp(PacketPtr pkt)
{
    WalkerSenderState * senderState =
        dynamic_cast<WalkerSenderState *>(pkt->popSenderState());
    WalkerState * senderWalk = senderState->senderWalk;
    bool walkComplete = senderWalk->recvPacket(pkt);
    delete senderState;
    if (walkComplete) {
        if (senderWalk->requestMode == BaseMMU::Execute && senderWalk->wasStarted()) {
            accountInstructionWalks();
            assert(activeInstructionWalks);
            --activeInstructionWalks;
        }
        if (senderWalk->requestMode != BaseMMU::Execute &&
            senderWalk->wasStarted() && !senderWalk->req->isPrefetch()) {
            accountDataWalks();
            if (senderWalk->requestMode == BaseMMU::Read)
                --activeDataLoads;
            else
                --activeDataStores;
        }
        std::list<WalkerState *>::iterator iter;
        for (iter = currStates.begin(); iter != currStates.end(); iter++) {
            WalkerState * walkerState = *(iter);
            if (walkerState == senderWalk) {
                iter = currStates.erase(iter);
                break;
            }
        }
        delete senderWalk;
        // Since we block requests when another is outstanding, we
        // need to check if there is a waiting request to be serviced
        if (currStates.size() && !startWalkWrapperEvent.scheduled())
            // delay sending any new requests until we are finished
            // with the responses
            schedule(startWalkWrapperEvent, clockEdge());
    }
    return true;
}

void
Walker::WalkerPort::recvReqRetry()
{
    walker->recvReqRetry();
}

void
Walker::recvReqRetry()
{
    portBlocked = false;
    std::list<WalkerState *>::iterator iter;
    for (iter = currStates.begin(); iter != currStates.end(); iter++) {
        WalkerState * walkerState = *(iter);
        if (walkerState->isRetrying()) {
            walkerState->retry();
            if (portBlocked) break;
        }
    }
}

bool Walker::sendTiming(WalkerState* sendingState, PacketPtr pkt)
{
    if (portBlocked) return false;
    WalkerSenderState* walker_state = new WalkerSenderState(sendingState);
    pkt->pushSenderState(walker_state);
    if (port.sendTimingReq(pkt)) {
        return true;
    } else {
        portBlocked = true;
        // undo the adding of the sender state and delete it, as we
        // will do it again the next time we attempt to send it
        pkt->popSenderState();
        delete walker_state;
        return false;
    }

}

Port &
Walker::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    else
        return ClockedObject::getPort(if_name, idx);
}

void
Walker::WalkerState::initState(ThreadContext * _tc,
        BaseMMU::Mode _mode, bool _isTiming)
{
    assert(state == Ready);
    started = false;
    tc = _tc;
    mode = _mode;
    requestMode = _mode;
    timing = _isTiming;
}

void
Walker::startWalkWrapper()
{
    // Completing a queued squash can synchronously start another translation.
    // Defer that nested pass so it cannot erase the outer list iterator.
    if (startingWalks) {
        if (!startWalkWrapperEvent.scheduled())
            schedule(startWalkWrapperEvent, clockEdge(Cycles(1)));
        return;
    }
    startingWalks = true;
    unsigned active = 0, num_squashed = 0;
    for (auto *walk : currStates)
        if (walk->wasStarted()) ++active;
    bool more_squashes = false;
    for (auto it = currStates.begin(); it != currStates.end();) {
        WalkerState *walk = *it;
        if (walk->wasStarted() || walk->leader) { ++it; continue; }
        if (walk->translation->squashed()) {
            if (num_squashed >= numSquashable) {
                more_squashes = true;
                ++it;
                continue;
            }
            it = currStates.erase(it);
            ++num_squashed;
            walk->translation->finish(std::make_shared<UnimpFault>("Squashed Inst"),
                                      walk->req, walk->tc, walk->mode);
            delete walk;
        } else if (active < maxActiveWalks) {
            ++it;
            ++active;
            walk->startWalk();
        } else {
            ++it;
        }
    }
    startingWalks = false;
    if (more_squashes && !startWalkWrapperEvent.scheduled())
        schedule(startWalkWrapperEvent, clockEdge(Cycles(1)));
}

Fault
Walker::WalkerState::startWalk()
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    if (timing && mode == BaseMMU::Execute) {
        walker->accountInstructionWalks();
        ++walker->activeInstructionWalks;
    } else if (timing && !req->isPrefetch()) {
        walker->accountDataWalks();
        if (requestMode == BaseMMU::Read)
            ++walker->activeDataLoads;
        else
            ++walker->activeDataStores;
    }
    setupWalk(req->getVaddr());
    if (timing) {
        nextState = state;
        state = Waiting;
        timingFault = NoFault;
        if (prefixHit) {
            sendPrefixResponse();
        } else {
            sendPackets();
        }
    } else {
        do {
            walker->port.sendAtomic(read);
            PacketPtr write = NULL;
            fault = stepWalk(write);
            assert(fault == NoFault || read == NULL);
            state = nextState;
            nextState = Ready;
            if (write) {
                walker->port.sendAtomic(write);
                // delete the write packet as it is not needed anymore
                delete write;
            }
        } while (read);
        state = Ready;
        nextState = Waiting;
    }
    return fault;
}

void
Walker::WalkerState::sendPrefixResponse()
{
    // A delayed prefix response owns no FB or PTE memory transaction. Reuse
    // walk completion so cancellation and context races have one owner.
    assert(prefixHit && read);
    PacketPtr cached = read;
    read = nullptr;
    cached->makeTimingResponse();
    cached->pushSenderState(new WalkerSenderState(this));
    ++inflight;
    Walker *owner = walker;
    owner->schedule(new EventFunctionWrapper(
        [owner, cached] { owner->recvTimingResp(cached); },
        owner->name() + ".prefixResponse", true),
        owner->clockEdge(owner->prefixHitLatency));
}

Fault
Walker::WalkerState::startFunctional(Addr &addr, unsigned &logBytes)
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    setupWalk(addr);

    do {
        walker->port.sendFunctional(read);
        // On a functional access (page table lookup), writes should
        // not happen so this pointer is ignored after stepWalk
        PacketPtr write = NULL;
        fault = stepWalk(write);
        assert(fault == NoFault || read == NULL);
        // delete the write packet if it exists
        if (write) {
            delete write;
        }
        state = nextState;
        nextState = Ready;
    } while (read);
    logBytes = entry.logBytes;
    addr = entry.paddr;

    return fault;
}

Fault
Walker::WalkerState::stepWalk(PacketPtr &write)
{
    assert(state != Ready && state != Waiting);
    const bool fromPrefix = prefixHit;
    prefixHit = false;
    if (!fromPrefix)
        recordFunctionalTranslationPte(entry.functionalTranslation,
                                       read->getAddr());
    Fault fault = NoFault;
    write = NULL;
    PageTableEntry pte;
    if (dataSize == 8)
        pte = read->getLE<uint64_t>();
    else
        pte = read->getLE<uint32_t>();
    VAddr vaddr = entry.vaddr;
    bool uncacheable = pte.pcd;
    Addr nextRead = 0;
    bool doWrite = false;
    if (state == LongPTE || state == PAEPTE || state == PTE)
        observedLeafLogBytes = 12;
    else if (pte.p && pte.ps) {
        if (state == LongPDP) observedLeafLogBytes = 30;
        if (state == LongPD || state == PAEPD) observedLeafLogBytes = 21;
        if (state == PSEPD) observedLeafLogBytes = 22;
    }
    bool doTLBInsert = false;
    bool doEndWalk = false;
    bool badNX = pte.nx && mode == BaseMMU::Execute && enableNX;
    if (dataSize == 8 && enableNX)
        entry.noExec = entry.noExec || pte.nx;
    if ((state == LongPML4 || state == LongPDP || state == LongPD || state == LongPTE) &&
        pte.p && ((pte.nx && !enableNX) ||
                  (state == LongPML4 && pte.ps) ||
                  (physicalAddressBits < 52 &&
                   bits(uint64_t(pte), 51, physicalAddressBits)))) {
        endWalk();
        return pageFault(true, true);
    }
    switch(state) {
      case LongPML4:
        DPRINTF(PageTableWalker, "Got long mode PML4 entry %#016x.\n", pte);
        nextRead = mbits(pte, 51, 12) + vaddr.longl3 * dataSize;
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        nextState = LongPDP;
        break;
      case LongPDP:
        DPRINTF(PageTableWalker, "Got long mode PDP entry %#016x.\n", pte);
        nextRead = mbits(pte, 51, 12) + vaddr.longl2 * dataSize;
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (pte.ps) {
            CpuidResult features;
            auto *isa = static_cast<ISA *>(tc->getIsaPtr());
            isa->cpuid->doCpuid(tc, 0x80000001, 0, features);
            if (!(features.rdx & (1U << 26)) ||
                bits(uint64_t(pte), 29, 13) || bits(uint64_t(pte), 51, 48)) {
                doEndWalk = true;
                doWrite = false;
                fault = pageFault(true, true);
                break;
            }
            entry.logBytes = 30;
            entry.paddr = mbits(pte, 51, 30);
            entry.uncacheable = uncacheable;
            entry.global = enableGlobal && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 30);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
        nextState = LongPD;
        break;
      case LongPD:
        DPRINTF(PageTableWalker, "Got long mode PD entry %#016x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (!pte.ps) {
            // 4 KB page
            entry.logBytes = 12;
            nextRead = mbits(pte, 51, 12) + vaddr.longl1 * dataSize;
            nextState = LongPTE;
            break;
        } else {
            // 2 MB page
            entry.logBytes = 21;
            entry.paddr = mbits(pte, 51, 21);
            entry.uncacheable = uncacheable;
            entry.global = enableGlobal && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 21);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
      case LongPTE:
        DPRINTF(PageTableWalker, "Got long mode PTE entry %#016x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        entry.paddr = mbits(pte, 51, 12);
        entry.uncacheable = uncacheable;
        entry.global = enableGlobal && pte.g;
        entry.patBit = bits(pte, 12);
        entry.vaddr = mbits(entry.vaddr, 63, 12);
        doTLBInsert = true;
        doEndWalk = true;
        break;
      case PAEPDP:
        DPRINTF(PageTableWalker,
                "Got legacy mode PAE PDP entry %#08x.\n", pte);
        nextRead = mbits(pte, 51, 12) + vaddr.pael2 * dataSize;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        nextState = PAEPD;
        break;
      case PAEPD:
        DPRINTF(PageTableWalker, "Got legacy mode PAE PD entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (!pte.ps) {
            // 4 KB page
            entry.logBytes = 12;
            nextRead = mbits(pte, 51, 12) + vaddr.pael1 * dataSize;
            nextState = PAEPTE;
            break;
        } else {
            // 2 MB page
            entry.logBytes = 21;
            entry.paddr = mbits(pte, 51, 21);
            entry.uncacheable = uncacheable;
            entry.global = enableGlobal && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 21);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
      case PAEPTE:
        DPRINTF(PageTableWalker,
                "Got legacy mode PAE PTE entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        entry.paddr = mbits(pte, 51, 12);
        entry.uncacheable = uncacheable;
        entry.global = enableGlobal && pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = mbits(entry.vaddr, 63, 12);
        doTLBInsert = true;
        doEndWalk = true;
        break;
      case PSEPD:
        DPRINTF(PageTableWalker, "Got legacy mode PSE PD entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (!pte.ps) {
            // 4 KB page
            entry.logBytes = 12;
            nextRead = mbits(pte, 31, 12) + vaddr.norml2 * dataSize;
            nextState = PTE;
            break;
        } else {
            // 4 MB page
            entry.logBytes = 22;
            entry.paddr = bits(pte, 20, 13) << 32 | mbits(pte, 31, 22);
            entry.uncacheable = uncacheable;
            entry.global = enableGlobal && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 22);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
      case PD:
        DPRINTF(PageTableWalker, "Got legacy mode PD entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        // 4 KB page
        entry.logBytes = 12;
        nextRead = mbits(pte, 31, 12) + vaddr.norml1 * dataSize;
        nextState = PTE;
        break;
      case PTE:
        DPRINTF(PageTableWalker, "Got legacy mode PTE entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        entry.paddr = mbits(pte, 31, 12);
        entry.uncacheable = uncacheable;
        entry.global = enableGlobal && pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = mbits(entry.vaddr, 31, 12);
        doTLBInsert = true;
        doEndWalk = true;
        break;
      default:
        panic("Unknown page table walker state %d!\n");
    }
    if (state == LongPML4 || state == LongPDP || state == LongPD) {
        // SDM 5.10.3 requires every relevant upper Accessed flag to be set in
        // memory before caching. An outstanding A-bit write keeps this walk
        // uncached; later walks can fill after that write has completed.
        cacheablePrefix &= !doWrite;
        if (!fromPrefix && timing && walker->pagingStructureCache &&
            cacheablePrefix && !doEndWalk && fault == NoFault &&
            translationGeneration == walker->tlb->generation()) {
            const unsigned level = state == LongPML4 ? 0 : state == LongPDP ? 1 : 2;
            PageTableEntry aggregate = pte;
            aggregate.w = entry.writable;
            aggregate.u = entry.user;
            aggregate.nx = entry.noExec;
            walker->pagingStructureCache->insert({level, walkCr3, walkCr4, walkEfer,
                entry.pcid, (entry.vaddr & mask(48)) >> PagingStructureCache::shift(level),
                read->getAddr(), uint64_t(aggregate), entry.functionalTranslation});
        }
    }
    if (doTLBInsert) {
        entry.pwt = pte.pwt;
        completeFunctionalTranslationLeaf(entry.functionalTranslation,
            entry.vaddr, entry.paddr, entry.logBytes, entry.global);
    }
    if (doEndWalk) {
        if (doTLBInsert && !functional && !timing)
            walker->tlb->insert(entry.vaddr, entry, entry.pcid,
                                true, requestMode);

        endWalk();
    } else {
        PacketPtr oldRead = read;
        //If we didn't return, we're setting up another read.
        Request::Flags flags = oldRead->req->getFlags();
        flags.set(Request::UNCACHEABLE, uncacheable);
        RequestPtr request = std::make_shared<Request>(
            nextRead, oldRead->getSize(), flags, walker->requestorId);
        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
        // If we need to write, adjust the read packet to write the modified
        // value back to memory.
        if (doWrite) {
            write = oldRead;
            if (dataSize == 8)
                write->setLE<uint64_t>(pte);
            else
                write->setLE<uint32_t>(pte);
            write->cmd = MemCmd::WriteReq;
        } else {
            write = NULL;
            delete oldRead;
        }
    }
    return fault;
}

void
Walker::WalkerState::endWalk()
{
    nextState = Ready;
    delete read;
    read = NULL;
}

void
Walker::WalkerState::setupWalk(Addr vaddr)
{
    observedLeafLogBytes = 0;
    prefixHit = false;
    cacheablePrefix = true;
    if (timing) translationGeneration = walker->tlb->generation();
    VAddr addr = vaddr;
    CR3 cr3 = tc->readMiscRegNoEffect(misc_reg::Cr3);
    walkCr3 = uint64_t(cr3);
    CR4 cr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
    walkCr4 = uint64_t(cr4);
    entry.pcid = cr4.pcide ? uint64_t(cr3.pcid) : 0;
    enableGlobal = cr4.pge;
    // Check if we're in long mode or not
    Efer efer = tc->readMiscRegNoEffect(misc_reg::Efer);
    walkEfer = uint64_t(efer);
    CpuidResult width;
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    if (isa->cpuid->doCpuid(tc, 0x80000008, 0, width))
        physicalAddressBits = bits(width.rax, 7, 0);
    fatal_if(physicalAddressBits < 32 || physicalAddressBits > 52,
             "Unsupported x86 physical address width for paging");
    dataSize = 8;
    Addr topAddr;
    if (efer.lma) {
        // Do long mode.
        state = LongPML4;
        topAddr = (cr3.longPdtb << 12) + addr.longl4 * dataSize;
        enableNX = efer.nxe;
    } else {
        // We're in some flavor of legacy mode.
        if (cr4.pae) {
            // Do legacy PAE.
            state = PAEPDP;
            topAddr = (cr3.paePdtb << 5) + addr.pael3 * dataSize;
            enableNX = efer.nxe;
        } else {
            dataSize = 4;
            topAddr = (cr3.pdtb << 12) + addr.norml2 * dataSize;
            if (cr4.pse) {
                // Do legacy PSE.
                state = PSEPD;
            } else {
                // Do legacy non PSE.
                state = PD;
            }
            enableNX = false;
        }
    }

    nextState = Ready;
    entry.vaddr = vaddr;
    entry.noExec = false;
    entry.writable = entry.user = true;

    const Addr root = efer.lma ? Addr(cr3.longPdtb) << 12 :
        (cr4.pae ? Addr(cr3.paePdtb) << 5 : Addr(cr3.pdtb) << 12);
    beginFunctionalTranslationWalk(entry.functionalTranslation, root);

    PagingStructureCache::Prefix prefix;
    if (timing && efer.lma && walker->pagingStructureCache &&
        walker->pagingStructureCache->lookup(walkCr3, walkCr4, walkEfer,
                                            entry.pcid, vaddr, prefix)) {
        prefixHit = true;
        state = prefix.level == 0 ? LongPML4 : prefix.level == 1 ? LongPDP : LongPD;
        topAddr = prefix.pteAddress;
        entry.functionalTranslation = prefix.provenance;
    }

    Request::Flags flags = Request::PHYSICAL;
    flags.set(Request::PT_WALK);

    // PCD can't be used if CR4.PCIDE=1 [sec 2.5
    // of Intel's Software Developer's manual]
    if (!cr4.pcide && cr3.pcd)
        flags.set(Request::UNCACHEABLE);

    RequestPtr request = std::make_shared<Request>(
        topAddr, dataSize, flags, walker->requestorId);

    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
    if (prefixHit)
        read->setLE<uint64_t>(prefix.pte);
}

bool
Walker::WalkerState::recvPacket(PacketPtr pkt)
{
    assert(pkt->isResponse());
    assert(inflight);
    assert(state == Waiting);
    inflight--;
    if (squashed) {
        // if were were squashed, return true once inflight is zero and
        // this WalkerState will be freed there.
        delete pkt;
        return (inflight == 0);
    }
    if (pkt->isRead()) {
        // should not have a pending read it we also had one outstanding
        assert(!read);

        // @todo someone should pay for this
        pkt->headerDelay = pkt->payloadDelay = 0;

        state = nextState;
        nextState = Ready;
        PacketPtr write = NULL;
        read = pkt;
        timingFault = stepWalk(write);
        state = Waiting;
        assert(timingFault == NoFault || read == NULL);
        if (write) {
            writes.push_back(write);
        }
        sendPackets();
    } else {
        sendPackets();
    }
    if (inflight == 0 && read == NULL && writes.size() == 0) {
        state = Ready;
        nextState = Waiting;
        if (translationGeneration != walker->tlb->generation() ||
            walkCr3 != tc->readMiscRegNoEffect(misc_reg::Cr3) ||
            walkCr4 != tc->readMiscRegNoEffect(misc_reg::Cr4) ||
            walkEfer != tc->readMiscRegNoEffect(misc_reg::Efer)) {
            // A changed context must not keep requests joined to this walk.
            releaseFollowers(false);
            if (translation->squashed()) {
                translation->finish(std::make_shared<UnimpFault>("Squashed Inst"),
                                    req, tc, mode);
                return true;
            }
            // An invalidate raced this walk. Re-read the page tables rather
            // than installing a stale leaf or reporting its stale fault.
            entry = TlbEntry();
            timingFault = NoFault;
            setupWalk(req->getVaddr());
            nextState = state;
            state = Waiting;
            if (prefixHit)
                sendPrefixResponse();
            else
                sendPackets();
            return false;
        }
        if (requestMode == BaseMMU::Execute)
            ++walker->itlbStats.WALK_COMPLETED;
        if (requestMode == BaseMMU::Read && timing && !req->isPrefetch())
            ++walker->dtlbLoadStats.WALK_COMPLETED;
        if (requestMode == BaseMMU::Write && timing && !req->isPrefetch())
            ++walker->dtlbStoreStats.WALK_COMPLETED;
        {
            if (requestMode == BaseMMU::Execute && observedLeafLogBytes == 12)
                ++walker->itlbStats.WALK_COMPLETED_4K;
            if (requestMode == BaseMMU::Execute &&
                (observedLeafLogBytes == 21 || observedLeafLogBytes == 22))
                ++walker->itlbStats.WALK_COMPLETED_2M_4M;
            if (requestMode == BaseMMU::Execute && observedLeafLogBytes == 30)
                ++walker->itlbStats.WALK_COMPLETED_1G;
            if (requestMode == BaseMMU::Execute && !observedLeafLogBytes)
                ++walker->itlbStats.unclassifiedCompleted;
            if (requestMode != BaseMMU::Execute && timing && !req->isPrefetch()) {
                auto &stats = requestMode == BaseMMU::Read ?
                    walker->dtlbLoadStats : walker->dtlbStoreStats;
                if (observedLeafLogBytes == 12) ++stats.WALK_COMPLETED_4K;
                if (observedLeafLogBytes == 21 || observedLeafLogBytes == 22)
                    ++stats.WALK_COMPLETED_2M_4M;
                if (observedLeafLogBytes == 30) ++stats.WALK_COMPLETED_1G;
                if (!observedLeafLogBytes) ++stats.unclassifiedCompleted;
            }
        }
        if (timingFault == NoFault) {
            /*
             * Finish the translation. Now that we know the right entry is
             * in the TLB, this should work with no memory accesses.
             * There could be new faults unrelated to the table walk like
             * permissions violations, so we'll need the return value as
             * well.
             */
            // Install only once all walk packets have completed, then perform
            // the permission check before another walk can evict this leaf.
            walker->tlb->insert(entry.vaddr, entry, entry.pcid,
                                true, requestMode);
            bool delayedResponse;
            Fault fault = walker->tlb->translate(req, tc, NULL, mode,
                                                 delayedResponse, true);
            assert(!delayedResponse);
            releaseFollowers(true);
            // Let the CPU continue.
            translation->finish(fault, req, tc, mode);
        } else {
            // A fault is request-specific (including its fault address).
            // Rewalk followers instead of copying the leader's fault.
            releaseFollowers(false);
            // There was a fault during the walk. Let the CPU know.
            translation->finish(timingFault, req, tc, mode);
        }
        return true;
    }

    return false;
}

void
Walker::WalkerState::releaseFollowers(bool completed)
{
    auto waiting = std::move(followers);
    followers.clear();
    if (!completed) {
        for (auto *follower : waiting)
            follower->leader = nullptr;
        return;
    }

    // Perform all TLB lookups before callbacks can initiate another walk and
    // evict the newly installed leaf.  Each request retains its own privilege,
    // fault, address and squash handling.
    struct Completion {
        WalkerState *state;
        Fault fault;
    };
    std::vector<Completion> ready;
    ready.reserve(waiting.size());
    for (auto *follower : waiting) {
        walker->currStates.remove(follower);
        Fault fault;
        if (follower->translation->squashed()) {
            fault = std::make_shared<UnimpFault>("Squashed Inst");
        } else {
            bool delayed = false;
            fault = walker->tlb->translate(follower->req, follower->tc,
                nullptr, follower->mode, delayed, true);
            assert(!delayed);
        }
        ready.push_back({follower, fault});
    }
    for (auto &completion : ready) {
        auto *follower = completion.state;
        follower->translation->finish(completion.fault, follower->req,
                                      follower->tc, follower->mode);
        delete follower;
    }
}

void
Walker::WalkerState::sendPackets()
{
    //If we're already waiting for the port to become available, just return.
    if (retrying)
        return;

    //Reads always have priority
    if (read) {
        PacketPtr pkt = read;
        read = NULL;
        inflight++;
        if (!walker->sendTiming(this, pkt)) {
            retrying = true;
            read = pkt;
            inflight--;
            return;
        }
    }
    //Send off as many of the writes as we can.
    while (writes.size()) {
        PacketPtr write = writes.back();
        writes.pop_back();
        inflight++;
        if (!walker->sendTiming(this, write)) {
            retrying = true;
            writes.push_back(write);
            inflight--;
            return;
        }
    }
}

unsigned
Walker::WalkerState::numInflight() const
{
    return inflight;
}

bool
Walker::WalkerState::isRetrying()
{
    return retrying;
}

bool
Walker::WalkerState::isTiming()
{
    return timing;
}

bool
Walker::WalkerState::wasStarted()
{
    return started;
}

void
Walker::WalkerState::squash()
{
    squashed = true;
}

void
Walker::WalkerState::retry()
{
    retrying = false;
    sendPackets();
}

Fault
Walker::WalkerState::pageFault(bool present, bool reserved)
{
    DPRINTF(PageTableWalker, "Raising page fault.\n");
    HandyM5Reg m5reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    if (mode == BaseMMU::Execute && !enableNX)
        mode = BaseMMU::Read;
    return std::make_shared<PageFault>(entry.vaddr, present, mode,
                                       m5reg.cpl == 3, reserved);
}

} // namespace X86ISA
} // namespace gem5
