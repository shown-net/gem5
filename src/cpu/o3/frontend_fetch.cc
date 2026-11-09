// SPDX-License-Identifier: BSD-3-Clause
// Native request and x86 functional-decode adapter for the component frontend.
#include "cpu/o3/fetch.hh"
#include <algorithm>
#include <cstring>
#include "arch/x86/decoder.hh"
#include "arch/x86/pcstate.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/Fetch.hh"
#include "mem/inst_code.hh"
#include "params/BaseO3CPU.hh"

namespace gem5::o3
{
void
Fetch::accountIcacheStalls()
{
    const uint64_t now = uint64_t(cpu->curCycle());
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!cpu->unhaltedClockStats.running[tid] || stalls[tid].decode)
            continue;
        RequestPtr waiting = fetchStatus[tid] == IcacheWaitResponse ?
                             memReq[tid] : nullptr;
        if (!waiting && frontend) {
            const Addr address = pc[tid]->instAddr() + fetchOffset[tid];
            if (!frontend->instructionBytes->contains(address)) {
                for (const auto &[request, state] : byteRequests)
                    if (state.response &&
                        cpu->contextToThread(request->contextId()) == tid &&
                        request->getVaddr() == fetchBufferAlignPC(address)) {
                        waiting = request;
                        break;
                    }
            }
        }
        if (!waiting) continue;
        const auto facts = waiting->getExtension<TaoTraceRubyResponseExtension>();
        if (!facts || !facts->hasL1MissTick) continue;
        if (frontend && frontend->readySupply(lastIcacheAccounting)) continue;
        const uint64_t began = uint64_t(cpu->ticksToCycles(facts->firstL1MissTick));
        const uint64_t start = std::max(lastIcacheAccounting, began);
        if (now > start) {
            cpu->icacheDataStats.STALLS += now - start;
            cpu->icacheTagStats.STALLS += now - start;
        }
    }
    lastIcacheAccounting = now;
}

bool
Fetch::emitFrontendRecord(FrontendRecordPtr record)
{
    auto inst = record->materialize(cpu);
    fatal_if(toRename->size >= MaxWidth, "IDQ output exceeds compiled width");
    toRename->insts[toRename->size++] = inst;
    ppFetch->notify(inst);
    inst->decodeTick = curTick() - inst->fetchTick;
    wroteToTimeBuffer = true;

    if (inst->isDirectCtrl() && (inst->isUncondCtrl() || inst->readPredTaken())) {
        auto target = inst->branchTarget();
        if (*target != inst->readPredTarg()) {
            inst->setBranchPredMispredicted();
            const auto cause = inst->untrackedFrontendBranch ?
                RecoveryCause::UnknownBranch : RecoveryCause::FrontendBranch;
            cpu->recoveryStats.accept(inst->threadNumber, inst->seqNum, cause);
            auto &info = toBAC->decodeInfo[inst->threadNumber];
            info.branchMispredict = true;
            info.controlMispredict = false;
            info.mispredictInst = inst;
            info.squash = true;
            info.doneSeqNum = inst->seqNum;
            set(info.nextPC, *target);
            info.branchTaken = inst->readPredTaken() || inst->isUncondCtrl();
            info.squashInst = inst;
            inst->setPredTarg(*target);
            // The native backward signal redirects the path next cycle.
            return false;
        }
    }
    return true;
}

