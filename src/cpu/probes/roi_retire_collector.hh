#ifndef __CPU_PROBES_ROI_RETIRE_COLLECTOR_HH__
#define __CPU_PROBES_ROI_RETIRE_COLLECTOR_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "cpu/base.hh"
#include "params/RoiRetireCollector.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5
{

class RoiRetireCollector : public ProbeListenerObject
{
  public:
    RoiRetireCollector(const RoiRetireCollectorParams &params);
    ~RoiRetireCollector() override;

    void regProbeListeners() override;
    void acknowledge();
    uint64_t retiredInstructions() const;
    uint64_t workloadUserInstructions() const;

  private:
    enum class State { WaitingForBegin, Collecting, Ended };
    enum class Event { None, Begin, Window, End };

    void retire(const ArchitecturalRetireRecord &record);
    void signal(Event event);
    void flushTraceChunk();
    void finalizeTrace();

    using RetireListener = ProbeListenerArg<
        RoiRetireCollector, ArchitecturalRetireRecord>;

    const Addr beginPc;
    const Addr endPc;
    const Addr traceImageBase;
    const std::vector<Addr> traceExecSegments;
    const uint64_t windowInsts;
    const std::vector<uint64_t> windowSchedule;
    BaseCPU *const cpu;
    State state = State::WaitingForBegin;
    Event pendingEvent = Event::None;
    uint64_t retired = 0;
    uint64_t ownerAddressSpaceId = 0;
    uint64_t workloadUserInstructions_ = 0;
    uint64_t nextBoundary = 0;
    size_t windowIndex = 0;

    std::ofstream *traceOutput = nullptr;
    const unsigned traceChunkRecords;
    const int traceZstdLevel;
    bool traceFinalized = false;
    std::vector<Addr> tracePcs;

    struct WindowStats : public statistics::Group
    {
        WindowStats(statistics::Group *parent);
        statistics::Scalar instructions;
        statistics::Scalar workloadUserInstructions;
    } stats;
};

} // namespace gem5

#endif // __CPU_PROBES_ROI_RETIRE_COLLECTOR_HH__
