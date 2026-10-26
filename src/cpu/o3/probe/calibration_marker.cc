// SPDX-License-Identifier: BSD-3-Clause
// Read-only retirement observation; HANDOFF controls only the KVM prefix.
#include "cpu/o3/probe/calibration_marker.hh"
#include "cpu/o3/probe/tao_trace.hh"
#include <stdexcept>
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "params/BaseO3CPU.hh"
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
      marker_cpu_(dynamic_cast<CPU *>(params.manager)),
      branch_observation_(params.branch_observation)
{
    fatal_if(!marker_pc_ || !marker_cpu_, "Marker requires O3 manager and boundary PC");
    fatal_if(!params.marker_branch_trace_file.empty() && !branch_observation_,
             "Branch trace requires branch observation");
    marker_instances_.insert(this);
    if (!params.marker_pc_trace_file.empty())
        marker_pc_trace_ = std::make_unique<MarkerPcTrace>(params.marker_pc_trace_file);
    if (!params.marker_branch_trace_file.empty())
        marker_branch_trace_ = std::make_unique<MarkerBranchTrace>(params.marker_branch_trace_file);
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
    const auto &p = static_cast<const BaseO3CPUParams &>(marker_cpu_->params());
    marker_cpu_->taskStats.observe(marker_cpu_->tcBase(0),
        p.taskStatsSwitchPc, p.taskStatsCurrentOffset, p.taskStatsPidOffset);
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
CalibrationMarker::observeMarkerBranch(const DynInstPtr &inst)
{
    const uint64_t macro_index = marker_macro_index_++;
    if (!branch_observation_ || !inst->isCondCtrl()) return;
    // x86 Br and Eret control the microcode sequencer, rather than the
    // architectural instruction stream. Br also lacks IsLastMicroop, but
    // explicitly exclude both here to preserve that boundary for Eret.
    const auto mnemonic = inst->staticInst->getName();
    if (inst->isMicroop() && (mnemonic == "br" || mnemonic == "eret")) return;

    fatal_if(!inst->originalBranchPredPC,
             "retired conditional branch lacks its original prediction");
    const bool taken = inst->pcState().branching();
    std::unique_ptr<PCStateBase> next_pc(inst->pcState().clone());
    inst->staticInst->advancePC(*next_pc);
    const bool direction_miss = inst->branchDirectionPredictionValid &&
        inst->originalBranchDirectionPrediction != taken;
    const bool next_pc_miss = inst->originalBranchPredPC->instAddr() !=
        next_pc->instAddr();
    const uint64_t branch_index = marker_branch_index_++;
    if (marker_branch_trace_) {
        marker_branch_trace_->append(MarkerBranchObservation{
            branch_index, macro_index, uint64_t(inst->seqNum),
            uint32_t(inst->threadNumber), inst->pcState().instAddr(),
            curTick(), taken, inst->branchDirectionPredictionValid,
            inst->originalBranchDirectionPrediction, inst->readPredTaken(),
            inst->originalBranchPredPC->instAddr(), next_pc->instAddr(),
            direction_miss, next_pc_miss, inst->branchPredMispredicted(),
            inst->branchCommitSquash, inst->branchBtbHit});
    }
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
    if (owner->marker_measurement_phase_ == MarkerMeasurementPhase::Measuring) {
        owner->observeMarkerBranch(inst);
        if (owner->marker_pc_trace_) {
            const int64_t fetch_tick = inst->fetchTick == Tick(-1) ? -1 : int64_t(inst->fetchTick);
            owner->marker_pc_trace_->append(pc, curTick(), fetch_tick,
                inst->dispatchTick, inst->issueTick, inst->completeTick);
        }
    }
    if (pc != marker_pc_) return;
    owner->marker_address_space_ = inst->fetchedAddressSpaceId;
    ++owner->marker_ordinal_;
    const bool begin = owner->marker_ordinal_ == owner->marker_begin_;
    const bool end = owner->marker_ordinal_ == owner->marker_end_;
    if (!begin && !end) return;
    owner->marker_measurement_phase_ = begin ?
        MarkerMeasurementPhase::Measuring : MarkerMeasurementPhase::Complete;
    if (end && owner->marker_pc_trace_) owner->marker_pc_trace_->finalizeTrace();
    if (end && owner->marker_branch_trace_) owner->marker_branch_trace_->finalizeTrace();
    // Snapshot at retirement without draining or changing pipeline state.
    statistics::dump();
    if (begin) {
        owner->marker_macro_index_ = owner->marker_branch_index_ = 0;
        if (owner->marker_pc_trace_) owner->marker_pc_trace_->initializeTrace();
        if (owner->marker_branch_trace_) owner->marker_branch_trace_->initializeTrace();
    }
    if (end) exitSimulationLoopNow(8, {{"kind", "end"}});
}
} }
