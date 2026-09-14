#ifndef __CPU_PROBES_MARKER_RETIRE_COLLECTOR_HH__
#define __CPU_PROBES_MARKER_RETIRE_COLLECTOR_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "cpu/base.hh"
#include "params/MarkerRetireCollector.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5
{

class MarkerRetireCollector : public ProbeListenerObject
{
  public:
    MarkerRetireCollector(const MarkerRetireCollectorParams &params);
    ~MarkerRetireCollector() override;

    void regProbeListeners() override;
    void acknowledge();

  private:
    enum class State
    {
        WaitingForBegin,
        Collecting,
        Ended
    };
    enum class Event
    {
        None,
        Begin,
        End
    };

    void retire(const ArchitecturalRetireRecord &record);
    void signal(Event event);
    void initializeTrace();
    void flushTraceChunk();
    void finalizeTrace();

    using RetireListener =
        ProbeListenerArg<MarkerRetireCollector, ArchitecturalRetireRecord>;

    const Addr markerPc;
    const Addr traceImageBase;
    const std::vector<Addr> traceExecSegments;
    BaseCPU *const cpu;
    State state = State::WaitingForBegin;
    Event pendingEvent = Event::None;
    uint64_t ownerAddressSpaceId = 0;

    const std::string traceOutputFile;
    const std::string traceElfSha256;
    std::ofstream *traceOutput = nullptr;
    const unsigned traceChunkRecords;
    const int traceZstdLevel;
    bool traceFinalized = false;
    std::vector<Addr> tracePcs;
};

} // namespace gem5

#endif // __CPU_PROBES_MARKER_RETIRE_COLLECTOR_HH__
