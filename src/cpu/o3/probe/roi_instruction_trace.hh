#ifndef __CPU_O3_PROBE_ROI_INSTRUCTION_TRACE_HH__
#define __CPU_O3_PROBE_ROI_INSTRUCTION_TRACE_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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
    void retire(const std::pair<StaticInstPtr, Addr> &inst);
    void finalize();

    using RetireListener = ProbeListenerArg<
        RoiInstructionTrace, std::pair<StaticInstPtr, Addr>>;

    void flushChunk();

    std::ofstream *output;
    const unsigned chunkRecords;
    const int zstdLevel;
    const Addr loadBias;
    const Addr endPc;
    bool tracing = false;
    bool finalized = false;
    uint64_t records = 0;
    uint64_t chunks = 0;
    std::vector<Addr> pcs;
    std::vector<uint64_t> flags;
    std::vector<uint32_t> sizes;
    std::vector<std::string> opcodes;
    std::vector<std::string> disassemblies;
    std::unordered_set<Addr> describedPcs;
};

} // namespace gem5

#endif // __CPU_O3_PROBE_ROI_INSTRUCTION_TRACE_HH__