void
Fetch::fetchFrontend(bool &status_change)
{
    if (frontend->instructionBytes->lookaheadEnabled &&
        frontend->instructionBytes->requestDepth > 1 &&
        !activeThreads->empty()) {
        const ThreadID waiting_tid = activeThreads->front();
        const auto status = fetchStatus[waiting_tid];
        if (status == ItlbWait || status == IcacheWaitResponse ||
            status == IcacheWaitRetry) {
            auto head = ftq->readHead(waiting_tid);
            if (head && !frontend->dsb->candidate(
                    frontend->identity(pc[waiting_tid]->instAddr()),
                    uint64_t(cpu->curCycle())))
                fetchPredictedBytes(waiting_tid, head);
        }
    }
    const ThreadID tid = getFetchingThread();
    if (tid == InvalidThreadID) {
        threadFetched = numFetchingThreads;
        profileStall(0);
        return;
    }
    if (uint64_t(cpu->curCycle()) < btbReadyCycle[tid]) return;
    if (microcodeControl[tid]) {
        ++fetchStats.microcodeControlWaitCycles;
        if (repStringKind(macroop[tid].get()) != RepStringKind::None)
            ++fetchStats.repControlWaitCycles;
        return;
    }
    if (!frontend->canAccept() || checkStall(tid)) return;
    if (fetchStatus[tid] == IcacheAccessComplete) {
        fetchStatus[tid] = Running;
        status_change = true;
    }
    const bool fetching_successor = byteLookahead[tid] &&
        (fetchStatus[tid] == ItlbWait ||
         fetchStatus[tid] == IcacheWaitResponse ||
         fetchStatus[tid] == IcacheWaitRetry);
    if (fetchStatus[tid] != Running && !fetching_successor) return;

    if (decoupledFrontEnd && !ftq->isHeadReady(tid)) {
        if (!fetching_successor)
            ftqReady(tid, status_change);
        fetchStats.status[FtqWait]++;
        return;
    }

    auto *dec = dynamic_cast<X86ISA::Decoder *>(decoder[tid]);
    assert(dec);
    frontend->context(cpu->tcBase(tid)->readMiscRegNoEffect(X86ISA::misc_reg::Cr3),
                      dec->frontendContext());
    PCStateBase &at = *pc[tid];
    FetchTargetPtr curFT = decoupledFrontEnd ? ftq->readHead(tid) : nullptr;
    auto reportTargetBoundary = [&](const char *reason) {
        static unsigned remaining = 24;
        if (remaining && cpu->tmaStats.slots.value() > 0 && cpu->inUserMode(tid)) {
            --remaining;
            inform("Frontend target boundary %s: PC=%s target=%s history=%i",
                   reason, at, curFT ? curFT->toString() : "none",
                   curFT && curFT->bpuHistory != nullptr);
        }
    };
    if (curFT && !curFT->inRange(at.instAddr())) {
        reportTargetBoundary("entry");
        bacResteer(at, tid);
        return;
    }
    Addr offset = fetchOffset[tid];
    StaticInstPtr current_macro = macroop[tid];
    bool in_rom = isRomMicroPC(at.microPC());
    if (checkInterrupt(at.instAddr()) && !delayedCommit[tid]) return;

    const uint64_t cycle = uint64_t(cpu->curCycle());
    if (lcpDecoded[tid] && cycle < lcpReadyCycle[tid]) {
        ++cpu->decodeStats.LCP;
        return;
    }
    auto pendingTemplateWrite = [&](const FrontendTemplate *value) {
        if (!value) return false;
        const auto count = value->physicalBlocks.size();
        for (size_t i = 0; i < count; ++i) {
            // Invalidation can destroy value, so return immediately on a hit.
            const Addr block = value->physicalBlocks[i];
            if (pendingCodeWrite(tid, block)) return true;
        }
        return false;
    };
    auto lookupValidated = [&]() -> const FrontendTemplate * {
        // A synchronous native ITLB hit completes now. Do not insert a
        // Fetch bubble solely because the code lease needed refreshing.
        // A maximum-length x86 instruction can require two adjacent lines.
        for (unsigned check = 0; check < 2; ++check) {
            const auto *candidate = frontend->dsb->candidate(
                frontend->identity(at.instAddr()), cycle);
            if (!candidate) return frontend->lookup(at.instAddr(), cycle);
            std::vector<Addr> blocks;
            if (frontend->physicalBlocks(at.instAddr(), candidate->bytes, blocks))
                return frontend->lookup(at.instAddr(), cycle);
            fetchCacheLine(frontend->validationAddress(at.instAddr(), cycle),
                           tid, at.instAddr());
            if (fetchStatus[tid] != Running || memReq[tid]) return nullptr;
        }
        return frontend->lookup(at.instAddr(), cycle);
    };
    const FrontendTemplate *cached = nullptr;
    bool loop_replay = false;
    if (!current_macro && !in_rom && !lcpDecoded[tid]) {
        cached = frontend->loopCandidate(at.instAddr());
        loop_replay = cached != nullptr;
        if (!cached) cached = lookupValidated();
        if (fetchStatus[tid] != Running && !fetching_successor) return;
        if (pendingTemplateWrite(cached)) return;
    }
    const bool ms_body = current_macro && current_macro->usesMicrocodeService() &&
        at.microPC() >= current_macro->microcodePrefixOps();
    FrontendSource source = in_rom || ms_body ? FrontendSource::Microcode :
        current_macro ? expansionSource :
        (loop_replay ? FrontendSource::Lsd :
         cached ? FrontendSource::Dsb : FrontendSource::Mite);
    if (frontend->instructionBytes->lookaheadEnabled &&
        frontend->instructionBytes->requestDepth > 1 &&
        source == FrontendSource::Mite && curFT)
        fetchPredictedBytes(tid, curFT);
    if (frontend->instructionBytes->lookaheadEnabled &&
        frontend->instructionBytes->requestDepth == 1 &&
        source == FrontendSource::Mite && curFT && !memReq[tid] &&
        frontend->instructionBytes->contains(at.instAddr())) {
        const Addr successor = curFT->readPredTarg().instAddr();
        if (fetchBufferAlignPC(successor) !=
                fetchBufferAlignPC(at.instAddr()) &&
            !frontend->instructionBytes->contains(successor))
            fetchCacheLine(successor, tid, successor, true);
    }
    if (!frontend->canAccept(source)) return;
    unsigned limit = frontend->sourceService(source)->freeBatchOps();
    if (source == FrontendSource::Microcode)
        limit = std::min(limit, frontend->ms->extractionWidth());
    unsigned instruction_limit = frontend->sourceService(source)->maxInstructions();
    unsigned accepted_instructions = pendingMemoryExpansion.empty() ? 0 : 1;
    unsigned consumed_targets = 0;
    unsigned consumed_taken = 0;
    bool stop = false;
    std::vector<FrontendRecordPtr> records = std::move(pendingMemoryExpansion);
    std::vector<FrontendTemplate> fills;
    AllocationGroupPtr &memoryGroup = memoryExpansionGroup;
    auto sharesAllocation = [](const StaticInstPtr &macro) {
        return macro && (macro->isMicroFusedMemory() ||
                         macro->isStackRegAllocation());
    };

    while (records.size() < limit && !stop) {
        const bool was_in_rom = in_rom;
        if (decoupledFrontEnd && !curFT) {
            if (consumed_targets >= maxFTPerCycle ||
                consumed_taken >= maxTakenPredPerCycle ||
                !ftq->isHeadReady(tid))
                break;
            curFT = ftq->readHead(tid);
            if (!curFT->inRange(at.instAddr())) {
                reportTargetBoundary("next");
                bacResteer(at, tid);
                break;
            }
        }
        StaticInstPtr op;
        if (!current_macro && !in_rom) {
            if (accepted_instructions >= instruction_limit) break;
            StaticInstPtr root;
            if (source == FrontendSource::Dsb || source == FrontendSource::Lsd) {
                if (!cached) cached = source == FrontendSource::Lsd ?
                    frontend->loopCandidate(at.instAddr()) : lookupValidated();
                if (!cached) break;
                if (pendingTemplateWrite(cached)) break;
                panic_if(cached->physicalBlocks.size() > decodedCodeBlocks[tid].size(),
                         "Template spans too many physical code blocks");
                decodedCodeBlockCount[tid] = cached->physicalBlocks.size();
                std::copy(cached->physicalBlocks.begin(), cached->physicalBlocks.end(),
                          decodedCodeBlocks[tid].begin());
                codeDecodeInProgress[tid] = false;
                root = cached->macroop;
                auto &xpc = at.as<X86ISA::PCState>();
                xpc.size(cached->bytes);
                xpc.npc(xpc.pc() + cached->bytes);
                offset = 0;
                dec->reset();
                cached = nullptr;
            } else if (lcpDecoded[tid]) {
                root = std::move(lcpDecoded[tid]);
            } else {
                if (!dsbMissExposed[tid]) {
                    dsbMissExposed[tid] = std::make_shared<bool>(false);
                    frontend->decodingDsbMiss(dsbMissExposed[tid]);
                }
                while (!dec->instReady()) {
                    const Addr address = (at.instAddr() + offset) & dec->pcMask();
                    const Addr chunk = address & ~Addr(instSize - 1);
                    if (!frontend->instructionBytes->contains(chunk) ||
                        !frontend->instructionBytes->contains(chunk + instSize - 1)) {
                        fetchCacheLine(address, tid, at.instAddr());
                        break;
                    }
                    if (pendingCodeWrite(tid,
                            frontend->instructionBytes->physical(chunk))) {
                        dec->reset();
                        offset = 0;
                        clearCodeDecode(tid);
                        expansionTemplate.reset();
                        break;
                    }
                    if (!frontend->instructionBytes->serve(chunk, instSize, cycle))
                        break;
                    const Addr block = frontend->instructionBytes->physical(chunk) &
                                       ~(cacheBlkSize - 1);
                    auto begin = decodedCodeBlocks[tid].begin();
                    auto end = begin + decodedCodeBlockCount[tid];
                    if (std::find(begin, end, block) == end) {
                        panic_if(decodedCodeBlockCount[tid] == decodedCodeBlocks[tid].size(),
                                 "Instruction decode spans too many code blocks");
                        decodedCodeBlocks[tid][decodedCodeBlockCount[tid]++] = block;
                    }
                    codeDecodeInProgress[tid] = true;
                    decodeCanReuse &= frontend->instructionBytes->canReuse(chunk);
                    std::memcpy(dec->moreBytesPtr(),
                                frontend->instructionBytes->at(chunk), instSize);
                    dec->moreBytes(at, address);
                    if (dec->needMoreBytes()) offset += instSize;
                }
                if (!dec->instReady()) break;
                root = dec->decode(at);
                codeDecodeInProgress[tid] = false;
                if (decodeCanReuse) {
                    std::vector<Addr> blocks;
                    const unsigned bytes = at.as<X86ISA::PCState>().size();
                    if (frontend->physicalBlocks(at.instAddr(), bytes, blocks)) {
                        expansionTemplate = FrontendTemplate{
                            frontend->identity(at.instAddr()), root, {},
                            bytes, std::move(blocks), 0};
                    }
                }
                decodeCanReuse = true;
                if (root->hasLengthChangingPrefix()) {
                    // Every qualified encoding in a traversed 16-byte block
                    // consumes its own ILD service. The retained decoded root
                    // prevents retries from charging this occurrence again.
                    lcpDecoded[tid] = root;
                    lcpReadyCycle[tid] = cycle + 3;
                    ++cpu->decodeStats.LCP;
                    break;
                }
            }
            const bool microcoded = root->usesMicrocodeService();
            if (microcoded && !root->microcodePrefixOps() &&
                source != FrontendSource::Microcode) {
                if (!records.empty() ||
                    !frontend->canAccept(FrontendSource::Microcode)) {
                    lcpDecoded[tid] = root;
                    break;
                }
                source = FrontendSource::Microcode;
                limit = std::min(frontend->ms->freeBatchOps(),
                                 frontend->ms->extractionWidth());
                instruction_limit = frontend->ms->maxInstructions();
            }
            ++accepted_instructions;
            ++cpu->fetchStats[tid]->numInsts;
            expansionSource = source;
            if (root->isMacroop()) current_macro = root;
            else { op = root; offset = 0; }
        }

        if (current_macro && current_macro->usesMicrocodeService() &&
            at.microPC() >= current_macro->microcodePrefixOps() &&
            source != FrontendSource::Microcode)
            break;

        if (sharesAllocation(current_macro) &&
            !memoryGroup && at.microPC() != 0) {
            // A precise replay may arrive at a lowered internal operation.
            // Shared units are assembled again from the architectural start.
            const unsigned bytes = at.as<X86ISA::PCState>().size();
            at.set(at.instAddr());
            at.as<X86ISA::PCState>().size(bytes);
            at.as<X86ISA::PCState>().npc(at.instAddr() + bytes);
        }
        if (sharesAllocation(current_macro) &&
            at.microPC() == 0 &&
            records.size() + current_macro->functionalMicroops() > limit)
            break;
        if (in_rom) op = dec->fetchRomMicroop(at.microPC(), current_macro);
        else if (current_macro) op = current_macro->fetchMicroop(at.microPC());
        if (!op) break;

        auto record = std::make_unique<FrontendRecord>(cpu, tid, op, current_macro,
            at, in_rom ? FrontendSource::Microcode : source, true);
        record->dsbMiss = dsbMissExposed[tid] != nullptr;
        record->dsbMissExposed = dsbMissExposed[tid];
        std::vector<Addr> code_blocks;
        const unsigned code_bytes = at.as<X86ISA::PCState>().size();
        bool have_code_blocks = decodedCodeBlockCount[tid] != 0;
        if (have_code_blocks) {
            code_blocks.assign(decodedCodeBlocks[tid].begin(),
                               decodedCodeBlocks[tid].begin() + decodedCodeBlockCount[tid]);
        } else {
            have_code_blocks = frontend->physicalBlocks(
                at.instAddr(), code_bytes, code_blocks);
        }
        if (!have_code_blocks && code_bytes &&
            frontend->instructionBytes->contains(at.instAddr()) &&
            frontend->instructionBytes->contains(at.instAddr() + code_bytes - 1)) {
            // A same-line D-cache replacement can invalidate the lease after
            // translation but before the I-fetch response. The returned byte
            // buffer still identifies the physical bytes actually decoded.
            const Addr first = frontend->instructionBytes->physical(at.instAddr()) &
                               ~(cacheBlkSize - 1);
            const Addr last = frontend->instructionBytes->physical(
                at.instAddr() + code_bytes - 1) & ~(cacheBlkSize - 1);
            code_blocks.clear();
            code_blocks.push_back(first);
            if (last != first) code_blocks.push_back(last);
            have_code_blocks = true;
        }
        if (have_code_blocks) {
            panic_if(code_blocks.size() > record->codeBlocks.size(),
                     "Instruction spans too many physical code blocks");
            record->codeBlockCount = code_blocks.size();
            std::copy(code_blocks.begin(), code_blocks.end(),
                      record->codeBlocks.begin());
            record->staleCode = expansionCodeStale[tid];
        } else {
            DPRINTF(Fetch, "[tid:%i] No physical code identity for [sn:%llu] PC %s\n",
                    tid, record->sequence, at);
        }
        if (sharesAllocation(current_macro) &&
            current_macro->functionalMicroops() > 1) {
            if (!memoryGroup) {
                panic_if(at.microPC() != 0, "Microfused memory group began mid-expansion");
                memoryGroup = std::make_shared<AllocationGroup>(record->sequence,
                    record->sequence + cpu->instSeqStride() *
                        (current_macro->functionalMicroops() - 1), 0, 0);
                memoryGroup->independentBackendEntries =
                    current_macro->isStackRegAllocation();
                if (op->isStoreAddress()) {
                    panic_if((current_macro->functionalMicroops() != 2 &&
                              !(current_macro->isStackRegAllocation() &&
                                current_macro->functionalMicroops() == 3)) ||
                             !current_macro->fetchMicroop(1)->isStoreData(),
                             "Split store must contain one address and one data operation");
                }
            }
            record->allocationGroup = memoryGroup;
        }
        const Cycles target_delay = op->isControl() ?
            bac->btbLookupLatency(tid, at.instAddr()) : Cycles(0);
        const bool supplied_target = curFT &&
            curFT->isExitBranch(at.instAddr()) && curFT->ready(cycle);
        const bool static_microcode_jump =
            op->isMicrocodeControl() && !op->isCondCtrl();
        if (op->isMicrocodeControl()) {
            if (static_microcode_jump) {
                // This internal jump has no operand-dependent decision.
                // Keep its functional record and commit order, but do not
                // wait for a backend round trip to learn the constant UPC.
                auto target = op->branchTarget(at);
                set(record->predictedPC, *target);
            } else {
                op->advancePC(*record->predictedPC);
            }
        } else record->prediction = decoupledFrontEnd ?
            bac->predictFrontendInstruction(op, record->sequence, tid,
                                             *record->predictedPC, curFT) :
            bac->predictInstruction(op, record->sequence, tid,
                                    *record->predictedPC);
        const bool target_exit = curFT && curFT->isExitInst(at.instAddr()) &&
            (!op->isMicroop() || op->isLastMicroop());
        if (target_exit && curFT->isExitBranch(at.instAddr()) && !op->isControl())
            bac->discardNonBranchTarget(tid, at.instAddr());
        stop = at.branching() || record->prediction.taken;
        // Conditional internal branches use static fall-through prediction.
        // Wrong-path records use normal ROB/LSQ squash before any retirement.
        if (record->prediction.taken && target_delay > Cycles(0) && !supplied_target)
            btbReadyCycle[tid] = cycle + uint64_t(target_delay) + 1;
        if (op->isControl()) ++cpu->fetchStats[tid]->numBranches;
        if (stop) ++fetchStats.predictedBranches;
        delayedCommit[tid] = op->isDelayedCommit();
        const bool finished = op->isLastMicroop() ||
            at.instAddr() != record->predictedPC->instAddr();
        if (finished && expansionTemplate)
            expansionTemplate->endsEntry = record->prediction.taken || op->isUncondCtrl();
        if (source == FrontendSource::Mite && expansionTemplate && !in_rom)
            expansionTemplate->operations.push_back(op);
        if (in_rom) expansionTemplate.reset();
        set(at, *record->predictedPC);
        if (curFT && (target_exit || !curFT->inRange(at.instAddr()) ||
                      !ftq->isReady(tid))) {
            ++consumed_targets;
            if (record->prediction.taken) ++consumed_taken;
            if (!ftq->popHead(tid)) {
                reportTargetBoundary("history");
                bacResteer(at, tid);
                stop = true;
            }
            curFT = nullptr;
        }
        const auto single = [](const StaticInstPtr &inst) {
            return !inst->isMicroop() || (inst->isFirstMicroop() && inst->isLastMicroop());
        };
        if (source != FrontendSource::Microcode && single(op) && !records.empty()) {
            auto &previous = records.back();
            const bool producer_ready = single(previous->instruction) ||
                (previous->allocationGroup && previous->allocationGroup->end == previous->sequence);
            if (producer_ready &&
                (!previous->allocationGroup || !previous->allocationGroup->branch) &&
                FrontendCoordinator::fusionEligibleProducer(*previous->instruction) &&
                previous->addressSpace == record->addressSpace &&
                previous->fromUser == record->fromUser &&
                previous->instruction->canMacroFuseWith(*op) &&
                previous->pc->instAddr() + previous->instruction->size() == record->pc->instAddr() &&
                previous->pc->instAddr() / 64 == record->pc->instAddr() / 64) {
                auto group = previous->allocationGroup;
                if (!group) {
                    group = std::make_shared<AllocationGroup>(previous->sequence,
                        record->sequence, previous->sequence, record->sequence);
                } else {
                    group->branchProducer = previous->sequence;
                    group->branch = group->end = record->sequence;
                }
                previous->allocationGroup = group;
                record->allocationGroup = std::move(group);
            }
        }
        records.push_back(std::move(record));
        frontendBatchPending = true;
        if (finished) {
            current_macro = nullptr;
            memoryGroup.reset();
            offset = 0;
            clearCodeDecode(tid);
            dsbMissExposed[tid].reset();
            if (expansionTemplate) {
                fills.push_back(std::move(*expansionTemplate));
                expansionTemplate.reset();
            }
            if (source == FrontendSource::Microcode) stop = true;
        }
        in_rom = isRomMicroPC(at.microPC());
        if (in_rom != was_in_rom) stop = true;
        if (op->isQuiesce()) {
            fetchStatus[tid] = QuiescePending;
            status_change = true;
            stop = true;
        }
    }
    if (consumed_taken > 1) ++fetchStats.multiTakenBatches;
    if (memoryGroup) {
        // Never submit the front half of one allocation unit to the source.
        size_t first = records.size();
        while (first && records[first - 1]->allocationGroup == memoryGroup) --first;
        for (size_t i = first; i < records.size(); ++i)
            pendingMemoryExpansion.push_back(std::move(records[i]));
        records.resize(first);
        if (accepted_instructions) --accepted_instructions;
        cpu->activityThisCycle();
    }
    macroop[tid] = current_macro;
    fetchOffset[tid] = offset;
    numInst += records.size();
    if (!records.empty()) {
        frontend->submit(source, std::move(records), accepted_instructions,
                         std::move(fills), cycle);
        wroteToTimeBuffer = true;
    }
    frontendBatchPending = false;
    fetchStats.status[Running]++;
}

