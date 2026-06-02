/*
 * TAO structured instruction tracer.
 */

#include "cpu/taotrace.hh"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "arch/generic/mmu.hh"
#include "base/callback.hh"
#include "base/loader/symtab.hh"
#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/base.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/TAOTrace.hh"
#include "enums/OpClass.hh"

#if HAVE_PROTOBUF
#include "proto/protoio.hh"
#include "proto/tao_trace.pb.h"
#endif

namespace gem5
{

namespace trace {

namespace
{

std::string
jsonEscape(const std::string &value)
{
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
          case '\\':
            out << "\\\\";
            break;
          case '"':
            out << "\\\"";
            break;
          case '\n':
            out << "\\n";
            break;
          case '\r':
            out << "\\r";
            break;
          case '\t':
            out << "\\t";
            break;
          default:
            out << ch;
            break;
        }
    }
    return out.str();
}

std::pair<uint64_t, uint64_t>
regBitmap128(const StaticInstPtr &inst, bool dest)
{
    uint64_t bitmap_low = 0;
    uint64_t bitmap_high = 0;
    const int count = dest ? inst->numDestRegs() : inst->numSrcRegs();
    for (int i = 0; i < count; ++i) {
        const RegId &reg = dest ? inst->destRegIdx(i) : inst->srcRegIdx(i);
        if (reg.classValue() == InvalidRegClass)
            continue;
        unsigned shift = 0xFFFF;
        const unsigned idx = static_cast<unsigned>(reg.index());
        switch (reg.classValue()) {
        case IntRegClass:
            if (idx < 16) shift = idx;
            break;
        case FloatRegClass:
            if (idx < 8) shift = 16 + idx;
            else if (idx >= 8 && idx < 40) shift = 24 + (idx - 8);
            break;
        case CCRegClass:
            if (idx < 6) shift = 56 + idx;
            break;
        default:
            break;
        }
        if (shift == 0xFFFF) continue;
        if (shift < 64U) {
            bitmap_low |= (1ULL << shift);
        } else if (shift < 128U) {
            bitmap_high |= (1ULL << (shift - 64U));
        }
    }
    return {bitmap_low, bitmap_high};
}

Addr
directBranchTarget(const StaticInstPtr &inst, const PCStateBase &pc)
{
    if (!inst->isDirectCtrl())
        return 0;
    std::unique_ptr<PCStateBase> target = inst->branchTarget(pc);
    return target ? target->instAddr() : 0;
}

Addr
actualControlTarget(const StaticInstPtr &inst, const PCStateBase &pc)
{
    if (!inst->isControl())
        return 0;
    std::unique_ptr<PCStateBase> target(pc.clone());
    inst->advancePC(*target);
    return target ? target->instAddr() : 0;
}

bool
controlTaken(const StaticInstPtr &inst, const PCStateBase &pc, Addr target)
{
    if (!inst->isControl())
        return false;
    if (!inst->isCondCtrl())
        return true;
    const Addr direct_target = directBranchTarget(inst, pc);
    return direct_target != 0 && target == direct_target;
}

uint32_t
taoDataAccessLevelToUint8(const std::string &level)
{
    if (level == "L1") return 1;
    if (level == "L2") return 2;
    if (level == "MEM") return 3;
    return 0;
}

} // anonymous namespace

TAOTracer::TAOTracer(const Params &params)
    : InstTracer(params),
      _traceKind(params.trace_kind),
      _traceFormat(params.trace_format),
      _traceFile(params.trace_file),
      _batchSize(params.batch_size)
{
    if (_batchSize == 0)
        _batchSize = 2048;
    if (_traceFormat != "jsonl" && _traceFormat != "protobuf_batch") {
        panic("unsupported TAO trace format: %s\n", _traceFormat.c_str());
    }
}

TAOTracer::~TAOTracer()
{
#if HAVE_PROTOBUF
    flushProtoBatch();
    _protoStream.reset();
#endif
}

#if HAVE_PROTOBUF
void
TAOTracer::ensureProtoStream()
{
    if (_protoStream)
        return;

    _protoStream.reset(new ProtoOutputStream(simout.resolve(_traceFile)));
    ProtoMessage::TaoTraceHeader header;
    header.set_version(1);
    header.set_trace_kind(_traceKind);
    header.set_schema_name(_traceKind == "detailed" ?
                           "tao_detailed_minimal_v3" :
                           "tao_functional_minimal_v3");
    header.set_batch_size(_batchSize);
    _protoStream->write(header);
    _protoStream->flush();
    registerExitCallback([this]() {
        flushProtoBatch();
        _functionalBatch.reset();
        _detailedBatch.reset();
        _protoStream.reset();
    });
}

