#ifndef __CPU_PROBES_ROI_PC_TRACE_HH__
#define __CPU_PROBES_ROI_PC_TRACE_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "cpu/base.hh"
#include "params/RoiPcTrace.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5
{

class RoiPcTrace : public ProbeListenerObject
{
  public:
    RoiPcTrace(const RoiPcTraceParams &params);
    ~RoiPcTrace() override;

    void regProbeListeners() override;
    void startTracing();
    void stopTracing();
    uint64_t recordCount() const;

  private:
    void retire(const ArchitecturalRetireRecord &record);
    void flushChunk();
    void finalize();

    using RetireListener = ProbeListenerArg<
        RoiPcTrace, ArchitecturalRetireRecord>;

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

#endif // __CPU_PROBES_ROI_PC_TRACE_HH__