bool
Fetch::byteRequestPending(Addr address, ThreadID tid) const
{
    const Addr block = fetchBufferAlignPC(address);
    for (const auto &[request, state] : byteRequests)
        if (cpu->contextToThread(request->contextId()) == tid &&
            request->getVaddr() == block)
            return true;
    return false;
}

void
Fetch::discardByteLookahead(ThreadID tid)
{
    if (retryPkt && retryTid == tid && byteRequests.count(retryPkt->req)) {
        TaoTraceFrontendRegistry::noteTerminal(retryPkt->req,
            TaoTraceFrontendRegistry::Terminal::RetryDiscard,
            uint64_t(curTick()));
        delete retryPkt;
        retryPkt = nullptr;
        retryTid = InvalidThreadID;
    }
    for (auto it = byteRequests.begin(); it != byteRequests.end();) {
        if (cpu->contextToThread(it->first->contextId()) == tid)
            it = byteRequests.erase(it);
        else
            ++it;
    }
}

bool
Fetch::startByteLookahead(Addr address, ThreadID tid)
{
    const uint64_t cycle = uint64_t(cpu->curCycle());
    const Addr block = fetchBufferAlignPC(address);
    if (cacheBlocked || checkInterrupt(address) || checkStall(tid) ||
        lastByteIssue == cycle ||
        byteRequests.size() >= frontend->instructionBytes->requestDepth - 1 ||
        frontend->instructionBytes->contains(address) ||
        byteRequestPending(address, tid) ||
        (memReq[tid] && memReq[tid]->getVaddr() == block) ||
        frontend->dsb->candidate(frontend->identity(address), cycle))
        return false;
    auto request = std::make_shared<Request>(
        block, fetchBufferSize, Request::INST_FETCH, cpu->instRequestorId(),
        address, cpu->thread[tid]->contextId());
    request->taskId(cpu->taskId());
    byteRequests.emplace(request, ByteRequest{frontend->dsb->epoch()});
    lastByteIssue = cycle;
    TaoTraceFrontendRegistry::noteRequestStart(
        request, cpu->inUserMode(tid), false, false, uint64_t(curTick()));
    cpu->mmu->translateTiming(request, cpu->thread[tid]->getTC(),
                             new FetchTranslation(this), BaseMMU::Execute);
    return true;
}

