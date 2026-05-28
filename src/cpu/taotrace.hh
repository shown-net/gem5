/*
 * TAO structured instruction tracer.
 */

#ifndef __CPU_TAOTRACE_HH__
#define __CPU_TAOTRACE_HH__

#include <memory>
#include <string>

#include "config/have_protobuf.hh"
#include "params/TAOTracer.hh"
#include "sim/insttracer.hh"

namespace gem5
{

class ExecContext;
class ThreadContext;

namespace trace {

class TAOTracer;

} // namespace trace
} // namespace gem5

#if HAVE_PROTOBUF
class ProtoOutputStream;
namespace ProtoMessage {
class TaoFunctionalBatch;
class TaoDetailedBatch;
}
#endif

namespace gem5
{

namespace trace {

class TAOTracerRecord : public InstRecord
{
  public:
    TAOTracerRecord(Tick _when, ThreadContext *_thread,
                    const StaticInstPtr _staticInst,
                    const PCStateBase &_pc,
                    TAOTracer &_tracer,
                    const StaticInstPtr _macroStaticInst = nullptr)
        : InstRecord(_when, _thread, _staticInst, _pc, _macroStaticInst),
          tracer(_tracer)
    {}

    void dump() override;

  private:
    TAOTracer &tracer;
};

class TAOTracer : public InstTracer
{
  public:
    typedef TAOTracerParams Params;
    TAOTracer(const Params &params);
    ~TAOTracer() override;

    const std::string &traceKind() const { return _traceKind; }
    bool useProtobufBatch() const { return _traceFormat == "protobuf_batch"; }

    InstRecord *
    getInstRecord(Tick when, ThreadContext *tc,
            const StaticInstPtr staticInst, const PCStateBase &pc,
            const StaticInstPtr macroStaticInst=nullptr) override;

    InstRecord *
    getInstRecord(Tick when, ThreadContext *tc, const ExecContext *xc,
            const StaticInstPtr staticInst, const PCStateBase &pc,
            const StaticInstPtr macroStaticInst = nullptr) override;

    void writeRecord(
        Tick when, ThreadContext *thread, const StaticInstPtr inst,
        const PCStateBase &pc, const std::string &opcode_text,
        uint64_t src_bitmap, uint64_t src_bitmap_high,
        uint64_t dst_bitmap, uint64_t dst_bitmap_high, Addr mem_addr,
        Addr branch_target, bool branch_taken, bool is_indirect_branch, bool squashed,
        bool branch_mispred, Tick fetch_tick, Tick issue_tick,
        Tick complete_tick, Tick commit_tick, bool is_mem,
        const std::string &data_access_level, bool icache_miss,
        bool itlb_miss, bool dtlb_miss, bool tlb_miss, bool in_user_mode,
        const TAOTracerRecord &record);

  private:
    std::string _traceKind;
    std::string _traceFormat;
    std::string _traceFile;
    uint32_t _batchSize;

#if HAVE_PROTOBUF
    std::unique_ptr<ProtoOutputStream> _protoStream;
    std::unique_ptr<ProtoMessage::TaoFunctionalBatch> _functionalBatch;
    std::unique_ptr<ProtoMessage::TaoDetailedBatch> _detailedBatch;
    uint32_t _currentBatchRows = 0;

    void ensureProtoStream();
    void flushProtoBatch();
#endif
};

} // namespace trace
} // namespace gem5

#endif // __CPU_TAOTRACE_HH__
