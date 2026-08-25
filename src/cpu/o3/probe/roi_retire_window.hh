#ifndef __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__
#define __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/static_inst_fwd.hh"
#include "params/RoiRetireWindow.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5
{

class RoiRetireWindow : public ProbeListenerObject
{
  public:
    RoiRetireWindow(const RoiRetireWindowParams &params);

    void regProbeListeners() override;
    void start();
    void stop();
    std::string eventKind() const;
    void acknowledge();
    uint64_t completeWindows() const;
    uint64_t systemInstructions() const;

  private:
    enum class State
    {
        WaitingForBegin,
        Active,
        Ended
    };

    enum class Event
    {
        None,
        Begin,
        Window,
        End
    };

    void retire(const SystemRetireRecord &record);
    void signal(Event event);

    using RetireListener =
        ProbeListenerArg<RoiRetireWindow, SystemRetireRecord>;

    const Addr beginPc;
    const Addr endPc;
    const Addr targetExecStart;
    const Addr targetExecEnd;
    const std::vector<uint64_t> windowInsts;
    BaseCPU *const cpu;
    State state = State::WaitingForBegin;
    Event pendingEvent = Event::None;
    bool skipInitialBeginPc = false;
    uint64_t targetExecUserInstructions = 0;
    uint64_t nextBoundary = 0;
    uint64_t windows = 0;
    struct WindowStats : public statistics::Group
    {
        WindowStats(statistics::Group *parent);

        statistics::Scalar instructions;
    } stats;
};

} // namespace gem5

#endif // __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__
