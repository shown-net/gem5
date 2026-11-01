// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>
#include "cpu/o3/dyn_inst_ptr.hh"
#include "params/TaoTrace.hh"

namespace gem5 { class ThreadContext;
namespace o3 {
class CPU;
class TaoTrace;
class FixedWorkMarker
{
  public:
    FixedWorkMarker(TaoTrace &owner, const TaoTraceParams &params);
    ~FixedWorkMarker();
    void start();
    void finish();
    void exportResult();
    void observe(const DynInstPtr &inst, uint64_t before);
    bool capturing() const { return capturing_; }
    bool scoring() const { return phase_ == 1; }
    uint64_t beginTick() const { return begin_tick_; }
    uint64_t endTick() const { return end_tick_; }
    static bool hasInstances() { return !instances_.empty(); }
    static void handoff(gem5::ThreadContext *tc);
    static void resume();
  private:
    void snapshot(bool begin, const DynInstPtr &inst);
    void result(const DynInstPtr &inst, uint64_t before);
    TaoTrace &owner_;
    CPU *cpu_;
    std::vector<Addr> begins_, ends_;
    std::vector<unsigned> cores_;
    std::vector<uint64_t> workids_;
    unsigned worker_ = 0;
    int phase_ = 0;
    bool capturing_ = false;
    const Addr result_pc_;
    const uint64_t result_max_bytes_;
    bool result_ready_ = false;
    gem5::ThreadContext *result_context_ = nullptr;
    Addr input_addr_ = 0, output_addr_ = 0;
    uint64_t input_bytes_ = 0, output_bytes_ = 0, result_record_ = 0;
    uint64_t cr3_ = 0, begin_record_ = 0, end_record_ = 0;
    uint64_t begin_tick_ = 0, end_tick_ = 0, begin_macro_ = 0, end_macro_ = 0;
    std::FILE *snapshots_ = nullptr;
    static std::unordered_set<FixedWorkMarker *> instances_;
    static gem5::ThreadContext *handoff_context_;
    static bool handed_off_, team_ended_;
};
} }
