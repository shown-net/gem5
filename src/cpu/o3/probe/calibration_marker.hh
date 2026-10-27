// SPDX-License-Identifier: BSD-3-Clause
// Commit-point Marker control and optional architectural observation.
#pragma once
#include <memory>
#include <string>
#include <unordered_set>
#include "base/statistics.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/probes/marker_pc_trace.hh"
#include "params/TaoTrace.hh"
#include "sim/probe/probe.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5 { class ThreadContext;
namespace o3 {
class CPU;
class TaoTrace;
class CalibrationMarker
{
  public:
    CalibrationMarker(TaoTrace &owner, const TaoTraceParams &params);
    ~CalibrationMarker();
    void regProbeListeners();
    static bool hasInstances() { return !marker_instances_.empty(); }
    void startMarkerCapture(const std::string &directory);
    void selectMarkerWindow(uint64_t begin, uint64_t end);
    void resumeMarkerHandoff();
    static void traceMarkerHandoff(gem5::ThreadContext *tc);
    static void traceSourceRoiCheckpoint(gem5::ThreadContext *tc);
  private:
    TaoTrace &owner_;
    bool marker_capture_started_ = false;
    Addr marker_pc_;
    CPU *marker_cpu_;
    uint64_t marker_ordinal_ = 0, marker_begin_ = 1, marker_end_ = 2;
    uint64_t marker_address_space_ = 0;
    enum class MarkerMeasurementPhase { Warming, Measuring, Complete };
    MarkerMeasurementPhase marker_measurement_phase_ = MarkerMeasurementPhase::Warming;
    std::unique_ptr<MarkerPcTrace> marker_pc_trace_;
    const bool branch_observation_;
    std::unique_ptr<MarkerBranchTrace> marker_branch_trace_;
    uint64_t marker_macro_index_ = 0, marker_branch_index_ = 0;
    void observeMarkerBranch(const DynInstPtr &inst);
    void observeMarkerCommit(const DynInstPtr &inst);
    static CalibrationMarker *marker_owner_;
    static uint64_t marker_address_root_;
    static gem5::ThreadContext *marker_handoff_context_;
    static std::unordered_set<CalibrationMarker *> marker_instances_;
};
} }
