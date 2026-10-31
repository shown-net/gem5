// SPDX-License-Identifier: BSD-3-Clause
// Read-only retirement observation; HANDOFF controls only the KVM prefix.
#include "cpu/o3/probe/calibration_marker.hh"
#include "cpu/o3/probe/tao_trace.hh"
#include <stdexcept>
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/thread_context.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5 { namespace o3 {
CalibrationMarker *CalibrationMarker::marker_owner_ = nullptr;
uint64_t CalibrationMarker::marker_address_root_ = 0;
gem5::ThreadContext *CalibrationMarker::marker_handoff_context_ = nullptr;
std::unordered_set<CalibrationMarker *> CalibrationMarker::marker_instances_;


CalibrationMarker::CalibrationMarker(TaoTrace &owner, const TaoTraceParams &params)
    : owner_(owner), marker_pc_(params.marker_pc),
      marker_cpu_(dynamic_cast<CPU *>(params.manager))
{
    fatal_if(!marker_pc_ || !marker_cpu_, "Marker requires O3 manager and boundary PC");
    marker_cpu_->markerObservation = true;
    marker_instances_.insert(this);
}
CalibrationMarker::~CalibrationMarker()
{
    marker_instances_.erase(this);
    if (marker_owner_ == this) marker_owner_ = nullptr;
}
void
CalibrationMarker::regProbeListeners()
{
    using Listener = ProbeListenerArg<CalibrationMarker, DynInstPtr>;
    owner_.connectListener<Listener>(this, "Commit", &CalibrationMarker::observeMarkerCommit);
}
void
CalibrationMarker::startMarkerCapture(const std::string &directory)
{
    fatal_if(marker_capture_started_, "Marker capture already started");
    marker_capture_started_ = true;

}
void
CalibrationMarker::traceMarkerHandoff(gem5::ThreadContext *tc)
{
    if (marker_instances_.empty()) {
        return;
    }
    // Capture the target identity before leaving KVM.
    traceSourceRoiCheckpoint(tc);
    marker_address_root_ = tc->getSystemPtr()->getSourceRoiAddressSpaceId();
    if (simQuantum && !marker_owner_) {
        // Parallel KVM hypercalls exit at the next safe global boundary.
        // Hold the triggering context so adjacent START/END instructions
        // cannot execute in KVM before handoff or fork.
        fatal_if(marker_handoff_context_, "overlapping Marker handoff hooks");
        marker_handoff_context_ = tc;
        // Halt is explicitly resumed by the handler; unlike suspend it
        // cannot be undone by an interrupt during the KVM quantum.
        tc->halt();
    }
}
void
CalibrationMarker::resumeMarkerHandoff()
{
    if (!marker_handoff_context_) {
        return;
    }
    auto *tc = marker_handoff_context_;
    marker_handoff_context_ = nullptr;
    // CPU takeover replaces the System's context for the same context ID.
    tc->getSystemPtr()->threads[tc->contextId()]->activate();
}
void
CalibrationMarker::selectMarkerWindow(uint64_t begin, uint64_t end)
{
    fatal_if(marker_owner_ || !begin || end <= begin,
             "invalid CalibrationMarker Marker window selection");
    marker_owner_ = this;
    fatal_if(!marker_address_root_,
             "CalibrationMarker warmup lacks target address space");
    marker_address_space_ = marker_address_root_;
    marker_begin_ = begin;
    marker_end_ = end;
}
void
CalibrationMarker::traceSourceRoiCheckpoint(gem5::ThreadContext *tc)
{
    if (!tc || !tc->getSystemPtr()) return;
    X86ISA::Efer efer =
        tc->readMiscRegNoEffect(X86ISA::misc_reg::Efer);
    if (!efer.lma) {
        warn("CalibrationMarker: source ROI checkpoint ignored outside x86-64 "
             "long mode");
        return;
    }
    X86ISA::CR3 cr3 =
        tc->readMiscRegNoEffect(X86ISA::misc_reg::Cr3);
    const uint64_t root = uint64_t(cr3.longPdtb) << 12;
    if (root == 0) {
        throw std::runtime_error(
            "CalibrationMarker source ROI checkpoint captured a zero guest CR3");
    }
    tc->getSystemPtr()->setSourceRoiAddressSpaceId(root);
    inform("CalibrationMarker: source ROI checkpoint captured guest PTE root=%#lx",
           (unsigned long)root);
}
void
CalibrationMarker::observeMarkerCommit(const DynInstPtr &inst)
{
    auto *owner = marker_owner_;
    if (!owner || !marker_capture_started_ ||
        owner->marker_measurement_phase_ == MarkerMeasurementPhase::Complete ||
        (inst->isMicroop() && !inst->isLastMicroop()) ||
        !inst->fetchedFromUser || !inst->fetchedAddressSpaceId ||
        (owner->marker_address_space_ &&
         owner->marker_address_space_ != inst->fetchedAddressSpaceId))
        return;
    const Addr pc = inst->pcState().instAddr();
    if (pc != marker_pc_) return;
    owner->marker_address_space_ = inst->fetchedAddressSpaceId;
    ++owner->marker_ordinal_;
    const bool begin = owner->marker_ordinal_ == owner->marker_begin_;
    const bool end = owner->marker_ordinal_ == owner->marker_end_;
    if (!begin && !end) return;
    owner->marker_measurement_phase_ = begin ?
        MarkerMeasurementPhase::Measuring : MarkerMeasurementPhase::Complete;
    // Snapshot at retirement without draining or changing pipeline state.
    statistics::dump();
    if (end) exitSimulationLoopNow(8, {{"kind", "end"}});
}
} }
