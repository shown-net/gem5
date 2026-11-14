/*
 * Copyright (c) 2010-2012, 2014-2019, 2025 Arm Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#include "cpu/o3/rename.hh"

#include <list>

#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/reg_class.hh"
#include "debug/Activity.hh"
#include "debug/Rename.hh"
#include "params/BaseO3CPU.hh"
#include "cpu/o3/zero_idiom.hh"
#include "cpu/o3/stack_engine.hh"
#include "cpu/o3/frontend_record.hh"
#include "arch/x86/insts/macroop.hh"
#include "arch/x86/regs/int.hh"
#include "debug/StackEngine.hh"

namespace gem5
{

namespace o3
{

namespace
{
bool
isNearControlConstant(const DynInstPtr &inst)
{
    const auto kind = nearStackControl(inst->macroop.get());
    const bool call = kind == NearStackControl::DirectCall ||
        kind == NearStackControl::RegisterCall ||
        kind == NearStackControl::MemoryCall;
    return (call && (inst->staticInst->getName() == "rdip" ||
        (kind == NearStackControl::DirectCall && inst->isCall()))) ||
        (kind == NearStackControl::ReturnImmediate &&
         inst->staticInst->getName() == "limm");
}

// Only the flag-free, implicit 64-bit PUSH register update is eligible.
// Keep the functional micro-op for other CPU models and the unready case.
bool
isImplicitPushUpdate(const DynInstPtr &inst)
{
    if (!inst->isLastMicroop() || inst->isSquashed() ||
        inst->getFault() != NoFault || inst->staticInst->getName() != "subi" ||
        inst->numSrcRegs() != 1 || inst->numDestRegs() != 1)
        return false;
    auto *macro = dynamic_cast<X86ISA::MacroopBase *>(inst->macroop.get());
    if (!macro || macro->macroopKind() != MacroopKind::PushRegister)
        return false;
    const auto env = macro->getEmulEnv();
    if (env.stackSize != 8 || env.dataSize != 8)
        return false;
    auto *isa = inst->tcBase()->getIsaPtr();
    const auto rsp = X86ISA::int_reg::Rsp.flatten(*isa);
    return inst->srcRegIdx(0).flatten(*isa) == rsp &&
           inst->destRegIdx(0).flatten(*isa) == rsp;
}
}

// clang-format off
std::string Rename::RenameStats::statusStrings[ThreadStatusMax] = {
    "Running",
    "Idle",
    "StartSquash",
    "Squashing",
    "Blocked",
    "Unblocking",
    "SerializeStall",
};

std::string Rename::RenameStats::statusDefinitions[ThreadStatusMax] = {
    "Number of cycles rename is running",
    "Number of cycles rename is idle",
    "Not Used",
    "Number of cycles rename is squashing",
    "Number of cycles rename is blocking",
    "Number of cycles rename is unblocking",
    "Number of cycles rename stalled for serializing inst",
};
// clang-format on

Rename::Rename(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      iewToRenameDelay(params.iewToRenameDelay),
      decodeToRenameDelay(params.frontend ? Cycles(0) : params.decodeToRenameDelay),
      commitToRenameDelay(params.commitToRenameDelay),
      renameWidth(params.renameWidth),
      numThreads(params.numThreads),
      resourceStallStats(_cpu),
      stats(_cpu)
{
    if (renameWidth > MaxWidth)
        fatal("renameWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             renameWidth, static_cast<int>(MaxWidth));

    // @todo: Make into a parameter.
    skidBufferMax = (decodeToRenameDelay + 1) * params.decodeWidth;
    if (params.frontend) skidBufferMax = 2 * params.frontend->idq->capacityValue();
    for (uint32_t tid = 0; tid < MaxThreads; tid++) {
        renameStatus[tid] = Idle;
        renameMap[tid] = nullptr;
        instsInProgress[tid] = 0;
        loadsInProgress[tid] = 0;
        storesInProgress[tid] = 0;
        freeEntries[tid] = {0, 0, 0, 0};
        emptyROB[tid] = true;
        stalls[tid] = {false, false};
        serializeInst[tid] = nullptr;
        serializeOnNextInst[tid] = false;
    }
}

Rename::ResourceStallStats::ResourceStallStats(statistics::Group *parent)
    : statistics::Group(parent, "RESOURCE_STALLS"),
      ADD_STAT(SB, statistics::units::Cycle::get(),
               "Candidate cycles an allocation group needs unavailable store capacity"),
      ADD_STAT(SB_ROB_OVERLAP, statistics::units::Cycle::get(),
               "Candidate SB cycles also lacking ROB reservation"),
      ADD_STAT(SB_IQ_OVERLAP, statistics::units::Cycle::get(),
               "Candidate SB cycles also lacking IQ reservation"),
      ADD_STAT(SB_FRONTEND_BLOCKED, statistics::units::Cycle::get(),
               "Candidate SB cycles with rename backpressure or IEW stall"),
      ADD_STAT(SCOREBOARD, statistics::units::Cycle::get(),
               "Observed rename ticks stalled by a serializing operation; not register dependencies")
{}

std::string
Rename::name() const
{
    return cpu->name() + ".rename";
}

Rename::RenameStats::RenameStats(statistics::Group *parent)
    : statistics::Group(parent, "rename"),
      ADD_STAT(status, statistics::units::Cycle::get(),
               "Number of cycles spent in each rename state"),
      ADD_STAT(renamedInsts, statistics::units::Count::get(),
               "Number of instructions processed by rename"),
      ADD_STAT(recoveryAllocationSlots, statistics::units::Count::get(),
               "Correct-path allocation slots accepted while old ROB tracking cleanup continues"),
      ADD_STAT(pushUpdatesComputed, statistics::units::Count::get(),
               "Implicit 64-bit register PUSH updates computed at rename"),
      ADD_STAT(pushUpdatesDeferred, statistics::units::Count::get(),
               "Implicit 64-bit register PUSH updates sent to execution"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions processed by rename"),
      ADD_STAT(ROBFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to ROB full"),
      ADD_STAT(IQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to IQ full"),
      ADD_STAT(LQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to LQ full"),
      ADD_STAT(SQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to SQ full"),
      ADD_STAT(fullRegistersEvents, statistics::units::Count::get(),
               "Number of times there has been no free registers"),
      ADD_STAT(fullRegistersByClass, statistics::units::Count::get(),
               "Register classes short on a failed rename check; may overlap"),
      ADD_STAT(renamedOperands, statistics::units::Count::get(),
               "Number of destination operands rename has renamed"),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of register rename lookups that rename has made"),
      ADD_STAT(intLookups, statistics::units::Count::get(),
               "Number of integer rename lookups"),
      ADD_STAT(fpLookups, statistics::units::Count::get(),
               "Number of floating rename lookups"),
      ADD_STAT(vecLookups, statistics::units::Count::get(),
               "Number of vector rename lookups"),
      ADD_STAT(vecPredLookups, statistics::units::Count::get(),
               "Number of vector predicate rename lookups"),
      ADD_STAT(matLookups, statistics::units::Count::get(),
               "Number of matrix rename lookups"),
      ADD_STAT(committedMaps, statistics::units::Count::get(),
               "Number of HB maps that are committed"),
      ADD_STAT(undoneMaps, statistics::units::Count::get(),
               "Number of HB maps that are undone due to squashing"),
      ADD_STAT(serializing, statistics::units::Count::get(),
               "count of serializing insts renamed"),
      ADD_STAT(tempSerializing, statistics::units::Count::get(),
               "count of temporary serializing insts renamed"),
      ADD_STAT(skidInsts, statistics::units::Count::get(),
               "count of insts added to the skid buffer"),
      ADD_STAT(intReturned, statistics::units::Count::get(),
               "count of registers freed and written back to integer free list"),
      ADD_STAT(fpReturned, statistics::units::Count::get(),
               "count of registers freed and written back to floating point free list")

{
    status.init(ThreadStatusMax).flags(statistics::pdf | statistics::nozero);
    for (int i = 0; i < ThreadStatusMax; ++i) {
        status.subname(i, statusStrings[i]);
        status.subdesc(i, statusDefinitions[i]);
    }

    renamedInsts.prereq(renamedInsts);
    squashedInsts.prereq(squashedInsts);

    ROBFullEvents.prereq(ROBFullEvents);
    IQFullEvents.prereq(IQFullEvents);
    LQFullEvents.prereq(LQFullEvents);
    SQFullEvents.prereq(SQFullEvents);
    fullRegistersEvents.prereq(fullRegistersEvents);
    fullRegistersByClass.init(CCRegClass + 1);
    const char *const registerClasses[] = {
        "integer", "floating_point", "vector", "vector_element",
        "vector_predicate", "matrix", "condition_code"
    };
    for (int i = IntRegClass; i <= CCRegClass; ++i)
        fullRegistersByClass.subname(i, registerClasses[i]);

    renamedOperands.prereq(renamedOperands);
    lookups.prereq(lookups);
    intLookups.prereq(intLookups);
    fpLookups.prereq(fpLookups);
    vecLookups.prereq(vecLookups);
    vecPredLookups.prereq(vecPredLookups);
    matLookups.prereq(matLookups);

    committedMaps.prereq(committedMaps);
    undoneMaps.prereq(undoneMaps);
    serializing.flags(statistics::total);
    tempSerializing.flags(statistics::total);
    skidInsts.flags(statistics::total);

    intReturned.prereq(intReturned);
    fpReturned.prereq(fpReturned);
}

void
Rename::regProbePoints()
{
    ppRename = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Rename");
    ppSquashInRename = new ProbePointArg<SeqNumRegPair>(cpu->getProbeManager(),
                                                        "SquashInRename");
}

void
Rename::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from IEW stage.
    fromIEW = timeBuffer->getWire(-iewToRenameDelay);

    // Setup wire to read infromation from time buffer, from commit stage.
    fromCommit = timeBuffer->getWire(-commitToRenameDelay);

    // Setup wire to write information to previous stages.
    toDecode = timeBuffer->getWire(0);
}

void
Rename::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to write information to future stages.
    toIEW = renameQueue->getWire(0);
}

void
Rename::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to get information from decode.
    fromDecode = decodeQueue->getWire(-decodeToRenameDelay);
}

void
Rename::startupStage()
{
    resetStage();
}

void
Rename::clearStates(ThreadID tid)
{
    renameStatus[tid] = Idle;

    freeEntries[tid].iqEntries = iew_ptr->instQueue.numFreeEntries(tid);
    freeEntries[tid].lqEntries = iew_ptr->ldstQueue.numFreeLoadEntries(tid);
    freeEntries[tid].sqEntries = iew_ptr->ldstQueue.numFreeStoreEntries(tid);
    freeEntries[tid].robEntries = commit_ptr->numROBFreeEntries(tid);
    emptyROB[tid] = true;

    stalls[tid].iew = false;
    serializeInst[tid] = NULL;

    instsInProgress[tid] = 0;
    loadsInProgress[tid] = 0;
    storesInProgress[tid] = 0;

    serializeOnNextInst[tid] = false;

    // Clear out any of this thread's instructions being sent to IEW.
    for (int i = -cpu->renameQueue.getPast();
         i <= cpu->renameQueue.getFuture(); ++i) {
        RenameStruct& rename_struct = cpu->renameQueue[i];
        removeCommThreadInsts(tid, rename_struct);
    }

    // Clear out any of this thread's instructions being sent to decode.
    for (int i = -cpu->timeBuffer.getPast();
         i <= cpu->timeBuffer.getFuture(); ++i) {
        TimeStruct& time_struct = cpu->timeBuffer[i];
        time_struct.renameInfo[tid] = {};
        time_struct.renameBlock[tid] = false;
        time_struct.renameUnblock[tid] = false;
    }
}

void
Rename::resetStage()
{
    _status = Inactive;

    resumeSerialize = false;
    resumeUnblocking = false;

    // Grab the number of free entries directly from the stages.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        renameStatus[tid] = Idle;

        freeEntries[tid].iqEntries = iew_ptr->instQueue.numFreeEntries(tid);
        freeEntries[tid].lqEntries =
            iew_ptr->ldstQueue.numFreeLoadEntries(tid);
        freeEntries[tid].sqEntries =
            iew_ptr->ldstQueue.numFreeStoreEntries(tid);
        freeEntries[tid].robEntries = commit_ptr->numROBFreeEntries(tid);
        emptyROB[tid] = true;

        stalls[tid].iew = false;
        serializeInst[tid] = NULL;

        instsInProgress[tid] = 0;
        loadsInProgress[tid] = 0;
        storesInProgress[tid] = 0;

        serializeOnNextInst[tid] = false;
    }
}

void
Rename::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}


void
Rename::setRenameMap(UnifiedRenameMap::PerThreadUnifiedRenameMap& rm_ptr)
{
    for (ThreadID tid = 0; tid < numThreads; tid++)
        renameMap[tid] = &rm_ptr[tid];
}

void
Rename::setFreeList(UnifiedFreeList *fl_ptr)
{
    freeList = fl_ptr;
}

void
Rename::setScoreboard(Scoreboard *_scoreboard)
{
    scoreboard = _scoreboard;
}

bool
Rename::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (instsInProgress[tid] != 0 || !historyBuffer[tid].empty() ||
            !skidBuffer[tid].empty() ||
            !insts[tid].empty() ||
            (renameStatus[tid] != Idle && renameStatus[tid] != Running))
            return false;
    }
    return true;
}

void
Rename::takeOverFrom()
{
    resetStage();
}

void
Rename::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        assert(historyBuffer[tid].empty());
        assert(insts[tid].empty());
        assert(skidBuffer[tid].empty());
        assert(instsInProgress[tid] == 0);
    }
}

void
Rename::squash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    if (cpu->stackEngine && cpu->stackEngine->enabled())
        cpu->stackEngine->squash(tid, squash_seq_num);
    commit_ptr->reorderBuffer().cancelReservations(tid, squash_seq_num);
    iew_ptr->instQueue.cancelReservations(tid, squash_seq_num);
    if (cpu->frontend) cpu->frontend->cancelAfter(squash_seq_num);
    DPRINTF(Rename, "[tid:%i] [squash sn:%llu] Squashing instructions.\n",
        tid,squash_seq_num);

    // Clear the stall signal if rename was blocked or unblocking before.
    // If it still needs to block, the blocking should happen the next
    // cycle and there should be space to hold everything due to the squash.
    if (renameStatus[tid] == Blocked ||
        renameStatus[tid] == Unblocking) {
        toDecode->renameUnblock[tid] = 1;

        resumeSerialize = false;
        serializeInst[tid] = NULL;
    } else if (renameStatus[tid] == SerializeStall) {
        if (serializeInst[tid]->seqNum <= squash_seq_num) {
            DPRINTF(Rename, "[tid:%i] [squash sn:%llu] "
                "Rename will resume serializing after squash\n",
                tid,squash_seq_num);
            resumeSerialize = true;
            assert(serializeInst[tid]);
        } else {
            resumeSerialize = false;
            toDecode->renameUnblock[tid] = 1;

            serializeInst[tid] = NULL;
        }
    }

    // Set the status to Squashing.
    renameStatus[tid] = Squashing;

    // Squash any instructions from decode.
    for (int i=0; i<fromDecode->size; i++) {
        if (fromDecode->insts[i]->threadNumber == tid &&
            fromDecode->insts[i]->seqNum > squash_seq_num) {
            fromDecode->insts[i]->setSquashed();
            wroteToTimeBuffer = true;
        }

    }

    // Clear the instruction list and skid buffer in case they have any
    // insts in them.
    insts[tid].clear();

    // Clear the skid buffer in case it has any data in it.
    skidBuffer[tid].clear();

    doSquash(squash_seq_num, tid);
}

void
Rename::tick()
{
    wroteToTimeBuffer = false;

    blockThisCycle = false;

    bool status_change = false;

    toIEWIndex = 0;

    sortInsts();

    unsigned store_stall_reasons = 0;

    // Check stall and squash signals.
    for (ThreadID tid : *activeThreads) {
        DPRINTF(Rename, "Processing [tid:%i]\n", tid);

        status_change = checkSignalsAndUpdate(tid) || status_change;

        const unsigned first_output = toIEWIndex;
        unsigned frontend_blocked = 0;
        int sq_headroom = 0;
        if (cpu->frontend) {
            sq_headroom = calcFreeSQEntries(tid);
            if (stalls[tid].iew || renameStatus[tid] == Blocked)
                frontend_blocked |= FrontendCoordinator::RenameSignal;
            if (renameStatus[tid] == Squashing ||
                fromCommit->commitInfo[tid].squash)
                frontend_blocked |= FrontendCoordinator::Recovery;
            if (renameStatus[tid] == SerializeStall)
                frontend_blocked |= FrontendCoordinator::Serialize;
            if (calcFreeROBEntries(tid) <= 0)
                frontend_blocked |= FrontendCoordinator::FullROB;
            if (calcFreeIQEntries(tid) <= 0)
                frontend_blocked |= FrontendCoordinator::FullIQ;
            if (calcFreeLQEntries(tid) <= 0 && calcFreeSQEntries(tid) <= 0)
                frontend_blocked |= FrontendCoordinator::FullLSQ;
            if (renameMap[tid]->minFreeEntries() == 0)
                frontend_blocked |= FrontendCoordinator::FullPRF;
            auto &receiver = toDecode->renameInfo[tid];
            receiver.receiverValid = true;
            receiver.receiverBlocks = frontend_blocked;
            receiver.freeSQEntries = std::max(0, sq_headroom);
            receiver.receiverCapacity = frontend_blocked ? 0 :
                std::min({renameWidth,
                    unsigned(std::max(0, calcFreeROBEntries(tid))),
                    unsigned(std::max(0, calcFreeIQEntries(tid))),
                    renameMap[tid]->minFreeEntries()});
        }
        rename(status_change, tid);
        if (renameStatus[tid] == SerializeStall)
            ++resourceStallStats.SCOREBOARD;
        cpu->recoveryStats.allocatorState(tid,
            renameStatus[tid] == Squashing ||
            fromCommit->commitInfo[tid].squash);
        unsigned issued_slots = 0;
        for (unsigned i = first_output; i < toIEWIndex; ++i)
            issued_slots += toIEW->insts[i]->allocationSlots();
        if (fromCommit->commitInfo[tid].robSquashing)
            stats.recoveryAllocationSlots += issued_slots;
        if (issued_slots < renameWidth && toIEWIndex < MaxWidth &&
            renameStatus[tid] != Squashing &&
            renameStatus[tid] != SerializeStall &&
            !fromCommit->commitInfo[tid].squash)
        {
            const auto reasons = storeStallReasons(tid);
            store_stall_reasons |= reasons;
            if ((reasons & 1) && (blockThisCycle || stalls[tid].iew ||
                                  renameStatus[tid] == Blocked))
                store_stall_reasons |= 8;
        }
        if (cpu->frontend) {
            if (blockThisCycle) frontend_blocked |= FrontendCoordinator::RenameSignal;
            if (renameStatus[tid] == SerializeStall)
                frontend_blocked |= FrontendCoordinator::Serialize;
            unsigned slots = 0;
            for (unsigned i = first_output; i < toIEWIndex; ++i)
                slots += toIEW->insts[i]->allocationSlots();
            cpu->frontend->allocation(toIEWIndex - first_output, slots);
        }
    }

    if (store_stall_reasons & 1) {
        ++resourceStallStats.SB;
        if (store_stall_reasons & 2) ++resourceStallStats.SB_ROB_OVERLAP;
        if (store_stall_reasons & 4) ++resourceStallStats.SB_IQ_OVERLAP;
        if (store_stall_reasons & 8) ++resourceStallStats.SB_FRONTEND_BLOCKED;
    }

    if (numThreads == 1 && !activeThreads->empty()) {
        const bool no_candidate = insts[0].empty() && skidBuffer[0].empty();
        const bool blocked = blockThisCycle || stalls[0].iew ||
                             renameStatus[0] == Blocked ||
                             renameStatus[0] == SerializeStall ||
                             (no_candidate && calcFreeROBEntries(0) <= 0);
        cpu->tmaStats.renameCycle(blocked, renameStatus[0] == Squashing,
                                  no_candidate);
    }

    if (status_change) {
        updateStatus();
    }

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    for (ThreadID tid : *activeThreads) {
        // If we committed this cycle then doneSeqNum will be > 0
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            renameStatus[tid] != Squashing) {

            removeFromHistory(fromCommit->commitInfo[tid].doneSeqNum,
                                  tid);
        }
    }

    // @todo: make into updateProgress function
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        instsInProgress[tid] -= fromIEW->iewInfo[tid].dispatched;
        loadsInProgress[tid] -= fromIEW->iewInfo[tid].dispatchedToLQ;
        storesInProgress[tid] -= fromIEW->iewInfo[tid].dispatchedToSQ;
        assert(loadsInProgress[tid] >= 0);
        assert(storesInProgress[tid] >= 0);
        assert(instsInProgress[tid] >=0);
    }

}

unsigned
Rename::storeStallReasons(ThreadID tid)
{
    // An incomplete or squashed group is not a valid allocation candidate.
    // Prefer the oldest queued group, including while rename is blocked.
    InstQueue *queue = nullptr;
    if (!skidBuffer[tid].empty() && !insts[tid].empty())
        queue = skidBuffer[tid].front()->seqNum < insts[tid].front()->seqNum ?
            &skidBuffer[tid] : &insts[tid];
    else if (!skidBuffer[tid].empty())
        queue = &skidBuffer[tid];
    else if (!insts[tid].empty())
        queue = &insts[tid];
    if (!queue || queue->front()->isSquashed() ||
        commit_ptr->reorderBuffer().hasReservation(queue->front()))
        return 0;

    std::vector<DynInstPtr> batch;
    unsigned stores = 0;
    const auto end = queue->front()->allocationEnd();
    for (const auto &inst : *queue) {
        if (inst->isSquashed()) return 0;
        batch.push_back(inst);
        stores += inst->isStore() || inst->isAtomic();
        if (inst->seqNum == end) break;
    }
    if (batch.empty() || batch.back()->seqNum != end || !stores ||
        batch.size() > MaxWidth - toIEWIndex ||
        calcFreeSQEntries(tid) >= int(stores))
        return 0;
    unsigned reasons = 1;
    if (!commit_ptr->reorderBuffer().canReserve(batch)) reasons |= 2;
    if (!iew_ptr->instQueue.canReserve(batch)) reasons |= 4;
    return reasons;
}

void
Rename::rename(bool &status_change, ThreadID tid)
{
    // If status is Running or idle,
    //     call renameInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from decode
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed

    if (renameStatus[tid] == Blocked) {
        ++stats.status[Blocked];
    } else if (renameStatus[tid] == Squashing) {
        ++stats.status[Squashing];
    } else if (renameStatus[tid] == SerializeStall) {
        ++stats.status[SerializeStall];
        // If we are currently in SerializeStall and resumeSerialize
        // was set, then that means that we are resuming serializing
        // this cycle.  Tell the previous stages to block.
        if (resumeSerialize) {
            resumeSerialize = false;
            block(tid);
            toDecode->renameUnblock[tid] = false;
        }
    } else if (renameStatus[tid] == Unblocking) {
        if (resumeUnblocking) {
            block(tid);
            resumeUnblocking = false;
            toDecode->renameUnblock[tid] = false;
        }
    }

    if (renameStatus[tid] == Running ||
        renameStatus[tid] == Idle) {
        DPRINTF(Rename,
                "[tid:%i] "
                "Not blocked, so attempting to run stage.\n",
                tid);

        renameInsts(tid);
    } else if (renameStatus[tid] == Unblocking) {
        renameInsts(tid);

        if (validInsts()) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        // If we switched over to blocking, then there's a potential for
        // an overall status change.
        status_change = unblock(tid) || status_change || blockThisCycle;
    }
}

void
Rename::renameInsts(ThreadID tid)
{
    // Instructions can be either in the skid buffer or the queue of
    // instructions coming from decode, depending on the status.
    int insts_available = renameStatus[tid] == Unblocking ?
        skidBuffer[tid].size() : insts[tid].size();

    // Check the decode queue to see if instructions are available.
    // If there are no available instructions to rename, then do nothing.
    if (insts_available == 0) {
        DPRINTF(Rename, "[tid:%i] Nothing to do, breaking out early.\n",
                tid);
        // Should I change status to idle?
        ++stats.status[Idle];
        return;
    } else if (renameStatus[tid] == Unblocking) {
        ++stats.status[Unblocking];
    } else if (renameStatus[tid] == Running) {
        ++stats.status[Running];
    }

    InstQueue &insts_to_rename = renameStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    DPRINTF(Rename,
            "[tid:%i] "
            "%i available instructions to send iew.\n",
            tid, insts_available);

    DPRINTF(Rename,
            "[tid:%i] "
            "%i insts pipelining from Rename | "
            "%i insts dispatched to IQ last cycle.\n",
            tid, instsInProgress[tid], fromIEW->iewInfo[tid].dispatched);

    // Handle serializing the next instruction if necessary.
    if (serializeOnNextInst[tid]) {
        if (emptyROB[tid] && instsInProgress[tid] == 0) {
            // ROB already empty; no need to serialize.
            serializeOnNextInst[tid] = false;
        } else if (!insts_to_rename.empty()) {
            insts_to_rename.front()->setSerializeBefore();
        }
    }

    int renamed_insts = 0;
    unsigned renamed_slots = 0;
    unsigned renamed_backend_slots = 0;

    while (insts_available > 0 && toIEWIndex < MaxWidth &&
           (renamed_slots < renameWidth || !insts_to_rename.front()->allocationSlots())) {
        DPRINTF(Rename, "[tid:%i] Sending instructions to IEW.\n", tid);

        assert(!insts_to_rename.empty());

        DynInstPtr inst = insts_to_rename.front();
        StackEngine::Decision stack_decision;
        if (cpu->stackEngine && !inst->isSquashed()) {
            stack_decision = cpu->stackEngine->classify(inst);
            if (stack_decision.sync && !inst->stackSyncSatisfied) {
                inst->stackSyncSatisfied = true;
                insts_to_rename.push_front(
                    cpu->stackEngine->makeSync(inst, stack_decision.reason));
                ++insts_available;
                continue;
            }
            inst->stackTrackedWrite = stack_decision.trackedWrite;
            inst->stackAbsorbed = stack_decision.absorbedUpdate;
            if (inst->stackAbsorbed) {
                inst->stageDemand.robUnits = 0;
                inst->stageDemand.schedulerActions = 0;
                inst->stageDemand.schedulerUnits = 0;
            }
        }
        if (!inst->isSquashed() && isNearControlConstant(inst) &&
            inst->getFault() == NoFault) {
            // PC and CS are actual functional operands, not predicted values.
            // Preserve ordinary execution if any source is still unready.
            auto *isa = inst->tcBase()->getIsaPtr();
            bool ready = true;
            for (unsigned i = 0; i < inst->numSrcRegs(); ++i)
                ready &= scoreboard->getReg(renameMap[tid]->lookup(
                    inst->srcRegIdx(i).flatten(*isa)));
            inst->renameComputed = ready;
            inst->stageDemand.robUnits = ready ? 0 : 1;
            inst->stageDemand.schedulerUnits = ready ? 0 : 1;
            inst->stageDemand.schedulerActions = ready ? 0 : 1;
        }
        if (!inst->isSquashed() &&
            !inst->allocationGroup && inst->staticInst->isFullGprMove() &&
            inst->numSrcRegs() == 3 && inst->numDestRegs() == 1 &&
            inst->getFault() == NoFault) {
            auto *isa = inst->tcBase()->getIsaPtr();
            // Plain MOV retains two invalid old-destination operands for
            // merge/conditional templates. Index 1 is the actual GPR source.
            const auto source_reg = inst->srcRegIdx(1).flatten(*isa);
            const auto dest_reg = inst->destRegIdx(0).flatten(*isa);
            inst->eliminatedMove = source_reg.is(IntRegClass) &&
                dest_reg.is(IntRegClass) && source_reg != dest_reg &&
                !(stack_decision.sourceMask && cpu->stackEngine &&
                  cpu->stackEngine->offset(tid) != 0) &&
                !renameMap[tid]->lookup(source_reg)->isPinned() &&
                !renameMap[tid]->lookup(dest_reg)->isPinned();
        }
        if (!inst->isSquashed() && !inst->allocationGroup &&
            inst->staticInst->isZeroExtendingGprMove() &&
            inst->numSrcRegs() == 3 && inst->numDestRegs() == 1 &&
            inst->getFault() == NoFault) {
            auto *isa = inst->tcBase()->getIsaPtr();
            const auto source_reg = inst->srcRegIdx(1).flatten(*isa);
            const auto dest_reg = inst->destRegIdx(0).flatten(*isa);
            const auto source = renameMap[tid]->lookup(source_reg);
            // A 32-bit MOV must clear the upper half. Keep ordinary PRF
            // allocation/history; an unready producer is projected when its
            // actual value arrives. A self move still executes normally.
            const bool projection = source_reg.is(IntRegClass) &&
                dest_reg.is(IntRegClass) && source_reg != dest_reg &&
                !(stack_decision.sourceMask && cpu->stackEngine &&
                  cpu->stackEngine->offset(tid) != 0) &&
                !source->isPinned() &&
                !renameMap[tid]->lookup(dest_reg)->isPinned();
            inst->renameComputed = projection && scoreboard->getReg(source);
            inst->moveProjection = projection && !inst->renameComputed;
            if (projection) {
                inst->stageDemand.schedulerActions = 0;
                inst->stageDemand.schedulerUnits = 0;
            }
        }
        if (!inst->isSquashed() && !inst->allocationGroup &&
            inst->staticInst->isFullXmmMove() &&
            inst->numSrcRegs() == 2 && inst->numDestRegs() == 2 &&
            inst->getFault() == NoFault) {
            auto *isa = inst->tcBase()->getIsaPtr();
            inst->eliminatedMove = true;
            for (unsigned lane = 0; lane < 2; ++lane) {
                const auto source = inst->srcRegIdx(lane).flatten(*isa);
                const auto dest = inst->destRegIdx(lane).flatten(*isa);
                inst->eliminatedMove &= source.is(FloatRegClass) &&
                    dest.is(FloatRegClass) &&
                    !renameMap[tid]->lookup(source)->isPinned() &&
                    !renameMap[tid]->lookup(dest)->isPinned();
            }
        }
        const unsigned slots = inst->allocationSlots();
        if (slots > renameWidth - renamed_slots) {
            blockThisCycle = true;
            break;
        }
        const unsigned backend_slots = inst->backendSlots();
        std::vector<DynInstPtr> admission_batch;
        auto &rob = commit_ptr->reorderBuffer();
        if (!inst->isSquashed() && !rob.hasReservation(inst)) {
            // Admission is atomic for one allocation unit, but each consumer
            // reserves its own independently defined entries. All functional
            // members must be present before any register mapping changes.
            for (const auto &member : insts_to_rename) {
                if (cpu->stackEngine && cpu->stackEngine->enabled()) {
                    const auto d = cpu->stackEngine->classify(member);
                    member->stackTrackedWrite = d.trackedWrite;
                    member->stackAbsorbed = d.absorbedUpdate;
                    if (member->stackAbsorbed) {
                        member->stageDemand.robUnits = 0;
                        member->stageDemand.schedulerActions = 0;
                        member->stageDemand.schedulerUnits = 0;
                    }
                }
                if ((!cpu->stackEngine || !cpu->stackEngine->enabled()) &&
                    isImplicitPushUpdate(member)) {
                    auto *isa = member->tcBase()->getIsaPtr();
                    const auto rsp = X86ISA::int_reg::Rsp.flatten(*isa);
                    auto source = renameMap[tid]->lookup(rsp);
                    member->renameComputed =
                        !source->isPinned() && scoreboard->getReg(source);
                    member->stageDemand.schedulerActions =
                        member->renameComputed ? 0 : 1;
                    member->stageDemand.schedulerUnits =
                        member->stageDemand.schedulerActions;
                }
                admission_batch.push_back(member);
                if (member->seqNum == inst->allocationEnd())
                    break;
            }
            if (admission_batch.back()->seqNum != inst->allocationEnd() ||
                admission_batch.size() > MaxWidth - toIEWIndex) {
                blockThisCycle = true;
                break;
            }
            if (!rob.canReserve(admission_batch)) {
                blockThisCycle = true;
                incrFullStat(ROB);
                break;
            }
            if (!iew_ptr->instQueue.canReserve(admission_batch)) {
                blockThisCycle = true;
                incrFullStat(IQ);
                break;
            }
            unsigned loads = 0, stores = 0, int_entries = 0, float_entries = 0, wide = 0;
            std::array<unsigned, CCRegClass + 1> fragments{};
            for (const auto &member : admission_batch) {
                loads += member->isLoad();
                stores += member->isStore() || member->isAtomic();
                if (member->eliminatedMove)
                    continue;
                for (unsigned c = 0; c < fragments.size(); ++c)
                    fragments[c] += member->numDestRegs(RegClassType(c));
                if (member->stackTrackedWrite) {
                    auto *isa = member->tcBase()->getIsaPtr();
                    for (unsigned i = 0; i < member->numDestRegs(); ++i)
                        if (member->destRegIdx(i).flatten(*isa) ==
                            X86ISA::int_reg::Rsp.flatten(*isa))
                            --fragments[IntRegClass];
                }
                int_entries += member->hardwareDestEntries(IntRegClass);
                const auto fp = member->hardwareDestEntries(FloatRegClass);
                float_entries += fp;
                if (fp) wide += member->stageDemand.floatWideEntries;
            }
            if (calcFreeLQEntries(tid) < int(loads) ||
                calcFreeSQEntries(tid) < int(stores)) {
                blockThisCycle = true;
                incrFullStat(calcFreeLQEntries(tid) < int(loads) ? LQ : SQ);
                break;
            }
            bool registers_available =
                int_entries <= freeList->numFreeIntEntries() &&
                float_entries <= freeList->numFreeFloatEntries() &&
                wide <= freeList->numFreeFloatWideEntries();
            if (int_entries > freeList->numFreeIntEntries())
                ++stats.fullRegistersByClass[IntRegClass];
            if (float_entries > freeList->numFreeFloatEntries() ||
                wide > freeList->numFreeFloatWideEntries())
                ++stats.fullRegistersByClass[FloatRegClass];
            for (unsigned c = 0; c < fragments.size(); ++c) {
                if (fragments[c] > renameMap[tid]->numFreeEntries(RegClassType(c))) {
                    registers_available = false;
                    ++stats.fullRegistersByClass[c];
                }
            }
            if (!registers_available) {
                blockThisCycle = true;
                ++stats.fullRegistersEvents;
                break;
            }
        }

        insts_to_rename.pop_front();

        if (renameStatus[tid] == Unblocking) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Removing [sn:%llu] PC:%s from rename skidBuffer\n",
                    tid, inst->seqNum, inst->pcState());
        }

        if (inst->isSquashed()) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "instruction %i with PC %s is squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;

            // Decrement how many instructions are available.
            --insts_available;

            continue;
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Processing instruction [sn:%llu] with PC %s.\n",
                tid, inst->seqNum, inst->pcState());

        // Check here to make sure there are enough destination registers
        // to rename to.  Otherwise block.
        if (!renameMap[tid]->canRename(inst)) {
            DPRINTF(Rename,
                    "Blocking due to "
                    " lack of free physical registers to rename to.\n");
            blockThisCycle = true;
            insts_to_rename.push_front(inst);
            ++stats.fullRegistersEvents;
            for (int i = IntRegClass; i <= CCRegClass; ++i) {
                const auto type = static_cast<RegClassType>(i);
                if (!renameMap[tid]->canRename(inst, type))
                    ++stats.fullRegistersByClass[i];
            }

            break;
        }

        // If we detect a WaW conflict for MiscRegs, we tag
        // the instruction as SerializeBefore to avoid the conflict
        if (!inst->isSerializeAfter() && inst->numDestRegs(MiscRegClass)) {
            handleMiscRegWaW(inst, tid);
        }

        // Handle serializeAfter/serializeBefore instructions.
        // serializeAfter marks the next instruction as serializeBefore.
        // serializeBefore makes the instruction wait in rename until the ROB
        // is empty.

        // In this model, IPR accesses are serialize before
        // instructions, and store conditionals are serialize after
        // instructions.  This is mainly due to lack of support for
        // out-of-order operations of either of those classes of
        // instructions.
        if (inst->isSerializeBefore() && !inst->isSerializeHandled()) {
            DPRINTF(Rename, "Serialize before instruction encountered.\n");

            if (!inst->isTempSerializeBefore()) {
                stats.serializing++;
                inst->setSerializeHandled();
            } else {
                stats.tempSerializing++;
            }

            // Change status over to SerializeStall so that other stages know
            // what this is blocked on.
            renameStatus[tid] = SerializeStall;

            serializeInst[tid] = inst;

            blockThisCycle = true;

            break;
        } else if ((inst->isStoreConditional() || inst->isSerializeAfter()) &&
                   !inst->isSerializeHandled()) {
            DPRINTF(Rename, "Serialize after instruction encountered.\n");

            stats.serializing++;

            inst->setSerializeHandled();

            serializeAfter(insts_to_rename, tid);
        }

        if (cpu->coreFunctionalObservation && !inst->coreFunctionalFacts) {
            inst->coreFunctionalFacts = std::make_unique<CoreFunctionalSnapshot>(
                inst->numSrcRegs(), inst->numDestRegs());
        }
        // Nothing above this point has acquired downstream capacity. Once
        // admitted, these reservations survive pipeline delay and cannot be
        // stolen by a younger allocation or a memory replay.
        for (const auto &member : admission_batch) {
            rob.reserve(member);
            iew_ptr->instQueue.reserve(member);
        }

        renameSrcRegs(inst, inst->threadNumber);

        renameDestRegs(inst, inst->threadNumber);
        if (inst->coreFunctionalFacts)
            inst->coreFunctionalFacts->completeNormalization();
        if (inst->moveProjection)
            ++cpu->moveEliminationStats.accepted;
        if (inst->renameComputed) {
            const bool move = inst->staticInst->isZeroExtendingGprMove();
            const bool control_constant = isNearControlConstant(inst);
            // The source was ready before the allocation was reserved.
            // Execute the actual zero-extension/update into the new PRF.
            if (control_constant) {
                for (unsigned i = 0; i < inst->numSrcRegs(); ++i)
                    panic_if(!scoreboard->getReg(inst->renamedSrcIdx(i)),
                        "Control constant source became unready after admission");
            } else {
                panic_if(!scoreboard->getReg(inst->renamedSrcIdx(move ? 1 : 0)),
                         "Rename-computed source became unready after admission");
            }
            inst->execute();
            panic_if(inst->getFault() != NoFault,
                     "Rename-computed operation faulted");
            if (cpu->resultForwarding)
                cpu->resultForwarding->track(inst, false);
            for (unsigned i = 0; i < inst->numDestRegs(); ++i)
                scoreboard->setReg(inst->renamedDestIdx(i));
            inst->setExecuted();
            inst->setResultsWoken();
            if (move)
                ++cpu->moveEliminationStats.accepted;
            else if (!control_constant)
                ++stats.pushUpdatesComputed;
        } else if ((!cpu->stackEngine || !cpu->stackEngine->enabled()) &&
                   isImplicitPushUpdate(inst)) {
            ++stats.pushUpdatesDeferred;
        }
        if (cpu->stackEngine) {
            inst->stackSourceMask = stack_decision.sourceMask;
            inst->stackSourceDelta = cpu->stackEngine->enabled() ?
                cpu->stackEngine->offset(tid) : 0;
            if (stack_decision.sourceMask || stack_decision.trackedWrite ||
                stack_decision.fallback || inst->stackSync) {
                DPRINTF(StackEngine,
                    "[tid:%u] [sn:%llu] PC:%s op=%s base-delta=%lld "
                    "source-delta=%lld source-mask=%#llx tracked=%u absorbed=%u step=%lld fallback=%u\n",
                    tid, inst->seqNum, inst->pcState(), inst->staticInst->getName(),
                    cpu->stackEngine->enabled() ? cpu->stackEngine->offset(tid) : 0,
                    inst->stackSourceDelta,
                    stack_decision.sourceMask, stack_decision.trackedWrite,
                    stack_decision.absorbedUpdate, stack_decision.increment,
                    stack_decision.fallback);
            }
            if (cpu->stackEngine->enabled() &&
                (inst->stackSync || stack_decision.trackedWrite)) {
                const int64_t after = inst->stackSync ? 0 :
                    cpu->stackEngine->offset(tid) + stack_decision.increment;
                cpu->stackEngine->apply(tid, inst->seqNum, after);
                inst->stackStateChanged = true;
                inst->stackDeltaAfter = after;
                if (stack_decision.trackedWrite) cpu->stackEngine->noteTracked();
            }
            if (cpu->stackEngine->enabled() && stack_decision.fallback)
                cpu->stackEngine->noteFallback();
            if (cpu->stackEngine->enabled() && inst->isControl())
                cpu->stackEngine->checkpoint(tid, inst->seqNum);
        }
        if (inst->eliminatedMove) {
            inst->setExecuted();
            inst->setResultsWoken();
            ++cpu->moveEliminationStats.accepted;
        }
        if (inst->pendingFrontendAcceptance) {
            cpu->frontend->accepted(inst->seqNum);
            inst->pendingFrontendAcceptance = false;
        }
        if (inst->bypassesIQ()) {
            if (inst->staticInst->isZeroIdiom() && inst->getFault() == NoFault) {
                cpu->zeroIdiom->admit(inst);
                if (cpu->resultForwarding)
                    cpu->resultForwarding->track(inst, false);
                for (RegIndex i = 0; i < inst->numDestRegs(); ++i)
                    scoreboard->setReg(inst->renamedDestIdx(i));
            }
        }

        if (inst->isAtomic() || inst->isStore()) {
            storesInProgress[tid]++;
        } else if (inst->isLoad()) {
            loadsInProgress[tid]++;
        }

        ++renamed_insts;
        renamed_slots += slots;
        renamed_backend_slots += backend_slots;
        cpu->recordAllocatedUops(slots, inst);
        cpu->tmaStats.allocated(inst);
        // Notify potential listeners that source and destination registers for
        // this instruction have been renamed.
        ppRename->notify(inst);

        inst->renameEndTick = curTick() - inst->fetchTick;

        // Put instruction in rename queue.
        toIEW->insts[toIEWIndex] = inst;
        ++(toIEW->size);

        // Increment which instruction we're on.
        ++toIEWIndex;

        // Decrement how many instructions are available.
        --insts_available;
    }

    instsInProgress[tid] += renamed_backend_slots;
    stats.renamedInsts += renamed_insts;

    // If we wrote to the time buffer, record this.
    if (toIEWIndex) {
        wroteToTimeBuffer = true;
    }

    // Check if there's any instructions left that haven't yet been renamed.
    // If so then block.
    if (insts_available) {
        blockThisCycle = true;
    }

    if (blockThisCycle) {
        block(tid);
        toDecode->renameUnblock[tid] = false;
    }
}

void
Rename::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop_front();

        assert(tid == inst->threadNumber);

        DPRINTF(Rename, "[tid:%i] Inserting [sn:%llu] PC: %s into Rename "
                "skidBuffer\n", tid, inst->seqNum, inst->pcState());

        ++stats.skidInsts;

        skidBuffer[tid].push_back(inst);
    }

    if (skidBuffer[tid].size() > skidBufferMax) {
        InstQueue::iterator it;
        warn("Skidbuffer contents:\n");
        for (it = skidBuffer[tid].begin(); it != skidBuffer[tid].end(); it++) {
            warn("[tid:%i] %s [sn:%llu].\n", tid,
                    (*it)->staticInst->disassemble(
                        inst->pcState().instAddr()),
                    (*it)->seqNum);
        }
        panic("Skidbuffer Exceeded Max Size");
    }
}

void
Rename::sortInsts()
{
    int insts_from_decode = fromDecode->size;
    for (int i = 0; i < insts_from_decode; ++i) {
        const DynInstPtr &inst = fromDecode->insts[i];
        insts[inst->threadNumber].push_back(inst);
        inst->renameTick = curTick() - inst->fetchTick;
    }
}

bool
Rename::skidsEmpty()
{
    for (ThreadID tid : *activeThreads) {
        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

void
Rename::updateStatus()
{
    bool any_unblocking = false;

    for (ThreadID tid : *activeThreads) {
        if (renameStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // Rename will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::RenameIdx);
        }
    } else {
        // If it's not unblocking, then rename will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::RenameIdx);
        }
    }
}

bool
Rename::block(ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Blocking.\n", tid);

    // Add the current inputs onto the skid buffer, so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    // Only signal backwards to block if the previous stages do not think
    // rename is already blocked.
    if (renameStatus[tid] != Blocked) {
        // If resumeUnblocking is set, we unblocked during the squash,
        // but now we're have unblocking status. We need to tell earlier
        // stages to block.
        if (resumeUnblocking || renameStatus[tid] != Unblocking) {
            toDecode->renameBlock[tid] = true;
            toDecode->renameUnblock[tid] = false;
            wroteToTimeBuffer = true;
        }

        // Rename can not go from SerializeStall to Blocked, otherwise
        // it would not know to complete the serialize stall.
        if (renameStatus[tid] != SerializeStall) {
            // Set status to Blocked.
            renameStatus[tid] = Blocked;
            return true;
        }
    }

    return false;
}

bool
Rename::unblock(ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Trying to unblock.\n", tid);

    // Rename is done unblocking if the skid buffer is empty.
    if (skidBuffer[tid].empty() && renameStatus[tid] != SerializeStall) {

        DPRINTF(Rename, "[tid:%i] Done unblocking.\n", tid);

        toDecode->renameUnblock[tid] = true;
        wroteToTimeBuffer = true;

        renameStatus[tid] = Running;
        return true;
    }

    return false;
}

void
Rename::doSquash(const InstSeqNum &squashed_seq_num, ThreadID tid)
{
    auto hb_it = historyBuffer[tid].begin();

    // After a syscall squashes everything, the history buffer may be empty
    // but the ROB may still be squashing instructions.
    // Go through the most recent instructions, undoing the mappings
    // they did and freeing up the registers.
    while (!historyBuffer[tid].empty() &&
           hb_it->instSeqNum > squashed_seq_num) {
        assert(hb_it != historyBuffer[tid].end());
        if (cpu->zeroIdiom) cpu->zeroIdiom->cancel(hb_it->instSeqNum);

        DPRINTF(Rename, "[tid:%i] Removing history entry with sequence "
                "number %i (archReg: %d, newPhysReg: %d, prevPhysReg: %d).\n",
                tid, hb_it->instSeqNum, hb_it->archReg.index(),
                hb_it->newPhysReg->index(), hb_it->prevPhysReg->index());

        // Undo the rename mapping only if it was really a change.
        // Special regs that are not really renamed (like misc regs
        // and the zero reg) can be recognized because the new mapping
        // is the same as the old one.  While it would be merely a
        // waste of time to update the rename table, we definitely
        // don't want to put these on the free list.
        if (hb_it->newPhysReg != hb_it->prevPhysReg) {
            // Tell the rename map to set the architected register to the
            // previous physical register that it was renamed to.
            renameMap[tid]->setEntry(hb_it->archReg, hb_it->prevPhysReg);

            // The phys regs can still be owned by squashing but
            // executing instructions in IEW at this moment. To avoid
            // ownership hazard in SMT CPU, we delay the freelist update
            // until they are indeed squashed in the commit stage.
            freeingInProgress[tid].push_back(hb_it->newPhysReg);
        }

        // mark the speculative register as ready in the scoreboard as
        // the producing instruction is being squashed.
        // This is not necessary for renameable register as the physical
        // register is unreachable until it gets recycled as a new destination
        // register (it will be marked as busy anyway next time it gets used).
        // This is instead required for fixed mapping registers which could
        // be rereferenced as a source registers after the squash
        if (!hb_it->alias) scoreboard->setReg(hb_it->newPhysReg);

        // Notify potential listeners that the register mapping needs to be
        // removed because the instruction it was mapped to got squashed. Note
        // that this is done before hb_it is incremented.
        ppSquashInRename->notify(std::make_pair(hb_it->instSeqNum,
                                                hb_it->newPhysReg));

        historyBuffer[tid].erase(hb_it++);

        ++stats.undoneMaps;
    }
}

void
Rename::removeFromHistory(InstSeqNum inst_seq_num, ThreadID tid)
{
    if (cpu->zeroIdiom) cpu->zeroIdiom->retire(tid, inst_seq_num);
    DPRINTF(Rename, "[tid:%i] Removing a committed instruction from the "
            "history buffer %u (size=%i), until [sn:%llu].\n",
            tid, tid, historyBuffer[tid].size(), inst_seq_num);

    if (historyBuffer[tid].empty()) {
        DPRINTF(Rename, "[tid:%i] History buffer is empty.\n", tid);
        return;
    }
    auto hb_it = std::prev(historyBuffer[tid].end());
    if (hb_it->instSeqNum > inst_seq_num) {
        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Old sequence number encountered. "
                "Ensure that a syscall happened recently.\n",
                tid,inst_seq_num);
        return;
    }

    // Commit all the renames up until (and including) the committed sequence
    // number. Some or even all of the committed instructions may not have
    // rename histories if they did not have destination registers that were
    // renamed.
    while (!historyBuffer[tid].empty() &&
           hb_it != historyBuffer[tid].end() &&
           hb_it->instSeqNum <= inst_seq_num) {

        DPRINTF(Rename, "[tid:%i] Freeing up older rename of reg %i (%s), "
                "[sn:%llu].\n",
                tid, hb_it->prevPhysReg->index(),
                hb_it->prevPhysReg->className(),
                hb_it->instSeqNum);

        // Don't free special phys regs like misc and zero regs, which
        // can be recognized because the new mapping is the same as
        // the old one.
        if (hb_it->newPhysReg != hb_it->prevPhysReg &&
            freeList->addReg(hb_it->prevPhysReg)) {
            if (hb_it->prevPhysReg->classValue() == FloatRegClass)
                ++stats.fpReturned;
            if (hb_it->prevPhysReg->classValue() == IntRegClass)
                ++stats.intReturned;
        }


        ++stats.committedMaps;

        const auto previous = hb_it == historyBuffer[tid].begin() ?
            historyBuffer[tid].end() : std::prev(hb_it);
        historyBuffer[tid].erase(hb_it);
        hb_it = previous;
    }
}

void
Rename::renameSrcRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_src_regs = inst->numSrcRegs();
    auto *isa = tc->getIsaPtr();

    // Get the architectual register numbers from the source and
    // operands, and redirect them to the right physical register.
    for (int src_idx = 0; src_idx < num_src_regs; src_idx++) {
        const RegId& src_reg = inst->srcRegIdx(src_idx);
        const RegId flat_reg = src_reg.flatten(*isa);
        if (inst->coreFunctionalFacts)
            inst->coreFunctionalFacts->observeSource(src_idx, src_reg, flat_reg);
        PhysRegIdPtr renamed_reg;

        renamed_reg = map->lookup(flat_reg);
        switch (flat_reg.classValue()) {
          case InvalidRegClass:
            break;
          case IntRegClass:
            stats.intLookups++;
            break;
          case FloatRegClass:
            stats.fpLookups++;
            break;
          case VecRegClass:
          case VecElemClass:
            stats.vecLookups++;
            break;
          case VecPredRegClass:
            stats.vecPredLookups++;
            break;
          case MatRegClass:
            stats.matLookups++;
            break;
          case CCRegClass:
          case MiscRegClass:
            break;

          default:
            panic("Invalid register class: %d.", flat_reg.classValue());
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Looking up %s arch reg %i, got phys reg %i (%s)\n",
                tid, flat_reg.className(),
                src_reg.index(), renamed_reg->index(),
                renamed_reg->className());

        inst->renameSrcReg(src_idx, renamed_reg);

        // See if the register is ready or not.
        if (scoreboard->getReg(renamed_reg)) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is ready.\n",
                    tid, renamed_reg->index(), renamed_reg->flatIndex(),
                    renamed_reg->className());

            inst->markSrcRegReady(src_idx);
        } else {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is not ready.\n",
                    tid, renamed_reg->index(), renamed_reg->flatIndex(),
                    renamed_reg->className());
        }

        ++stats.lookups;
    }
}

void
Rename::renameDestRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_dest_regs = inst->numDestRegs();
    auto *isa = tc->getIsaPtr();

    // Rename the destination registers.
    for (int dest_idx = 0; dest_idx < num_dest_regs; dest_idx++) {
        const RegId& dest_reg = inst->destRegIdx(dest_idx);
        UnifiedRenameMap::RenameInfo rename_result;

        RegId flat_dest_regid = dest_reg.flatten(*isa);
        flat_dest_regid.setNumPinnedWrites(dest_reg.getNumPinnedWrites());
        if (inst->coreFunctionalFacts)
            inst->coreFunctionalFacts->observeDestination(
                dest_idx, dest_reg, flat_dest_regid);

        if (inst->stackTrackedWrite && flat_dest_regid ==
            X86ISA::int_reg::Rsp.flatten(*isa)) {
            const RegId invalid;
            auto *unused = map->lookup(invalid);
            inst->flattenedDestIdx(dest_idx, invalid);
            inst->renameDestReg(dest_idx, unused, unused);
            continue;
        }

        if (inst->eliminatedMove) {
            auto source = inst->renamedSrcIdx(
                inst->staticInst->isFullXmmMove() ? dest_idx : 1);
            auto previous = map->lookup(flat_dest_regid);
            if (source != previous) source->retainRenameReference();
            map->setEntry(flat_dest_regid, source);
            rename_result = {source, previous};
        } else {
            const bool entry = (flat_dest_regid.is(IntRegClass) ||
                                flat_dest_regid.is(FloatRegClass)) &&
                !flat_dest_regid.regClass().isFunctional(flat_dest_regid);
            const auto group = flat_dest_regid.is(FloatRegClass) &&
                inst->stageDemand.floatWideEntries == 1 ? 0 :
                flat_dest_regid.regClass().regAllocationGroup(flat_dest_regid);
            PhysRegIdPtr peer = nullptr;
            if (entry) {
                // Prior destinations already retain the latest successful
                // rename for each allocation group; no transient map is needed.
                for (int prior_idx = dest_idx - 1; prior_idx >= 0; --prior_idx) {
                    const auto &prior = inst->flattenedDestIdx(prior_idx);
                    if (prior.classValue() != flat_dest_regid.classValue() ||
                        prior.regClass().isFunctional(prior) ||
                        inst->renamedDestIdx(prior_idx) ==
                            inst->prevDestIdx(prior_idx))
                        continue;
                    const auto prior_group = prior.is(FloatRegClass) &&
                        inst->stageDemand.floatWideEntries == 1 ? 0 :
                        prior.regClass().regAllocationGroup(prior);
                    if (prior_group == group) {
                        peer = inst->renamedDestIdx(prior_idx);
                        break;
                    }
                }
            }
            rename_result = map->rename(flat_dest_regid, peer);
            if (entry && inst->stageDemand.floatWideEntries &&
                flat_dest_regid.is(FloatRegClass) &&
                rename_result.first != rename_result.second)
                freeList->markFloatWide(rename_result.first);
        }

        inst->flattenedDestIdx(dest_idx, flat_dest_regid);

        if (!inst->eliminatedMove) scoreboard->unsetReg(rename_result.first);

        DPRINTF(Rename,
                "[tid:%i] "
                "Renaming arch reg %i (%s) to physical reg %i (%i).\n",
                tid, dest_reg.index(), dest_reg.className(),
                rename_result.first->index(),
                rename_result.first->flatIndex());

        // Record the rename information so that a history can be kept.
        RenameHistory hb_entry(inst->seqNum, flat_dest_regid,
                               rename_result.first,
                               rename_result.second, inst->eliminatedMove);

        historyBuffer[tid].push_front(hb_entry);

        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Adding instruction to history buffer (size=%i).\n",
                tid,(*historyBuffer[tid].begin()).instSeqNum,
                historyBuffer[tid].size());

        // Tell the instruction to rename the appropriate destination
        // register (dest_idx) to the new physical register
        // (rename_result.first), and record the previous physical
        // register that the same logical register was renamed to
        // (rename_result.second).
        inst->renameDestReg(dest_idx,
                            rename_result.first,
                            rename_result.second);

        ++stats.renamedOperands;
    }
}

void
Rename::handleMiscRegWaW(DynInstPtr &inst, ThreadID tid)
{
    // If the destination register is already being produced, we mark the
    // instruction as serialize before, stalling it at rename until
    // the ROB is empty. This is an expensive way to deal with a WAW
    // conflict. Even if the instruction is marked as non speculative
    // and will only execute at commit time, the dependency logic
    // (called at dispatch time)
    // is not capable of handling two producers of the same register
    // at the same time.
    unsigned num_dest_regs = inst->numDestRegs();
    for (int dest_idx = 0; dest_idx < num_dest_regs; dest_idx++) {

        gem5::ThreadContext *tc = inst->tcBase();
        UnifiedRenameMap *map = renameMap[tid];
        auto *isa = tc->getIsaPtr();

        const RegId &dest_reg = inst->destRegIdx(dest_idx);
        const RegId flat_reg = dest_reg.flatten(*isa);
        PhysRegIdPtr phys_reg = map->lookup(flat_reg);

        if (phys_reg->classValue() == MiscRegClass &&
            !phys_reg->isAlwaysReady() &&
            !scoreboard->getReg(map->lookup(flat_reg))) {

            inst->setSerializeBefore();
            return;
        }
    }
}

int
Rename::calcFreeROBEntries(ThreadID tid)
{
    return commit_ptr->reorderBuffer().numFreeEntries(tid);
}

int
Rename::calcFreeIQEntries(ThreadID tid)
{
    return iew_ptr->instQueue.numFreeEntries(tid);
}

int
Rename::calcFreeLQEntries(ThreadID tid)
{
        int num_free = freeEntries[tid].lqEntries -
            (loadsInProgress[tid] - fromIEW->iewInfo[tid].dispatchedToLQ);
        DPRINTF(Rename,
                "calcFreeLQEntries: free lqEntries: %d, loadsInProgress: %d, "
                "loads dispatchedToLQ: %d\n",
                freeEntries[tid].lqEntries, loadsInProgress[tid],
                fromIEW->iewInfo[tid].dispatchedToLQ);
        return num_free;
}

int
Rename::calcFreeSQEntries(ThreadID tid)
{
        int num_free = freeEntries[tid].sqEntries -
            (storesInProgress[tid] - fromIEW->iewInfo[tid].dispatchedToSQ);
        DPRINTF(Rename, "calcFreeSQEntries: free sqEntries: %d, "
                "storesInProgress: %d, stores dispatchedToSQ: %d\n",
                freeEntries[tid].sqEntries, storesInProgress[tid],
                fromIEW->iewInfo[tid].dispatchedToSQ);
        return num_free;
}

unsigned
Rename::validInsts()
{
    unsigned inst_count = 0;

    for (int i=0; i<fromDecode->size; i++) {
        if (!fromDecode->insts[i]->isSquashed())
            inst_count++;
    }

    return inst_count;
}

void
Rename::readStallSignals(ThreadID tid)
{
    if (fromIEW->iewBlock[tid]) {
        stalls[tid].iew = true;
    }

    if (fromIEW->iewUnblock[tid]) {
        assert(stalls[tid].iew);
        stalls[tid].iew = false;
    }
}

bool
Rename::checkStall(ThreadID tid)
{
    bool ret_val = false;

    if (stalls[tid].iew) {
        DPRINTF(Rename,"[tid:%i] Stall from IEW stage detected.\n", tid);
        ret_val = true;
    } else if (renameStatus[tid] == SerializeStall &&
               (!emptyROB[tid] || instsInProgress[tid])) {
        DPRINTF(Rename,"[tid:%i] Stall: Serialize stall and ROB is not "
                "empty.\n",
                tid);
        ret_val = true;
    }

    return ret_val;
}

void
Rename::readFreeEntries(ThreadID tid)
{
    if (fromIEW->iewInfo[tid].usedIQ)
        freeEntries[tid].iqEntries = fromIEW->iewInfo[tid].freeIQEntries;

    if (fromIEW->iewInfo[tid].usedLSQ) {
        freeEntries[tid].lqEntries = fromIEW->iewInfo[tid].freeLQEntries;
        freeEntries[tid].sqEntries = fromIEW->iewInfo[tid].freeSQEntries;
    }

    if (fromCommit->commitInfo[tid].usedROB) {
        freeEntries[tid].robEntries =
            fromCommit->commitInfo[tid].freeROBEntries;
        emptyROB[tid] = fromCommit->commitInfo[tid].emptyROB;
    }

    DPRINTF(Rename, "[tid:%i] Free IQ: %i, Free ROB: %i, "
                    "Free LQ: %i, Free SQ: %i, FreeRM %i(%i %i %i %i %i %i %i)\n",
            tid,
            freeEntries[tid].iqEntries,
            freeEntries[tid].robEntries,
            freeEntries[tid].lqEntries,
            freeEntries[tid].sqEntries,
            renameMap[tid]->minFreeEntries(),
            renameMap[tid]->numFreeEntries(IntRegClass),
            renameMap[tid]->numFreeEntries(FloatRegClass),
            renameMap[tid]->numFreeEntries(VecRegClass),
            renameMap[tid]->numFreeEntries(VecElemClass),
            renameMap[tid]->numFreeEntries(VecPredRegClass),
            renameMap[tid]->numFreeEntries(MatRegClass),
            renameMap[tid]->numFreeEntries(CCRegClass));

    DPRINTF(Rename, "[tid:%i] Free FP allocation entries: %u\n",
            tid, freeList->numFreeFloatEntries());

    DPRINTF(Rename, "[tid:%i] %i instructions not yet in ROB\n",
            tid, instsInProgress[tid]);
}

bool
Rename::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if necessary.
    // If status was blocked
    //     check if stall conditions have passed
    //         if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.
    // If status was serialize stall
    //     check if ROB is empty and no insts are in flight to the ROB

    readFreeEntries(tid);
    readStallSignals(tid);

    if (fromCommit->commitInfo[tid].squash) {
        DPRINTF(Rename, "[tid:%i] Squashing instructions due to squash from "
                "commit.\n", tid);

        squash(fromCommit->commitInfo[tid].doneSeqNum, tid);

        return true;
    } else if (!freeingInProgress[tid].empty()) {
        DPRINTF(Rename, "[tid:%i] Freeing phys regs of misspeculated "
                "instructions.\n", tid);

        auto reg_it = freeingInProgress[tid].cbegin();
        while ( reg_it != freeingInProgress[tid].cend()){
            // Put the renamed physical register back on the free list.
            freeList->addReg(*reg_it);
            ++reg_it;
        }
        freeingInProgress[tid].clear();
    }

    if (checkStall(tid)) {
        return block(tid);
    }

    if (renameStatus[tid] == Blocked) {
        DPRINTF(Rename, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        renameStatus[tid] = Unblocking;

        unblock(tid);

        return true;
    }

    if (renameStatus[tid] == Squashing) {
        // Switch status to running if rename isn't being told to block or
        // squash this cycle.
        if (resumeSerialize) {
            DPRINTF(Rename,
                    "[tid:%i] Done squashing, switching to serialize.\n", tid);

            renameStatus[tid] = SerializeStall;
            return true;
        } else if (resumeUnblocking) {
            DPRINTF(Rename,
                    "[tid:%i] Done squashing, switching to unblocking.\n",
                    tid);
            renameStatus[tid] = Unblocking;
            return true;
        } else {
            DPRINTF(Rename, "[tid:%i] Done squashing, switching to running.\n",
                    tid);
            renameStatus[tid] = Running;
            return false;
        }
    }

    if (renameStatus[tid] == SerializeStall) {
        // Stall ends once the ROB is free.
        DPRINTF(Rename, "[tid:%i] Done with serialize stall, switching to "
                "unblocking.\n", tid);

        DynInstPtr serial_inst = serializeInst[tid];

        renameStatus[tid] = Unblocking;

        unblock(tid);

        DPRINTF(Rename, "[tid:%i] Processing instruction [%lli] with "
                "PC %s.\n", tid, serial_inst->seqNum, serial_inst->pcState());

        // Put instruction into queue here.
        serial_inst->clearSerializeBefore();

        if (!skidBuffer[tid].empty()) {
            skidBuffer[tid].push_front(serial_inst);
        } else {
            insts[tid].push_front(serial_inst);
        }

        DPRINTF(Rename, "[tid:%i] Instruction must be processed by rename."
                " Adding to front of list.\n", tid);

        serializeInst[tid] = NULL;

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause rename to change its status.  Rename remains the same as before.
    return false;
}

void
Rename::serializeAfter(InstQueue &inst_list, ThreadID tid)
{
    if (inst_list.empty()) {
        // Mark a bit to say that I must serialize on the next instruction.
        serializeOnNextInst[tid] = true;
        return;
    }

    // Set the next instruction as serializing.
    inst_list.front()->setSerializeBefore();
}

void
Rename::incrFullStat(const FullSource &source)
{
    switch (source) {
      case ROB:
        ++stats.ROBFullEvents;
        break;
      case IQ:
        ++stats.IQFullEvents;
        break;
      case LQ:
        ++stats.LQFullEvents;
        break;
      case SQ:
        ++stats.SQFullEvents;
        break;
      default:
        panic("Rename full stall stat should be incremented for a reason!");
        break;
    }
}

void
Rename::dumpHistory()
{
    std::list<RenameHistory>::iterator buf_it;

    for (ThreadID tid = 0; tid < numThreads; tid++) {

        buf_it = historyBuffer[tid].begin();

        while (buf_it != historyBuffer[tid].end()) {
            cprintf("Seq num: %i\nArch reg[%s]: %i New phys reg:"
                    " %i[%s] Old phys reg: %i[%s]\n",
                    (*buf_it).instSeqNum,
                    (*buf_it).archReg.className(),
                    (*buf_it).archReg.index(),
                    (*buf_it).newPhysReg->index(),
                    (*buf_it).newPhysReg->className(),
                    (*buf_it).prevPhysReg->index(),
                    (*buf_it).prevPhysReg->className());

            buf_it++;
        }
    }
}

} // namespace o3
} // namespace gem5
