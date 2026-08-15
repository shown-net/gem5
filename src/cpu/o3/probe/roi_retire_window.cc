#include "cpu/o3/probe/roi_retire_window.hh"

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

RoiRetireWindow::WindowStats::WindowStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(instructions, statistics::units::Count::get(),
               "User architectural instructions retired in the ROI window")
{
}

RoiRetireWindow::RoiRetireWindow(const RoiRetireWindowParams &params)
    : ProbeListenerObject(params),
      beginPc(params.begin_pc),
      endPc(params.end_pc),
      intervalInsts(params.interval_insts),
      stats(this)
{
    fatal_if(!intervalInsts,
             "ROI retired-instruction interval must be positive");
    fatal_if(beginPc == endPc, "ROI begin and end marker PCs must differ");
}

void
RoiRetireWindow::regProbeListeners()
{
    connectListener<RetireListener>(
        this, "ArchitecturalRetire", &RoiRetireWindow::retire);
}

void
RoiRetireWindow::retire(const o3::DynInstPtr &inst)
{
    const Addr pc = inst->pcState().instAddr();
    if (state == State::WaitingForBegin) {
        if (pc == beginPc && pendingEvent == Event::None) {
            signal(Event::Begin);
        }
        return;
    }
    if (state != State::Active)
        return;

    ++roiInstructions;
    stats.instructions++;
    if (pc == endPc) {
        signal(Event::End);
        return;
    }
    if (roiInstructions >= nextBoundary) {
        ++windows;
        nextBoundary += intervalInsts;
        // The exit event is handled after the current O3 tick. Any remaining
        // same-cycle commits stay in this raw window and are reflected by the
        // listener's instruction stat.
        signal(Event::Window);
    }
}

void
RoiRetireWindow::signal(Event event)
{
    if (pendingEvent == Event::End)
        return;
    if (pendingEvent == Event::Window && event == Event::End) {
        pendingEvent = event;
        return;
    }
    if (pendingEvent != Event::None)
        return;
    pendingEvent = event;
    exitSimLoopNow("roi retired-instruction event");
}

std::string
RoiRetireWindow::eventKind() const
{
    switch (pendingEvent) {
      case Event::Begin:
        return "begin";
      case Event::Window:
        return "window";
      case Event::End:
        return "end";
      case Event::None:
        return "none";
    }
    return "none";
}

void
RoiRetireWindow::acknowledge()
{
    fatal_if(pendingEvent == Event::None,
             "ROI retire listener has no pending event");
    if (pendingEvent == Event::Begin) {
        state = State::Active;
        nextBoundary = intervalInsts;
    } else if (pendingEvent == Event::End) {
        state = State::Ended;
    }
    pendingEvent = Event::None;
}

uint64_t
RoiRetireWindow::totalInstructions() const
{
    return roiInstructions;
}

uint64_t
RoiRetireWindow::completeWindows() const
{
    return windows;
}

} // namespace gem5