void
TAOTracer::flushProtoBatch()
{
    if (!_protoStream || _currentBatchRows == 0)
        return;

    if (_traceKind == "detailed") {
        _detailedBatch->set_row_count(_currentBatchRows);
        _protoStream->write(*_detailedBatch);
        _detailedBatch.reset(new ProtoMessage::TaoDetailedBatch);
    } else {
        _functionalBatch->set_row_count(_currentBatchRows);
        _protoStream->write(*_functionalBatch);
        _functionalBatch.reset(new ProtoMessage::TaoFunctionalBatch);
    }
    _protoStream->flush();
    _currentBatchRows = 0;
}
#endif

void
TAOTracer::writeRecord(
    Tick when, ThreadContext *thread, const StaticInstPtr inst,
    const PCStateBase &pc, const std::string &opcode_text,
    uint64_t src_bitmap, uint64_t src_bitmap_high,
    uint64_t dst_bitmap, uint64_t dst_bitmap_high, Addr mem_addr,
    Addr branch_target, bool branch_taken, bool is_indirect_branch, bool squashed,
    bool branch_mispred, Tick fetch_tick, Tick issue_tick,
    Tick complete_tick, Tick commit_tick, bool is_mem,
    const std::string &data_access_level, bool icache_miss, bool itlb_miss,
    bool dtlb_miss, bool tlb_miss, bool in_user_mode,
    const TAOTracerRecord &record)
{
    const bool detailed = _traceKind == "detailed";

    if (useProtobufBatch()) {
#if HAVE_PROTOBUF
        ensureProtoStream();
        if (detailed) {
            if (!_detailedBatch)
                _detailedBatch.reset(new ProtoMessage::TaoDetailedBatch);
            auto *batch = _detailedBatch.get();
            batch->add_pc(thread->getMMUPtr()->getValidAddr(
                pc.instAddr(), thread, BaseMMU::Execute));
            batch->add_micro_pc(pc.microPC());
            batch->add_seq(record.getFetchSeqValid() ? record.getFetchSeq() : 0);
            batch->add_commit_seq(record.getCpSeqValid() ? record.getCpSeq() : 0);
            batch->add_opcode_text(opcode_text);
            batch->add_src_reg_bitmap(src_bitmap);
            batch->add_dst_reg_bitmap(dst_bitmap);
            batch->add_src_reg_bitmap_high(src_bitmap_high);
            batch->add_dst_reg_bitmap_high(dst_bitmap_high);
            batch->add_is_branch(inst->isControl());
            batch->add_is_cond_branch(inst->isCondCtrl());
            batch->add_is_load(inst->isLoad());
            batch->add_is_store(inst->isStore());
            batch->add_mem_addr(mem_addr);
            batch->add_branch_target(branch_target);
            batch->add_branch_taken(branch_taken);
            batch->add_is_indirect_branch(is_indirect_branch);
            batch->add_fetch_tick(fetch_tick);
            batch->add_issue_tick(issue_tick);
            batch->add_complete_tick(complete_tick);
            batch->add_commit_tick(commit_tick);
            batch->add_is_squashed(squashed);
            batch->add_is_nop(inst->isNop());
            batch->add_branch_mispred_exact(record.getTAOBranchMispredValid() ?
                                            branch_mispred : false);
            batch->add_data_access_level_exact(record.getTAODataAccessLevelValid() ?
                                               taoDataAccessLevelToUint8(data_access_level) : 0);
            batch->add_icache_miss_exact(record.getTAOICacheMissValid() ?
                                         icache_miss : false);
            batch->add_tlb_miss_exact(record.getTAOTLBMissValid() ?
                                      tlb_miss : false);
        } else {
            if (!_functionalBatch)
                _functionalBatch.reset(new ProtoMessage::TaoFunctionalBatch);
            auto *batch = _functionalBatch.get();
            batch->add_pc(thread->getMMUPtr()->getValidAddr(
                pc.instAddr(), thread, BaseMMU::Execute));
            batch->add_micro_pc(pc.microPC());
            batch->add_opcode_text(opcode_text);
            batch->add_src_reg_bitmap(src_bitmap);
            batch->add_dst_reg_bitmap(dst_bitmap);
            batch->add_src_reg_bitmap_high(src_bitmap_high);
            batch->add_dst_reg_bitmap_high(dst_bitmap_high);
            batch->add_is_branch(inst->isControl());
            batch->add_is_cond_branch(inst->isCondCtrl());
            batch->add_is_load(inst->isLoad());
            batch->add_is_store(inst->isStore());
            batch->add_mem_addr(mem_addr);
            batch->add_branch_target(branch_target);
            batch->add_branch_taken(branch_taken);
            batch->add_is_indirect_branch(is_indirect_branch);
        }
        ++_currentBatchRows;
        if (_currentBatchRows >= _batchSize)
            flushProtoBatch();
        return;
#else
        panic("TAO protobuf_batch trace requested without protobuf support\n");
#endif
    }

    Addr cur_pc = thread->getMMUPtr()->getValidAddr(
        pc.instAddr(), thread, BaseMMU::Execute);
    std::ostringstream out;
    out << "{";
    out << "\"source\":\"real_gem5\"";
    out << ",\"trace_kind\":\"" << jsonEscape(_traceKind) << "\"";
    out << ",\"pc\":" << cur_pc;
    out << ",\"micro_pc\":" << pc.microPC();
    out << ",\"seq\":" << (record.getFetchSeqValid() ? record.getFetchSeq() : 0);
    out << ",\"commit_seq\":" << (record.getCpSeqValid() ? record.getCpSeq() : 0);
    out << ",\"opcode_text\":\"" << jsonEscape(opcode_text) << "\"";
    out << ",\"src_reg_bitmap\":" << src_bitmap;
    out << ",\"dst_reg_bitmap\":" << dst_bitmap;
    out << ",\"src_reg_bitmap_high\":" << src_bitmap_high;
    out << ",\"dst_reg_bitmap_high\":" << dst_bitmap_high;
    out << ",\"is_branch\":" << (inst->isControl() ? "true" : "false");
    out << ",\"is_cond_branch\":" << (inst->isCondCtrl() ? "true" : "false");
    out << ",\"is_load\":" << (inst->isLoad() ? "true" : "false");
    out << ",\"is_store\":" << (inst->isStore() ? "true" : "false");
    out << ",\"mem_addr\":" << mem_addr;
    out << ",\"branch_target\":" << branch_target;
    out << ",\"branch_taken\":" << (branch_taken ? "true" : "false");
    out << ",\"is_indirect_branch\":" << (is_indirect_branch ? "true" : "false");
    if (detailed) {
        out << ",\"tick\":" << when;
        out << ",\"disasm\":\"" << jsonEscape(opcode_text) << "\"";
        out << ",\"op_class\":\"" << enums::OpClassStrings[inst->opClass()] << "\"";
        out << ",\"mem_size\":" << (record.getMemValid() ? record.getSize() : 0);
        out << ",\"mem_flags\":" << (record.getMemValid() ? record.getFlags() : 0);
        out << ",\"fetch_tick\":" << fetch_tick;
        out << ",\"decode_tick\":" << (record.getTAOStageTicksValid() ?
            record.getTAODecodeTick() : 0);
        out << ",\"rename_tick\":" << (record.getTAOStageTicksValid() ?
            record.getTAORenameTick() : 0);
        out << ",\"dispatch_tick\":" << (record.getTAOStageTicksValid() ?
            record.getTAODispatchTick() : 0);
        out << ",\"issue_tick\":" << issue_tick;
        out << ",\"complete_tick\":" << complete_tick;
        out << ",\"commit_tick\":" << commit_tick;
        out << ",\"store_tick\":" << (record.getTAOStageTicksValid() ?
            record.getTAOStoreTick() : 0);
        out << ",\"is_squashed\":" << (squashed ? "true" : "false");
        out << ",\"is_nop\":" << (inst->isNop() ? "true" : "false");
        out << ",\"branch_mispred\":" << (branch_mispred ? "true" : "false");
        out << ",\"data_access_level\":\"" << jsonEscape(data_access_level) << "\"";
        out << ",\"icache_miss\":" << (icache_miss ? "true" : "false");
        out << ",\"tlb_miss\":" << (tlb_miss ? "true" : "false");
        if (record.getTAOBranchMispredValid()) {
            out << ",\"branch_mispred_exact\":" << (branch_mispred ? "true" : "false");
        }
        if (record.getTAODataAccessLevelValid()) {
            out << ",\"data_access_level_exact\":\"" << jsonEscape(data_access_level) << "\"";
        }
        if (record.getTAOICacheMissValid()) {
            out << ",\"icache_miss_exact\":" << (icache_miss ? "true" : "false");
        }
        if (record.getTAOTLBMissValid()) {
            out << ",\"tlb_miss_exact\":" << (tlb_miss ? "true" : "false");
        }
        if (record.getTAOITLBMissValid()) {
            out << ",\"itlb_miss_exact\":" << (itlb_miss ? "true" : "false");
        }
        if (record.getTAODTLBMissValid()) {
            out << ",\"dtlb_miss_exact\":" << (dtlb_miss ? "true" : "false");
        }
        out << ",\"predicate\":" << (record.getPredicate() ? "true" : "false");
        out << ",\"faulting\":" << (record.getFaulting() ? "true" : "false");
        out << ",\"in_user_mode\":" << (in_user_mode ? "true" : "false");
    }
    out << "}";

    trace::getDebugLogger()->dprintf_flag(
        when, thread->getCpuPtr()->name(), "TAOTrace", "%s\n",
        out.str().c_str());
}

