#include "cpu/o3/probe/peregrine_trace.hh"

#include <algorithm>

#include "arch/arm/pcstate.hh"
#include "base/logging.hh"
#include "base/statistics.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/op_class.hh"
#include "enums/OpClass.hh"
#include "proto/peregrine_trace.pb.h"
#include "sim/core.hh"

namespace gem5
{
namespace
{

uint32_t
classFlagsFor(const o3::DynInstPtr &inst, bool instSync)
{
    uint32_t flags = 0;
    const OpClass opc = inst->opClass();
    if (opc == enums::IntAlu)
        flags |= 1u;
    if (opc == enums::IntMult || opc == enums::IntDiv)
        flags |= 2u;
    if ((opc >= enums::SimdAdd && opc <= enums::SimdDotProd) ||
        (opc >= enums::Matrix && opc <= enums::MatrixOP) ||
        (opc >= enums::SimdExt && opc <= enums::SimdBf16MultAcc))
        flags |= 4u;
    if ((opc >= enums::FloatAdd && opc <= enums::FloatSqrt) ||
        (opc >= enums::SimdFloatAdd && opc <= enums::SimdFloatSqrt) ||
        (opc >= enums::SimdFloatReduceAdd &&
         opc <= enums::SimdFloatReduceCmp) ||
        (opc >= enums::Bf16Cvt && opc <= enums::Bf16Cvt))
        flags |= 8u;
    if (opc == enums::FloatMult || opc == enums::FloatDiv ||
        opc == enums::FloatSqrt || opc == enums::FloatMultAcc ||
        opc == enums::SimdFloatMult || opc == enums::SimdFloatDiv ||
        opc == enums::SimdFloatSqrt || opc == enums::SimdFloatMultAcc ||
        opc == enums::SimdFloatMatMultAcc || opc == enums::SimdBf16Mult ||
        opc == enums::SimdBf16MultAcc || opc == enums::SimdBf16MatMultAcc)
        flags |= 16u;
    if (inst->isLoad())
        flags |= 32u;
    if (inst->isStore())
        flags |= 64u;
    if (inst->isControl())
        flags |= 128u;
    if (instSync)
        flags |= 256u;
    return flags;
}

uint32_t
branchTypeFor(const StaticInstPtr &inst)
{
    if (!inst->isControl())
        return 0;
    if (inst->isCondCtrl())
        return 1;
    if (inst->isIndirectCtrl())
        return 3;
    return 2;
}

bool
isCanonicalMemoryOp(const OpClass opc)
{
    return opc == enums::MemRead || opc == enums::MemWrite ||
           opc == enums::FloatMemRead || opc == enums::FloatMemWrite;
}

uint32_t
fixedExecutionLatencyFor(const OpClass opc)
{
    // Fixed latencies of the canonical BaseO3CPU FUPool. Memory latency is
    // annotated separately by Anamol's configuration-specific cache model.
    switch (opc) {
      case enums::IntMult:
        return 3;
      case enums::IntDiv:
        return 20;
      case enums::FloatAdd:
      case enums::FloatCmp:
      case enums::FloatCvt:
      case enums::Bf16Cvt:
        return 2;
      case enums::FloatMult:
        return 4;
      case enums::FloatMultAcc:
        return 5;
      case enums::FloatMisc:
        return 3;
      case enums::FloatDiv:
        return 12;
      case enums::FloatSqrt:
        return 24;
      default:
        return 1;
    }
}

} // namespace

PeregrineTrace::PeregrineTrace(const PeregrineTraceParams &p)
    : ProbeListenerObject(p), traceStream(nullptr),
      chunkRecords(p.chunk_records),
      memDepLookback(p.mem_dep_lookback)
{
    traceStream = new ZstdProtoOutputStream(
        simout.resolve(p.output_file), p.zstd_level);
    statistics::registerDumpCallback([this]() { onStatsDump(); });
    registerExitCallback([this]() { closeStream(); });
}

PeregrineTrace::~PeregrineTrace()
{
    closeStream();
}

void
PeregrineTrace::closeStream()
{
    flushChunk();
    delete traceStream;
    traceStream = nullptr;
}

void
PeregrineTrace::flushChunk()
{
    if (!traceStream || records.empty())
        return;
    ProtoMessage::AnamolTraceChunk chunk;
    chunk.set_section_index(sectionIndex);
    chunk.add_dep_offsets(0);
    chunk.add_read_offsets(0);
    chunk.add_write_offsets(0);
    chunk.add_micro_op_offsets(0);
    chunk.add_micro_op_dep_offsets(0);
    chunk.add_micro_op_read_offsets(0);
    chunk.add_micro_op_write_offsets(0);
    for (size_t index = 0; index < records.size(); ++index) {
        const auto &record = records[index];
        chunk.add_ids(record.id);
        chunk.add_ips(record.pc);
        chunk.add_class_flags(record.classFlags);
        chunk.add_branch_types(record.branchType);
        chunk.add_branch_taken(record.branchTaken);
        chunk.add_branch_target_addrs(record.branchTarget);
        chunk.add_committed_control_ops(record.committedControlOps);
        chunk.add_committed_memory_ops(record.committedMemoryOps);
        chunk.add_committed_micro_ops(record.committedMicroOps);
        chunk.add_fixed_execution_latencies(record.fixedExecutionLatency);
        chunk.add_committed_int_alu_ops(record.committedIntAluOps);
        chunk.add_committed_int_mult_ops(record.committedIntMultOps);
        chunk.add_committed_int_div_ops(record.committedIntDivOps);
        chunk.add_committed_fp_alu_ops(record.committedFpAluOps);
        chunk.add_committed_fp_mult_div_ops(record.committedFpMultDivOps);
        chunk.add_committed_simd_ops(record.committedSimdOps);
        chunk.add_committed_matrix_ops(record.committedMatrixOps);
        chunk.add_committed_load_ops(record.committedLoadOps);
        chunk.add_committed_store_ops(record.committedStoreOps);
        chunk.add_committed_system_ops(record.committedSystemOps);
        for (auto dep : record.regDeps)
            chunk.add_dep_ids(dep);
        for (auto dep : record.memDeps)
            chunk.add_dep_ids(dep);
        chunk.add_dep_offsets(chunk.dep_ids_size());
        for (const auto &access : record.reads) {
            chunk.add_read_addresses(access.addr);
            chunk.add_read_sizes(access.size);
        }
        chunk.add_read_offsets(chunk.read_addresses_size());
        for (const auto &access : record.writes) {
            chunk.add_write_addresses(access.addr);
            chunk.add_write_sizes(access.size);
        }
        chunk.add_write_offsets(chunk.write_addresses_size());
        for (const auto &micro : record.microOps) {
            chunk.add_micro_op_ids(micro.id);
            chunk.add_micro_op_class_flags(micro.classFlags);
            chunk.add_micro_op_fixed_execution_latencies(
                micro.fixedExecutionLatency);
            for (auto dep : micro.deps)
                chunk.add_micro_op_dep_ids(dep);
            chunk.add_micro_op_dep_offsets(chunk.micro_op_dep_ids_size());
            for (const auto &access : micro.reads) {
                chunk.add_micro_op_read_addresses(access.addr);
                chunk.add_micro_op_read_sizes(access.size);
            }
            chunk.add_micro_op_read_offsets(
                chunk.micro_op_read_addresses_size());
            for (const auto &access : micro.writes) {
                chunk.add_micro_op_write_addresses(access.addr);
                chunk.add_micro_op_write_sizes(access.size);
            }
            chunk.add_micro_op_write_offsets(
                chunk.micro_op_write_addresses_size());
        }
        chunk.add_micro_op_offsets(chunk.micro_op_ids_size());
    }
    traceStream->writeDelimited(chunk);
    records.clear();
}

void
PeregrineTrace::onStatsDump()
{
    if (!traceStream)
        return;
    flushChunk();
    ++sectionIndex;
}

void
PeregrineTrace::regProbeListeners()
{
    using DynInstListener =
        ProbeListenerArg<PeregrineTrace, o3::DynInstPtr>;

    connectListener<DynInstListener>(
        this, "Execute", &PeregrineTrace::traceExecute);

    connectListener<ProbeListenerArg<
        PeregrineTrace, std::pair<o3::DynInstPtr, PacketPtr>>>(
            this, "DataAccessComplete",
            &PeregrineTrace::traceDataAccess);

    connectListener<DynInstListener>(
        this, "Commit", &PeregrineTrace::traceCommit);
}

void
PeregrineTrace::traceExecute(const o3::DynInstPtr &inst)
{
    onExecute(inst);
}

void
PeregrineTrace::traceDataAccess(
        const std::pair<o3::DynInstPtr, PacketPtr> &arg)
{
    onDataAccess(arg.first, arg.second);
}

void
PeregrineTrace::traceCommit(const o3::DynInstPtr &inst)
{
    onCommit(inst);
}

void
PeregrineTrace::pruneMemWrites(uint64_t currentKey)
{
    const uint64_t currentSeq = currentKey & 0x0000FFFFFFFFFFFFULL;
    if (currentSeq <= memDepLookback)
        return;
    const uint64_t horizon = currentSeq - memDepLookback;
    for (auto it = lastMemWriteSequenceId.begin();
         it != lastMemWriteSequenceId.end();) {
        if ((it->second & 0x0000FFFFFFFFFFFFULL) < horizon)
            it = lastMemWriteSequenceId.erase(it);
        else
            ++it;
    }
}

void
PeregrineTrace::onCommit(const o3::DynInstPtr &inst)
{
    auto &pc = inst->pcState();
    auto &sInst = inst->staticInst;
    const uint64_t depKey = snapshotKey(inst->threadNumber, inst->seqNum);
    auto [pendingIt, inserted] =
        pendingMacros.try_emplace(inst->threadNumber);
    auto &pending = pendingIt->second;
    if (inserted) {
        pending.dependencyKey = depKey;
        pending.pc = pc.instAddr();
    }
    if (sInst->isControl())
        ++pending.committedControlOps;
    if (isCanonicalMemoryOp(inst->opClass()))
        ++pending.committedMemoryOps;
    ++pending.committedMicroOps;
    const OpClass opc = inst->opClass();
    pending.committedIntAluOps += opc == enums::IntAlu;
    pending.committedIntMultOps += opc == enums::IntMult;
    pending.committedIntDivOps += opc == enums::IntDiv;
    pending.committedFpAluOps +=
        opc == enums::FloatAdd || opc == enums::FloatCmp ||
        opc == enums::FloatCvt || opc == enums::Bf16Cvt;
    pending.committedFpMultDivOps +=
        opc >= enums::FloatMult && opc <= enums::FloatSqrt;
    pending.committedSimdOps +=
        (opc >= enums::SimdAdd && opc <= enums::SimdDotProd) ||
        (opc >= enums::SimdExt && opc <= enums::SimdBf16MultAcc);
    pending.committedMatrixOps +=
        opc >= enums::Matrix && opc <= enums::MatrixOP;
    pending.committedLoadOps += inst->isLoad();
    pending.committedStoreOps += inst->isStore();
    pending.committedSystemOps += opc == enums::System;
    pending.fixedExecutionLatency = std::max(
        pending.fixedExecutionLatency,
        fixedExecutionLatencyFor(inst->opClass()));

    bool branchTaken = false;
    if (inst->isControl()) {
        branchTaken = pc.branching();
        pending.branchType = branchTypeFor(sInst);
        pending.branchTaken = branchTaken;
        pending.branchTarget =
            branchTaken ? pc.as<ArmISA::PCState>().npc() : 0;
    }

    const bool instSync =
        sInst->isFullMemBarrier() ||
        sInst->isReadBarrier() ||
        sInst->isWriteBarrier() ||
        sInst->isHtmStart() ||
        sInst->isHtmStop() ||
        sInst->isHtmCancel() ||
        sInst->isSyscall();
    pending.classFlags |= classFlagsFor(inst, instSync);

    std::vector<uint64_t> regDependencySequenceIds;
    for (size_t index = 0; index < inst->numSrcRegs(); ++index) {
        const RegKey key{inst->threadNumber, inst->srcRegIdx(index)};
        auto it = lastRegWriteSequenceId.find(key);
        if (it != lastRegWriteSequenceId.end()) {
            const uint64_t dependencySequenceId = it->second;
            if (dependencySequenceId != depKey &&
                std::find(
                    regDependencySequenceIds.begin(),
                    regDependencySequenceIds.end(),
                    dependencySequenceId) == regDependencySequenceIds.end()) {
                regDependencySequenceIds.push_back(dependencySequenceId);
            }
        }
    }
    for (size_t index = 0; index < inst->numDestRegs(); ++index)
        lastRegWriteSequenceId[
            {inst->threadNumber, inst->destRegIdx(index)}] =
                depKey;
    DepSnapshot snap;
    const uint64_t threadBegin = snapshotKey(inst->threadNumber, 0);
    depSnapshots.erase(
        depSnapshots.lower_bound(threadBegin),
        depSnapshots.lower_bound(depKey));
    auto it = depSnapshots.find(depKey);
    if (it != depSnapshots.end()) {
        snap = it->second;
        depSnapshots.erase(it);
    }
    snap.regDependencySequenceIds = regDependencySequenceIds;

    if (snap.writeAddrs.empty() && inst->isStore() && inst->effAddrValid()) {
        snap.writeAddrs.push_back({inst->effAddr, inst->effSize});
    }

    for (const auto &access : snap.readAddrs) {
        for (unsigned offset = 0; offset < access.size; ++offset) {
            auto writer = lastMemWriteSequenceId.find(access.addr + offset);
            if (writer == lastMemWriteSequenceId.end())
                continue;
            if (std::find(
                    snap.memDependencySequenceIds.begin(),
                    snap.memDependencySequenceIds.end(),
                    writer->second) == snap.memDependencySequenceIds.end()) {
                snap.memDependencySequenceIds.push_back(writer->second);
            }
        }
    }
    for (const auto &access : snap.writeAddrs) {
        for (unsigned offset = 0; offset < access.size; ++offset)
            lastMemWriteSequenceId[access.addr + offset] =
                depKey;
    }
    std::vector<uint32_t> microDeps;
    auto appendMicroDeps = [&](const std::vector<uint64_t> &dependencies) {
        for (auto dependency : dependencies) {
            auto emitted = emittedMicroOpIds.find(dependency);
            if (emitted != emittedMicroOpIds.end())
                microDeps.push_back(emitted->second);
        }
    };
    appendMicroDeps(snap.regDependencySequenceIds);
    appendMicroDeps(snap.memDependencySequenceIds);
    std::sort(microDeps.begin(), microDeps.end());
    microDeps.erase(
        std::unique(microDeps.begin(), microDeps.end()), microDeps.end());
    const uint32_t microOpId = nextMicroOpId++;
    emittedMicroOpIds[depKey] = microOpId;
    pending.microDependencyKeys.push_back(depKey);
    pending.microOps.push_back({
        microOpId,
        classFlagsFor(inst, instSync),
        fixedExecutionLatencyFor(inst->opClass()),
        std::move(microDeps),
        snap.readAddrs,
        snap.writeAddrs,
    });
    pending.regDependencySequenceIds.insert(
        pending.regDependencySequenceIds.end(),
        snap.regDependencySequenceIds.begin(),
        snap.regDependencySequenceIds.end());
    pending.reads.insert(
        pending.reads.end(), snap.readAddrs.begin(), snap.readAddrs.end());
    pending.writes.insert(
        pending.writes.end(), snap.writeAddrs.begin(), snap.writeAddrs.end());
    pending.memDependencySequenceIds.insert(
        pending.memDependencySequenceIds.end(),
        snap.memDependencySequenceIds.begin(),
        snap.memDependencySequenceIds.end());

    const bool macroBoundary = !inst->isMicroop() || inst->isLastMicroop();
    if (!macroBoundary)
        return;
    PendingMacro macro = std::move(pending);
    pendingMacros.erase(pendingIt);

    std::vector<uint32_t> regDeps;
    for (auto dep : macro.regDependencySequenceIds) {
        auto emitted = emittedRecordIds.find(dep);
        if (emitted != emittedRecordIds.end())
            regDeps.push_back(emitted->second);
    }
    std::vector<uint32_t> memDeps;
    for (auto dep : macro.memDependencySequenceIds) {
        auto emitted = emittedRecordIds.find(dep);
        if (emitted != emittedRecordIds.end())
            memDeps.push_back(emitted->second);
    }

    std::sort(regDeps.begin(), regDeps.end());
    regDeps.erase(std::unique(regDeps.begin(), regDeps.end()), regDeps.end());
    std::sort(memDeps.begin(), memDeps.end());
    memDeps.erase(std::unique(memDeps.begin(), memDeps.end()), memDeps.end());

    const uint32_t recordId = nextRecordId++;
    for (auto dependencyKey : macro.microDependencyKeys)
        emittedRecordIds[dependencyKey] = recordId;
    records.push_back({
        recordId,
        macro.pc,
        macro.classFlags,
        macro.branchType,
        macro.branchTaken,
        macro.branchTarget,
        macro.committedControlOps,
        macro.committedMemoryOps,
        macro.committedMicroOps,
        macro.fixedExecutionLatency,
        macro.committedIntAluOps,
        macro.committedIntMultOps,
        macro.committedIntDivOps,
        macro.committedFpAluOps,
        macro.committedFpMultDivOps,
        macro.committedSimdOps,
        macro.committedMatrixOps,
        macro.committedLoadOps,
        macro.committedStoreOps,
        macro.committedSystemOps,
        regDeps,
        std::move(macro.reads),
        std::move(macro.writes),
        memDeps,
        std::move(macro.microOps),
    });
    if (records.size() == chunkRecords) {
        flushChunk();
        pruneMemWrites(depKey);
    }
}

void
PeregrineTrace::onExecute(const o3::DynInstPtr &inst)
{
    const uint64_t depKey = snapshotKey(inst->threadNumber, inst->seqNum);
    depSnapshots[depKey] = DepSnapshot{{}, {}, {}, {}};
}

void
PeregrineTrace::onDataAccess(const o3::DynInstPtr &inst, PacketPtr pkt)
{
    if (!pkt)
        return;

    const Addr addr = inst->effAddrValid() ? inst->effAddr : pkt->getAddr();
    const unsigned size =
        inst->effAddrValid() ? inst->effSize : pkt->getSize();

    const bool isRead = inst->isLoad() || inst->isStoreConditional() ||
                        inst->isAtomic() || pkt->isRead();
    const bool isWrite = inst->isStore() || inst->isStoreConditional() ||
                         inst->isAtomic() || pkt->isWrite();

    if (!isRead && !isWrite)
        return;

    const uint64_t depKey = snapshotKey(inst->threadNumber, inst->seqNum);
    auto &snap = depSnapshots[depKey];
    if (isRead)
        snap.readAddrs.push_back({addr, size});
    if (isWrite)
        snap.writeAddrs.push_back({addr, size});
}

} // namespace gem5