void
Fetch::fetchPredictedBytes(ThreadID tid, FetchTargetPtr target)
{
    // Issue at most one request per cycle from the already predicted FTQ path.
    // Never walk past a locked target or manufacture an unpredicted address.
    const Addr next_line = fetchBufferAlignPC(pc[tid]->instAddr()) +
                           fetchBufferSize;
    if (next_line <= target->endAddress() &&
        startByteLookahead(next_line, tid))
        return;
    if (ftq->isLocked(tid)) return;
    if (startByteLookahead(target->readPredTarg().instAddr(), tid))
        return;
    for (unsigned i = 1; i <= frontend->instructionBytes->requestDepth; ++i) {
        auto next = ftq->readAhead(tid, i);
        if (!next) break;
        if (startByteLookahead(next->startAddress(), tid))
            return;
    }
}

void
Fetch::finishByteTranslation(const Fault &fault, const RequestPtr &request)
{
    const ThreadID tid = cpu->contextToThread(request->contextId());
    TaoTraceFrontendRegistry::noteTranslationComplete(request);
    const auto state = byteRequests.at(request);
    if (fault != NoFault || state.epoch != frontend->dsb->epoch() ||
        !cpu->system->isMemAddr(request->getPaddr()) ||
        pendingCodeWrite(tid, request->getPaddr()) || cacheBlocked) {
        // Speculative translation faults are reproduced by the demand path.
        TaoTraceFrontendRegistry::noteTerminal(request,
            fault != NoFault ? TaoTraceFrontendRegistry::Terminal::TranslationFault :
                               TaoTraceFrontendRegistry::Terminal::TranslationSquash,
            uint64_t(curTick()));
        byteRequests.erase(request);
        return;
    }
    frontend->translated(request->getVaddr(), request->getPaddr());
    auto *packet = new Packet(request, MemCmd::ReadReq);
    packet->dataDynamic(new uint8_t[fetchBufferSize]);
    ++fetchStats.cacheLines;
    const bool accepted = icachePort.sendTimingReq(packet);
    TaoTraceFrontendRegistry::noteSendAttempt(request, accepted);
    if (accepted) {
        byteRequests.at(request).response = true;
        ppFetchRequestSent->notify(request);
    } else {
        assert(!retryPkt);
        retryPkt = packet;
        retryTid = tid;
        cacheBlocked = true;
    }
}

