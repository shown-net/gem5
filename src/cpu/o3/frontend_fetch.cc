// SPDX-License-Identifier: BSD-3-Clause
// Native request and x86 functional-decode adapter for the component frontend.
#include "cpu/o3/fetch.hh"
#include <cstring>
#include "arch/x86/decoder.hh"
#include "arch/x86/pcstate.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/Fetch.hh"
#include "params/BaseO3CPU.hh"

namespace gem5::o3
{
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
    const ThreadID tid = getFetchingThread();
    if (tid == InvalidThreadID) {
        threadFetched = numFetchingThreads;
        profileStall(0);
        return;
    }
    if (uint64_t(cpu->curCycle()) < btbReadyCycle[tid]) return;
    if (microcodeControl[tid]) return;
    if (!frontend->canAccept() || checkStall(tid)) return;
    if (fetchStatus[tid] == IcacheAccessComplete) {
        fetchStatus[tid] = Running;
        status_change = true;
    }
    if (fetchStatus[tid] != Running) return;

    if (decoupledFrontEnd && !ftqReady(tid, status_change)) {
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
    if (!current_macro && !in_rom) {
        cached = lookupValidated();
        if (fetchStatus[tid] != Running) return;
    }
    FrontendSource source = in_rom ? FrontendSource::Microcode :
        current_macro ? expansionSource :
        (cached ? FrontendSource::Dsb : FrontendSource::Mite);
    if (!frontend->canAccept(source)) return;
    unsigned limit = frontend->sourceService(source)->freeBatchOps();
    unsigned instruction_limit = frontend->sourceService(source)->maxInstructions();
    unsigned accepted_instructions = pendingMemoryExpansion.empty() ? 0 : 1;
    unsigned consumed_targets = 0;
    unsigned consumed_taken = 0;
    bool stop = false;
    std::vector<FrontendRecordPtr> records = std::move(pendingMemoryExpansion);
    std::vector<FrontendTemplate> fills;
    AllocationGroupPtr &memoryGroup = memoryExpansionGroup;
    auto countGroupResources = [](const AllocationGroupPtr &group,
                                  const StaticInstPtr &instruction) {
        for (unsigned c = 0; c < group->renameDests.size(); ++c)
            group->renameDests[c] += instruction->numDestRegs(RegClassType(c));
        group->loadMembers += instruction->isLoad();
    };
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
            if (source == FrontendSource::Dsb) {
                if (!cached) cached = lookupValidated();
                if (!cached) break;
                root = cached->macroop;
                auto &xpc = at.as<X86ISA::PCState>();
                xpc.size(cached->bytes);
                xpc.functionalBytes(cached->functionalBytes,cached->functionalByteCount,
                                    cached->functionalDecoderContext);
                if (cpu->instructionTranslationObservation)
                    xpc.instructionTranslation(frontend->instructionTranslation(at.instAddr(),cached->bytes));
                xpc.npc(xpc.pc() + cached->bytes);
                offset = 0;
                dec->reset();
                cached = nullptr;
            } else {
                while (!dec->instReady()) {
                    const Addr address = (at.instAddr() + offset) & dec->pcMask();
                    const Addr chunk = address & ~Addr(instSize - 1);
                    if (!frontend->instructionBytes->contains(chunk) ||
                        !frontend->instructionBytes->contains(chunk + instSize - 1)) {
                        fetchCacheLine(address, tid, at.instAddr());
                        break;
                    }
                    if (!frontend->instructionBytes->serve(chunk, instSize, cycle))
                        break;
                    decodeCanReuse &= frontend->instructionBytes->canReuse();
                    std::memcpy(dec->moreBytesPtr(),
                                frontend->instructionBytes->at(chunk), instSize);
                    if (cpu->instructionTranslationObservation)
                        dec->observeInstructionChunk(frontend->instructionBytes->instructionTranslation(chunk,instSize),
                            cpu->tcBase(tid)->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) & ~uint64_t(0xfff));
                    dec->moreBytes(at, address);
                    if (dec->needMoreBytes()) offset += instSize;
                }
                if (!dec->instReady()) break;
                root = dec->decode(at);
                if (decodeCanReuse) {
                    std::vector<Addr> blocks;
                    const unsigned bytes = at.as<X86ISA::PCState>().size();
                    if (frontend->physicalBlocks(at.instAddr(), bytes, blocks)) {
                        expansionTemplate = FrontendTemplate{
                            frontend->identity(at.instAddr()), root, {},
                            bytes, std::move(blocks), 0, sharesAllocation(root),
                            at.as<X86ISA::PCState>().functionalBytes(),
                            at.as<X86ISA::PCState>().functionalByteCount(),
                            at.as<X86ISA::PCState>().functionalDecoderContext()};
                    }
                }
                decodeCanReuse = true;
            }
            ++accepted_instructions;
            ++cpu->fetchStats[tid]->numInsts;
            const bool microcoded = root->usesMicrocodeService();
            expansionSource = microcoded ? FrontendSource::Microcode : source;
            if (root->isMacroop()) current_macro = root;
            else { op = root; offset = 0; }
            if (microcoded && source != FrontendSource::Microcode) {
                expansionTemplate.reset();
                if (!records.empty() ||
                    !frontend->canAccept(FrontendSource::Microcode)) break;
                source = FrontendSource::Microcode;
                limit = frontend->ms->freeBatchOps();
                instruction_limit = frontend->ms->maxInstructions();
            }
        }

        if (sharesAllocation(current_macro) &&
            !memoryGroup && at.microPC() != 0) {
            // A precise replay may arrive at a lowered internal operation.
            // Shared units are assembled again from the architectural start.
            // Rewind the same immutable macro, preserving its decoded code
            // witness while set() resets the internal micro-PC state.
            const auto decoded = at.as<X86ISA::PCState>();
            at.set(at.instAddr());
            auto& replay = at.as<X86ISA::PCState>();
            replay.size(decoded.size());
            replay.npc(at.instAddr() + decoded.size());
            replay.functionalBytes(decoded.functionalBytes(),
                                   decoded.functionalByteCount(),
                                   decoded.functionalDecoderContext());
            replay.instructionTranslation(decoded.instructionTranslation());
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
        if (sharesAllocation(current_macro) &&
            current_macro->functionalMicroops() > 1) {
            if (!memoryGroup) {
                panic_if(at.microPC() != 0, "Microfused memory group began mid-expansion");
                memoryGroup = std::make_shared<AllocationGroup>(record->sequence,
                    record->sequence + current_macro->functionalMicroops() - 1, 0, 0);
                memoryGroup->independentBackendEntries =
                    current_macro->isStackRegAllocation();
                if (op->isStoreAddress()) {
                    panic_if(current_macro->functionalMicroops() != 2 ||
                             !current_macro->fetchMicroop(1)->isStoreData(),
                             "Split store must contain one address and one data operation");
                    memoryGroup->independentIQEntries = true;
                }
                for (unsigned m = 0; m < current_macro->functionalMicroops(); ++m)
                    countGroupResources(memoryGroup, current_macro->fetchMicroop(m));
                if (!memoryGroup->independentIQEntries &&
                    !memoryGroup->independentBackendEntries) {
                    // Share one scheduler reservation only when one configured
                    // IQ accepts every lowered member of the fused operation.
                    const auto &iqs = static_cast<const BaseO3CPUParams &>(
                        cpu->params()).instQueues;
                    bool compatible = false;
                    for (auto *iq : iqs) {
                        bool accepts_all = true;
                        for (unsigned m = 0;
                             m < current_macro->functionalMicroops(); ++m) {
                            if (!iq->allows(
                                    current_macro->fetchMicroop(m)->opClass())) {
                                accepts_all = false;
                                break;
                            }
                        }
                        if (accepts_all) {
                            compatible = true;
                            break;
                        }
                    }
                    memoryGroup->independentIQEntries = !compatible;
                }
            }
            record->allocationGroup = memoryGroup;
        }
        const Cycles target_delay = op->isControl() ?
            bac->btbLookupLatency(tid, at.instAddr()) : Cycles(0);
        const bool supplied_target = curFT &&
            curFT->isExitBranch(at.instAddr()) && curFT->ready(cycle);
        if (op->isMicrocodeControl()) {
            op->advancePC(*record->predictedPC);
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
        if (op->isMicrocodeControl()) {
            microcodeControl[tid] = record->sequence;
            stop = true;
        }
        if (record->prediction.taken && target_delay > Cycles(0) && !supplied_target)
            btbReadyCycle[tid] = cycle + uint64_t(target_delay) + 1;
        if (op->isControl()) ++cpu->fetchStats[tid]->numBranches;
        if (stop) ++fetchStats.predictedBranches;
        delayedCommit[tid] = op->isDelayedCommit();
        const bool finished = op->isLastMicroop() ||
            at.instAddr() != record->predictedPC->instAddr();
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
                previous->instruction->intExecution().family == IntExecutionFamily::ALU &&
                previous->addressSpace == record->addressSpace &&
                previous->fromUser == record->fromUser &&
                previous->instruction->canMacroFuseWith(*op) &&
                previous->pc->instAddr() + previous->instruction->size() == record->pc->instAddr() &&
                previous->pc->instAddr() / 64 == record->pc->instAddr() / 64) {
                auto group = previous->allocationGroup;
                if (!group) {
                    group = std::make_shared<AllocationGroup>(previous->sequence,
                        record->sequence, previous->sequence, record->sequence);
                    countGroupResources(group, previous->instruction);
                } else {
                    group->branchProducer = previous->sequence;
                    group->branch = group->end = record->sequence;
                }
                countGroupResources(group, op);
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

void
Fetch::invalidateCode(Addr physical_block)
{
    if (!frontend) return;
    frontend->invalidateCode(physical_block);
    expansionTemplate.reset();
    decodeCanReuse = false;
}

void
Fetch::resolveMicrocodeControl(const DynInstPtr &inst, const PCStateBase &next)
{
    const ThreadID tid = inst->threadNumber;
    if (microcodeControl[tid] != inst->seqNum) return;
    microcodeControl[tid] = 0;
    set(pc[tid], next);
    fetchOffset[tid] = 0;
    cpu->wakeCPU();
    cpu->activityThisCycle();
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
