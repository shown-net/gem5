#ifndef __CPU_O3_PROBE_ROI_INSTRUCTION_TRACE_HH__
#define __CPU_O3_PROBE_ROI_INSTRUCTION_TRACE_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "cpu/base.hh"
#include "cpu/static_inst_fwd.hh"
#include "params/RoiInstructionTrace.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5
{

class RoiInstructionTrace : public ProbeListenerObject
{
  public:
    RoiInstructionTrace(const RoiInstructionTraceParams &params);
    ~RoiInstructionTrace() override;

    void regProbeListeners() override;
    void startTracing();
    void stopTracing();
    uint64_t recordCount() const;

  private:
    void retire(const SystemRetireRecord &record);
    void finalize();

    using RetireListener = ProbeListenerArg<
        RoiInstructionTrace, SystemRetireRecord>;

    void flushChunk();

    std::ofstream *output;
    const unsigned chunkRecords;
    const int zstdLevel;
    const Addr targetExecStart;
    const Addr targetExecEnd;
    const Addr startPc;
    const Addr endPc;
    bool tracing = false;
    bool bodyActive = false;
    bool finalized = false;
    uint64_t records = 0;
    std::vector<Addr> pcs;
};

} // namespace gem5

#endif // __CPU_O3_PROBE_ROI_INSTRUCTION_TRACE_HH__