InstRecord *
TAOTracer::getInstRecord(Tick when, ThreadContext *tc,
        const StaticInstPtr staticInst, const PCStateBase &pc,
        const StaticInstPtr macroStaticInst)
{
    return getInstRecord(when, tc, nullptr, staticInst, pc, macroStaticInst);
}

InstRecord *
TAOTracer::getInstRecord(Tick when, ThreadContext *tc, const ExecContext *xc,
        const StaticInstPtr staticInst, const PCStateBase &pc,
        const StaticInstPtr macroStaticInst)
{
    [[maybe_unused]] const ExecContext *exec_context = xc;
    if (!useProtobufBatch() && !debug::TAOTrace)
        return nullptr;

    return new TAOTracerRecord(when, tc, staticInst, pc, *this,
                               macroStaticInst);
}

void
TAOTracerRecord::dump()
{
    if (getTAODumped())
        return;

    const StaticInstPtr inst = staticInst;
    const bool in_user_mode = thread->getIsaPtr()->inUserMode();
    Addr cur_pc = thread->getMMUPtr()->getValidAddr(
        pc->instAddr(), thread, BaseMMU::Execute);
    const std::string opcode_text = inst->disassemble(cur_pc);
    const bool functional = tracer.traceKind() == "functional";
    const auto src_fallback = regBitmap128(inst, false);
    const auto dst_fallback = regBitmap128(inst, true);
    const uint64_t src_bitmap = getTAORegBitmapValid() ?
        getTAOSrcRegBitmap() : src_fallback.first;
    const uint64_t src_bitmap_high = getTAORegBitmapValid() ?
        getTAOSrcRegBitmapHigh() : src_fallback.second;
    const uint64_t dst_bitmap = getTAORegBitmapValid() ?
        getTAODstRegBitmap() : dst_fallback.first;
    const uint64_t dst_bitmap_high = getTAORegBitmapValid() ?
        getTAODstRegBitmapHigh() : dst_fallback.second;
    const Addr mem_addr = getMemValid() ? getAddr() : 0;
    const Addr branch_target = getTAOBranchTargetValid() ?
        getTAOBranchTarget() : actualControlTarget(inst, *pc);
    const bool branch_taken = controlTaken(inst, *pc, branch_target);
    const bool is_indirect_branch = inst->isIndirectCtrl();
    const bool squashed = getTAOSquashedValid() ? getTAOSquashed() : false;
    if (functional && squashed) {
        setTAODumped();
        return;
    }
    const bool branch_mispred = getTAOBranchMispredValid() ?
        getTAOBranchMispred() : false;
    const Tick fetch_tick = getTAOStageTicksValid() ? getTAOFetchTick() : when;
    const Tick issue_tick = getTAOStageTicksValid() ? getTAOIssueTick() : when;
    const Tick complete_tick = getTAOStageTicksValid() ? getTAOCompleteTick() : when;
    const Tick commit_tick = getTAOStageTicksValid() ? getTAOCommitTick() : when;
    const bool is_mem = inst->isLoad() || inst->isStore();
    const std::string data_access_level = getTAODataAccessLevelValid() ?
        getTAODataAccessLevel() : (is_mem ? "UNKNOWN" : "NONE");
    const bool icache_miss = getTAOICacheMissValid() ? getTAOICacheMiss() : false;
    const bool itlb_miss = getTAOITLBMissValid() ? getTAOITLBMiss() : false;
    const bool dtlb_miss = getTAODTLBMissValid() ? getTAODTLBMiss() : false;
    const bool tlb_miss = getTAOTLBMissValid() ? getTAOTLBMiss() :
        (itlb_miss || dtlb_miss);

    tracer.writeRecord(
        when, thread, inst, *pc, opcode_text, src_bitmap, src_bitmap_high,
        dst_bitmap, dst_bitmap_high, mem_addr, branch_target, branch_taken,
        is_indirect_branch, squashed, branch_mispred, fetch_tick, issue_tick,
        complete_tick, commit_tick, is_mem, data_access_level, icache_miss,
        itlb_miss, dtlb_miss, tlb_miss, in_user_mode, *this);
    setTAODumped();
}

} // namespace trace
} // namespace gem5
