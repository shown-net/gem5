// SPDX-License-Identifier: BSD-3-Clause
// Read-only retirement observation; HANDOFF controls only the KVM prefix.
#include "cpu/o3/probe/calibration_marker.hh"
#include "cpu/o3/probe/tao_trace.hh"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/thread_context.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"
#include "sim/sim_object.hh"

namespace gem5 { namespace o3 {
uint64_t CalibrationMarker::marker_address_root_ = 0;
gem5::ThreadContext *CalibrationMarker::marker_handoff_context_ = nullptr;
std::unordered_set<CalibrationMarker *> CalibrationMarker::marker_instances_;


CalibrationMarker::CalibrationMarker(TaoTrace &owner, const TaoTraceParams &params)
    : owner_(owner), marker_pc_(params.marker_pc),
      marker_cpu_(dynamic_cast<CPU *>(params.manager)),
      worker_(params.marker_worker), begin_pcs_(params.marker_begin_pcs),
      end_pcs_(params.marker_end_pcs), workids_(params.marker_workids),
      stat_objects_(params.marker_stat_objects)
{
    fatal_if(!marker_pc_ || !marker_cpu_, "Marker requires O3 manager and boundary PC");
    fatal_if(marker_cpu_->numThreads != 1, "Marker core windows require one hardware thread");
    if (!begin_pcs_.empty()) {
        fatal_if(end_pcs_.size() != begin_pcs_.size() ||
                 workids_.size() != begin_pcs_.size() || worker_ >= begin_pcs_.size(),
                 "Marker fixed-work participant mismatch");
        std::set<Addr> pcs;
        for (const auto pc : begin_pcs_)
            fatal_if(!pc || !pcs.insert(pc).second, "Marker BEGIN PCs must be distinct");
        for (const auto pc : end_pcs_)
            fatal_if(!pc || !pcs.insert(pc).second, "Marker END PCs must be distinct");
    } else {
        fatal_if(!end_pcs_.empty() || !workids_.empty(), "Marker lacks fixed-work BEGIN PCs");
    }
    marker_cpu_->markerObservation = true;
    marker_instances_.insert(this);
    marker_pc_trace_file_ = params.marker_pc_trace_file;
}
CalibrationMarker::~CalibrationMarker()
{
    marker_instances_.erase(this);
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
    fatal_if(marker_capture_started_ || !selected_, "Marker capture requires a selected window");
    marker_capture_started_ = true;
    const std::string suffix = "-core" + std::to_string(marker_cpu_->cpuId());
    snapshots_.open(directory + "/marker-snapshots" + suffix + ".jsonl");
    fatal_if(!snapshots_, "Cannot open per-core Marker snapshots");
    if (!marker_pc_trace_file_.empty()) {
        const std::string prefix = marker_instances_.size() == 1 ? "" :
            "worker" + std::to_string(worker_) + "-";
        marker_pc_trace_ = std::make_unique<MarkerPcTrace>(
            directory + "/" + prefix + marker_pc_trace_file_);
    }

}
void
CalibrationMarker::traceMarkerHandoff(gem5::ThreadContext *tc)
{
    if (marker_instances_.empty()) {
        return;
    }
    // Capture the target identity before leaving KVM.
    traceSourceRoiCheckpoint(tc);
    inform("CalibrationMarker: HANDOFF context=%u SPEC_CTRL=%#lx",
           tc->contextId(), (unsigned long)tc->readMiscRegNoEffect(
               X86ISA::misc_reg::SpecCtrl));
    marker_address_root_ = tc->getSystemPtr()->getSourceRoiAddressSpaceId();
    if (simQuantum) {
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
    fatal_if(selected_ || !begin || end <= begin,
             "invalid CalibrationMarker Marker window selection");
    selected_ = true;
    fatal_if(!marker_address_root_,
             "CalibrationMarker warmup lacks target address space");
    marker_address_space_ = begin_pcs_.empty() ? marker_address_root_ : 0;
    marker_begin_ = begin;
    marker_end_ = end;
}
void
CalibrationMarker::snapshot(const char *kind, Addr pc, uint64_t completed)
{
    // No global dump, reset or drain: settle only this core's owned objects.
    marker_cpu_->preDumpStats();
    for (auto *object : stat_objects_)
        if (object != marker_cpu_) object->preDumpStats();
    std::map<std::string, double> values;
    std::function<void(const statistics::Group *, const std::string &)> collect =
        [&](const statistics::Group *group, const std::string &prefix) {
            for (const auto *info : group->getStats()) {
                if (const auto *scalar = dynamic_cast<const statistics::ScalarInfo *>(info)) {
                    const double value = scalar->value();
                    if (std::isfinite(value)) values[prefix + info->name] = value;
                } else if (const auto *vector = dynamic_cast<const statistics::VectorInfo *>(info)) {
                    const auto &entries = vector->value();
                    for (unsigned i = 0; i < entries.size(); ++i) {
                        if (std::isfinite(entries[i]))
                            values[prefix + info->name + "::" +
                                (i < vector->subnames.size() && !vector->subnames[i].empty() ?
                                 vector->subnames[i] : std::to_string(i))] = entries[i];
                    }
                }
            }
            for (const auto &[name, child] : group->getStatGroups())
                collect(child, prefix + name + ".");
        };
    collect(marker_cpu_, marker_cpu_->name() + ".");
    for (const auto *object : stat_objects_)
        if (object != marker_cpu_) collect(object, object->name() + ".");
    snapshots_ << "{\"schema\":\"gem5-marker-core-snapshot-v1\",\"kind\":\""
        << kind << "\",\"worker_id\":" << worker_
        << ",\"cpu_id\":" << marker_cpu_->cpuId()
        << ",\"cr3\":" << marker_address_space_ << ",\"pc\":" << pc
        << ",\"spec_ctrl\":" << marker_cpu_->getContext(0)->readMiscRegNoEffect(
            X86ISA::misc_reg::SpecCtrl)
        << ",\"tick\":" << curTick()
        << ",\"clock_period_ticks\":" << marker_cpu_->clockPeriod()
        << ",\"core_path\":\"" << marker_cpu_->name() << "\""
        << ",\"stat_object_paths\":[";
    bool first = true;
    for (const auto *object : stat_objects_) {
        if (!first) snapshots_ << ",";
        snapshots_ << "\"" << object->name() << "\"";
        first = false;
    }
    snapshots_ << "]"
        << ",\"completed_work\":";
    if (kind[0] == 'E' && !begin_pcs_.empty()) snapshots_ << completed;
    else snapshots_ << "null";
    snapshots_ << ",\"stats\":{\"simTicks\":" << curTick();
    for (const auto &[name, value] : values)
        snapshots_ << ",\"" << name << "\":" << std::setprecision(17) << value;
    snapshots_ << "}}\n";
    snapshots_.flush();
    fatal_if(!snapshots_, "Cannot write per-core Marker snapshot");
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
    auto *owner = this;
    if (!selected_ || !marker_capture_started_ ||
        owner->marker_measurement_phase_ == MarkerMeasurementPhase::Complete ||
        (inst->isMicroop() && !inst->isLastMicroop()))
        return;
    const Addr pc = inst->pcState().instAddr();
    const bool fixed = !begin_pcs_.empty();
    const bool fixed_begin = fixed && pc == begin_pcs_[worker_];
    const bool fixed_end = fixed && pc == end_pcs_[worker_];
    const bool endpoint = fixed ? fixed_begin || fixed_end : pc == marker_pc_;
    if (endpoint) {
        // Endpoints belong to the retiring context. Fetch metadata can lag
        // x86 privilege returns and address-space transitions.
        auto *tc = owner->marker_cpu_->tcBase(inst->threadNumber);
        const uint64_t root = tc->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) &
                              ~uint64_t(0xfff);
        if (!owner->marker_cpu_->inUserMode(inst->threadNumber) || !root ||
            (owner->marker_address_space_ && owner->marker_address_space_ != root)) {
            if (fixed_end)
                inform("CalibrationMarker: rejected END pc=%#lx cr3=%#lx "
                       "expected=%#lx user=%d seq=%lu",
                       pc, root, owner->marker_address_space_,
                       owner->marker_cpu_->inUserMode(inst->threadNumber),
                       inst->seqNum);
            return;
        }
        owner->marker_address_space_ = root;
        if (fixed) {
            fatal_if(uint64_t(tc->getReg(X86ISA::int_reg::Rdi)) != workids_[worker_] ||
                     uint64_t(tc->getReg(X86ISA::int_reg::Rsi)) != worker_,
                     "Marker fixed-work worker or work identity mismatch");
        }
    } else if (!inst->fetchedFromUser || !inst->fetchedAddressSpaceId ||
        (owner->marker_address_space_ &&
         owner->marker_address_space_ != inst->fetchedAddressSpaceId))
        return;
    // Fetch-time CPL can lag an x86 exception entry. Linux user PCs occupy
    // the lower canonical half, with the legacy vsyscall page as an exception.
    const bool linuxUserPc = !(pc >> 63) ||
        (pc >= 0xffffffffff600000ULL && pc < 0xffffffffff601000ULL);
    if (owner->marker_measurement_phase_ == MarkerMeasurementPhase::Measuring &&
        owner->marker_pc_trace_ && linuxUserPc) {
        const int64_t fetch_tick = inst->fetchTick == Tick(-1) ? -1 : int64_t(inst->fetchTick);
        owner->marker_pc_trace_->append(pc, curTick(), fetch_tick,
            inst->dispatchTick, inst->issueTick, inst->completeTick,
            inst->dataReturnTick == Tick(-1) ? -1 : int64_t(inst->dataReturnTick),
            inst->lastWakeDependents == Tick(-1) ? -1 : int64_t(inst->lastWakeDependents));
    }
    if (!endpoint) return;
    ++owner->marker_ordinal_;
    const bool begin = fixed ? fixed_begin : owner->marker_ordinal_ == owner->marker_begin_;
    const bool end = fixed ? fixed_end : owner->marker_ordinal_ == owner->marker_end_;
    if (!begin && !end) return;
    fatal_if((begin && marker_measurement_phase_ != MarkerMeasurementPhase::Warming) ||
             (end && marker_measurement_phase_ != MarkerMeasurementPhase::Measuring),
             "Marker core window endpoint order mismatch");
    owner->marker_measurement_phase_ = begin ?
        MarkerMeasurementPhase::Measuring : MarkerMeasurementPhase::Complete;
    if (end && owner->marker_pc_trace_) owner->marker_pc_trace_->finalizeTrace();
    if (begin) {
        owner->marker_cpu_->beginMemoryObservation();
        if (owner->marker_cpu_->tmaEnabled)
            owner->marker_cpu_->tmaStats.begin();
    } else {
        if (owner->marker_cpu_->tmaEnabled)
            owner->marker_cpu_->tmaStats.end();
        owner->marker_cpu_->endMemoryObservation();
    }
    // Snapshot at retirement without draining or changing pipeline state.
    const uint64_t completed = fixed && end ?
        uint64_t(marker_cpu_->tcBase(inst->threadNumber)->getReg(X86ISA::int_reg::Rdx)) : 0;
    snapshot(begin ? "BEGIN" : "END", pc, completed);
    if (marker_instances_.size() == 1) statistics::dump();
    if (begin && owner->marker_pc_trace_) owner->marker_pc_trace_->initializeTrace();
    if (end && std::all_of(marker_instances_.begin(), marker_instances_.end(),
            [](auto *marker) {
                return marker->marker_measurement_phase_ == MarkerMeasurementPhase::Complete;
            }))
        exitSimulationLoopNow(8, {{"kind", "end"}});
}
} }
