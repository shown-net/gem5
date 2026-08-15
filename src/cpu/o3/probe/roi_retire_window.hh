#ifndef __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__
#define __CPU_O3_PROBE_ROI_RETIRE_WINDOW_HH__

#include <cstdint>
#include <string>

#include "base/statistics.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
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

    void retire(const o3::DynInstPtr &inst);
    void signal(Event event);

    using RetireListener =
        ProbeListenerArg<RoiRetireWindow, o3::DynInstPtr>;

    const Addr beginPc;
    const Addr endPc;
    const uint64_t intervalInsts;
    State state = State::WaitingForBegin;
    Event pendingEvent = Event::None;
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
