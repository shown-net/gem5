#ifndef __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__
#define __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
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
    std::string eventKind() const;
    void acknowledge();
    uint64_t totalInstructions() const;
    uint64_t completeWindows() const;

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

    void retire(const std::pair<StaticInstPtr, Addr> &inst);
    void signal(Event event);

    using RetireListener =
        ProbeListenerArg<RoiRetireWindow, std::pair<StaticInstPtr, Addr>>;

    const Addr beginPc;
    const Addr endPc;
    const std::vector<uint64_t> windowInsts;
    State state = State::WaitingForBegin;
    Event pendingEvent = Event::None;
    bool skipInitialBeginPc = false;
    uint64_t roiInstructions = 0;
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