bool
Fetch::completeByteLookahead(PacketPtr packet)
{
    const auto it = byteRequests.find(packet->req);
    if (it == byteRequests.end()) return false;
    const ThreadID tid = cpu->contextToThread(packet->req->contextId());
    const auto validity = packet->req->getExtension<InstructionCodeValidity>();
    const bool valid = it->second.epoch == frontend->dsb->epoch() &&
        !(validity && validity->stale) &&
        !pendingCodeWrite(tid, packet->req->getPaddr());
    if (valid) {
        frontend->instructionBytes->complete(packet->req->getVaddr(),
            packet->req->getPaddr(), packet->getConstPtr<uint8_t>(),
            fetchBufferSize, uint64_t(cpu->curCycle()),
            !packet->req->isUncacheable());
        packet->req->setAccessLatency();
        cpu->ppInstAccessComplete->notify(packet);
    }
    TaoTraceFrontendRegistry::noteTerminal(packet->req,
        valid ? TaoTraceFrontendRegistry::Terminal::Response :
                TaoTraceFrontendRegistry::Terminal::SquashedResponse,
        uint64_t(curTick()));
    byteRequests.erase(it);
    delete packet;
    cpu->wakeCPU();
    switchToActive();
    return true;
}

void
Fetch::invalidateCode(Addr physical_block)
{
    if (!frontend) return;
    if (frontend->instructionBytes->matchesPhysicalBlock(
            physical_block, cacheBlkSize)) {
        for (ThreadID tid = 0; tid < numThreads; ++tid)
            fetchBufferValid[tid] = false;
    }
    frontend->invalidateCode(physical_block);
    expansionTemplate.reset();
    decodeCanReuse = false;
}

