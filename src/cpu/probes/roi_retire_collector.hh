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

  private:
    enum class State { WaitingForBodyStart, Collecting, Ended };
    enum class Event { None, Begin, Window, End };

    void retire(const ArchitecturalRetireRecord &record);
    void signal(Event event);
    void flushTraceChunk();
    void finalizeTrace();

    using RetireListener = ProbeListenerArg<
        RoiRetireCollector, ArchitecturalRetireRecord>;

    const Addr bodyStartPc;
    const Addr endPc;
    const Addr targetExecStart;
    const Addr targetExecEnd;
    const std::vector<uint64_t> windowInsts;
    BaseCPU *const cpu;
    State state = State::WaitingForBodyStart;
    Event pendingEvent = Event::None;
    uint64_t retired = 0;
    uint64_t targetExecUserInstructions = 0;
    uint64_t nextBoundary = 0;
    uint64_t windows = 0;

    std::ofstream *traceOutput = nullptr;
    const unsigned traceChunkRecords;
    const int traceZstdLevel;
    bool traceFinalized = false;
    std::vector<Addr> tracePcs;

    struct WindowStats : public statistics::Group
    {
        WindowStats(statistics::Group *parent);
        statistics::Scalar instructions;
    } stats;
};

} // namespace gem5

#endif // __CPU_PROBES_ROI_RETIRE_COLLECTOR_HH__
