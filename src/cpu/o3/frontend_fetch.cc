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
    if (curFT && !curFT->inRange(at.instAddr())) {
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
    FrontendSource source = current_macro ? expansionSource :
        (cached ? FrontendSource::Dsb : FrontendSource::Mite);
    if (!frontend->canAccept(source)) return;
    const unsigned limit = source == FrontendSource::Dsb ?
        frontend->dsb->freeBatchOps() : frontend->mite->freeBatchOps();
    const unsigned instruction_limit = source == FrontendSource::Dsb ?
        frontend->dsb->maxInstructions() : frontend->mite->maxInstructions();
    unsigned accepted_instructions = 0;
    unsigned consumed_targets = 0;
    unsigned consumed_taken = 0;
    bool stop = false;
    std::vector<FrontendRecordPtr> records;
    std::vector<FrontendTemplate> fills;

    while (records.size() < limit && !stop) {
        if (decoupledFrontEnd && !curFT) {
            if (consumed_targets >= maxFTPerCycle ||
                consumed_taken >= maxTakenPredPerCycle ||
                !ftq->isHeadReady(tid))
                break;
            curFT = ftq->readHead(tid);
            if (!curFT->inRange(at.instAddr())) {
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
                            bytes, std::move(blocks), 0};
                    }
                }
                decodeCanReuse = true;
            }
            ++accepted_instructions;
            ++cpu->fetchStats[tid]->numInsts;
            expansionSource = source;
            if (root->isMacroop()) current_macro = root;
            else { op = root; offset = 0; }
        }

        if (in_rom) op = dec->fetchRomMicroop(at.microPC(), current_macro);
        else if (current_macro) op = current_macro->fetchMicroop(at.microPC());
        if (!op) break;

        auto record = std::make_unique<FrontendRecord>(cpu, tid, op, current_macro,
            at, in_rom ? FrontendSource::Rom : source, true);
        record->prediction = decoupledFrontEnd ?
            bac->predictFrontendInstruction(op, record->sequence, tid,
                                             *record->predictedPC, curFT) :
            bac->predictInstruction(op, record->sequence, tid,
                                    *record->predictedPC);
        const bool target_exit = curFT && curFT->isExitInst(at.instAddr()) &&
            (!op->isMicroop() || op->isLastMicroop());
        stop = at.branching() || record->prediction.taken;
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
                bacResteer(at, tid);
                stop = true;
            }
            curFT = nullptr;
        }
        records.push_back(std::move(record));
        frontendBatchPending = true;
        if (finished) {
            current_macro = nullptr;
            offset = 0;
            if (expansionTemplate) {
                fills.push_back(std::move(*expansionTemplate));
                expansionTemplate.reset();
            }
        }
        in_rom = isRomMicroPC(at.microPC());
        if (op->isQuiesce()) {
            fetchStatus[tid] = QuiescePending;
            status_change = true;
            stop = true;
        }
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