bool
Fetch::markCodeWrite(ThreadID tid, InstSeqNum after, Addr physical_block)
{
    bool matched = false;
    for (auto &record : pendingMemoryExpansion)
        matched |= record->markCodeWrite(tid, after, physical_block);
    if (frontend)
        matched |= frontend->markCodeWrite(tid, after, physical_block);
    auto begin = decodedCodeBlocks[tid].begin();
    auto end = begin + decodedCodeBlockCount[tid];
    if (std::find(begin, end, physical_block) != end) {
        if ((codeDecodeInProgress[tid] || lcpDecoded[tid]) && !macroop[tid]) {
            // A decoded root waiting for ILD/MS service is still frontend
            // work. An older code write must discard it before materializing.
            lcpDecoded[tid] = nullptr;
            lcpReadyCycle[tid] = 0;
            dsbMissExposed[tid].reset();
            decoder[tid]->reset();
            fetchOffset[tid] = 0;
            clearCodeDecode(tid);
            expansionTemplate.reset();
        } else if (macroop[tid]) {
            expansionCodeStale[tid] = true;
        }
        matched = true;
    }
    return matched;
}

bool
Fetch::pendingCodeWrite(ThreadID tid, Addr physical_block)
{
    physical_block &= ~(cacheBlkSize - 1);
    if (!cpu->hasPendingCodeWrite(tid, physical_block)) return false;
    invalidateCode(physical_block);
    fetchBufferValid[tid] = false;
    DPRINTF(Fetch, "[tid:%i] Waiting for an older write to code line %#x\n",
            tid, physical_block);
    return true;
}

void
Fetch::invalidateTranslations()
{
    if (!frontend) return;
    frontend->invalidateTranslations();
    expansionTemplate.reset();
    decodeCanReuse = false;
}

void
Fetch::IcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    if (pkt->isInvalidate()) fetch->invalidateCode(pkt->getAddr());
}
}
