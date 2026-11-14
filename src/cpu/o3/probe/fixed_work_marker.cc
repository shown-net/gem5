// SPDX-License-Identifier: BSD-3-Clause
// Fixed physical-core windows; no fences, task-owner inference or timing feedback.
#include "cpu/o3/probe/fixed_work_marker.hh"
#include "cpu/o3/probe/tao_trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/stack_engine.hh"
#include "cpu/thread_context.hh"
#include "mem/taotrace_response.hh"
#include "mem/translating_port_proxy.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"
#include "sim/sim_exit.hh"
#include "sim/sim_object.hh"
#include "sim/system.hh"
#include <algorithm>
#include <fstream>
#include <set>

namespace gem5 { namespace o3 {
std::unordered_set<FixedWorkMarker *> FixedWorkMarker::instances_;
gem5::ThreadContext *FixedWorkMarker::handoff_context_ = nullptr;
bool FixedWorkMarker::handed_off_ = false;
bool FixedWorkMarker::team_ended_ = false;
Tick FixedWorkMarker::snapshot_origin_ = 0;
FixedWorkMarker *FixedWorkMarker::snapshot_coordinator_ = nullptr;

FixedWorkMarker::FixedWorkMarker(TaoTrace &owner, const TaoTraceParams &params)
    : owner_(owner), cpu_(dynamic_cast<CPU *>(params.manager)),
      begins_(params.fixed_work_begin_pcs), ends_(params.fixed_work_end_pcs),
      cores_(params.fixed_work_core_ids), workids_(params.fixed_work_workids),
      stat_objects_(params.marker_stat_objects),
      result_pc_(params.fixed_work_result_pc),
      result_max_bytes_(params.fixed_work_result_max_bytes),
      snapshot_interval_(params.fixed_work_snapshot_interval),
      snapshot_event_([this] { periodicSnapshot(); },
                      owner.name() + ".fixed_work_snapshot", false,
                      Event::CPU_Tick_Pri + 1)
{
    fatal_if(!cpu_ || begins_.empty() || begins_.size() > 4 ||
             ends_.size() != begins_.size() || cores_.size() != begins_.size() ||
             workids_.size() != begins_.size(), "invalid fixed-work participants");
    std::set<Addr> pcs;
    std::set<unsigned> cores;
    bool found = false;
    for (unsigned i = 0; i < begins_.size(); ++i) {
        fatal_if(!begins_[i] || !ends_[i] || !pcs.insert(begins_[i]).second ||
                 !pcs.insert(ends_[i]).second || !cores.insert(cores_[i]).second,
                 "fixed-work PCs and cores must be unique");
        if (int(cores_[i]) == params.fixed_work_source_core) {
            worker_ = i;
            found = true;
        }
    }
    fatal_if(!found || cpu_->numThreads != 1, "fixed-work source lacks a unique worker");
    fatal_if(bool(result_pc_) != bool(result_max_bytes_) || pcs.count(result_pc_),
             "fixed-work result PC requires a buffer bound and a distinct endpoint");
    cpu_->markerObservation = true;
    instances_.insert(this);
}
FixedWorkMarker::~FixedWorkMarker()
{
    if (snapshot_event_.scheduled()) owner_.deschedule(snapshot_event_);
    if (snapshot_coordinator_ == this) snapshot_coordinator_ = nullptr;
    instances_.erase(this);
    if (snapshots_) std::fclose(snapshots_);
}
void FixedWorkMarker::handoff(gem5::ThreadContext *tc)
{
    fatal_if(handed_off_ || !tc, "fixed-work HANDOFF must occur exactly once");
    handed_off_ = true;
    // No single checkpoint CR3 is advertised as the truth for forked workers.
    if (simQuantum) {
        handoff_context_ = tc;
        tc->halt();
    }
}
void FixedWorkMarker::resume()
{
    if (!handoff_context_) return;
    auto *tc = handoff_context_;
    handoff_context_ = nullptr;
    tc->getSystemPtr()->threads[tc->contextId()]->activate();
}
void FixedWorkMarker::start()
{
    fatal_if(capturing_ || !handed_off_ || instances_.size() != cores_.size(),
             "fixed-work capture requires one trace per participant and HANDOFF");
    fatal_if(!std::all_of(instances_.begin(), instances_.end(), [this](auto *marker) {
                 return marker->result_pc_ == result_pc_ &&
                        marker->result_max_bytes_ == result_max_bytes_ &&
                        marker->snapshot_interval_ == snapshot_interval_;
             }), "fixed-work participants disagree on result completion");
    const std::string path = owner_.output_dir_ + "/fixed-work-snapshots-core" +
                             std::to_string(cores_[worker_]) + ".jsonl";
    snapshots_ = std::fopen(path.c_str(), "w");
    fatal_if(!snapshots_, "cannot open fixed-work snapshots");
    capturing_ = true;
}
void FixedWorkMarker::scheduleSnapshot()
{
    if (!snapshot_interval_ || snapshot_coordinator_ != this) return;
    const Tick next = snapshot_origin_ +
        ((curTick() - snapshot_origin_) / snapshot_interval_ + 1) * snapshot_interval_;
    owner_.schedule(snapshot_event_, next);
}
void FixedWorkMarker::periodicSnapshot()
{
    fatal_if(snapshot_coordinator_ != this || team_ended_,
             "invalid fixed-work team snapshot event");
    // One event observes the team after all CPU tick/Commit events at this tick.
    // A BEGIN or END at this same tick is the only sample for that worker.
    std::vector<FixedWorkMarker *> ordered(instances_.begin(), instances_.end());
    std::sort(ordered.begin(), ordered.end(), [](auto *a, auto *b) {
        return a->worker_ < b->worker_;
    });
    for (auto *marker : ordered) {
        if (marker->capturing_ && marker->phase_ == 1 &&
            marker->begin_tick_ != curTick())
            marker->snapshot("TICK", 0);
    }
    scheduleSnapshot();
}
void FixedWorkMarker::snapshot(const char *kind, Addr pc)
{
    // CPU native retirement counters are updated before the Commit probe.
    cpu_->unhaltedClockStats.account();
    auto *mmu = cpu_->tcBase(0)->getMMUPtr();
    cpu_->preDumpStats();
    mmu->preDumpStats();
    for (auto *object : stat_objects_)
        if (object && object != cpu_) object->preDumpStats();
    std::fprintf(snapshots_,
        "{\"schema\":\"gem5-fixed-work-snapshot\",\"kind\":\"%s\","
        "\"worker\":%u,\"source_core\":%u,\"workid\":%lu,\"cr3\":%lu,"
        "\"pc\":%lu,\"tick\":%lu,\"record_after\":%lu,\"macro_after\":%lu,"
        "\"snapshot_interval_ticks\":%lu,\"snapshot_origin_tick\":%lu,"
        "\"clock_period_ticks\":%lu,\"numCycles\":%.0f,"
        "\"INST_RETIRED.ANY\":%.0f,\"UOPS_RETIRED.SLOTS\":%.0f,"
        "\"gem5_functional_uops\":%.0f,"
        "\"CPU_CLK_UNHALTED.THREAD\":%.0f,\"CPU_CLK_UNHALTED.REF_TSC\":%.0f,"
        "\"BR_INST_RETIRED.ALL_BRANCHES\":%.0f,\"BR_MISP_RETIRED.ALL_BRANCHES\":%.0f,"
        "\"MEM_INST_RETIRED.ALL_LOADS\":%.0f,\"MEM_INST_RETIRED.ALL_STORES\":%.0f,"
        "\"MEM_LOAD_RETIRED.L1_HIT\":%.0f,\"MEM_LOAD_RETIRED.L1_MISS\":%.0f,"
        "\"MEM_LOAD_RETIRED.L2_HIT\":%.0f,\"MEM_LOAD_RETIRED.L2_MISS\":%.0f,"
        "\"MEM_LOAD_RETIRED.L3_HIT\":%.0f,\"MEM_LOAD_RETIRED.L3_MISS\":%.0f,"
        "\"MEM_LOAD_RETIRED.FB_HIT\":%.0f,\"MEM_LOAD_RETIRED.sourceIncompleteResponse\":%.0f,"
        "\"tma.slots\":%.0f,\"tma.retiring\":%.0f,\"tma.badSpeculation\":%.0f,"
        "\"tma.frontendBound\":%.0f,\"tma.backendBound\":%.0f,\"tma.pending\":%.0f",
        kind, worker_, cores_[worker_], workids_[worker_],
        cr3_, uint64_t(pc), uint64_t(curTick()),
        owner_.fst_record_count_, owner_.functional_instruction_count_,
        uint64_t(snapshot_interval_), uint64_t(snapshot_origin_),
        uint64_t(cpu_->clockPeriod()), cpu_->baseStats.numCycles.value(),
        cpu_->instRetiredStats.ANY.value(), cpu_->uopsRetiredStats.SLOTS.value(),
        cpu_->commitStats[0]->numOps.value() -
            (cpu_->stackEngine ? cpu_->stackEngine->retiredSynchronizations() : 0),
        cpu_->unhaltedClockStats.THREAD.value(), cpu_->unhaltedClockStats.REF_TSC.value(),
        cpu_->branchInstStats.ALL_BRANCHES.value(), cpu_->branchMispStats.ALL_BRANCHES.value(),
        cpu_->memoryInstRetiredStats.ALL_LOADS.value(), cpu_->memoryInstRetiredStats.ALL_STORES.value(),
        cpu_->memoryLoadRetiredStats.L1_HIT.value(), cpu_->memoryLoadRetiredStats.L1_MISS.value(),
        cpu_->memoryLoadRetiredStats.L2_HIT.value(), cpu_->memoryLoadRetiredStats.L2_MISS.value(),
        cpu_->memoryLoadRetiredStats.L3_HIT.value(), cpu_->memoryLoadRetiredStats.L3_MISS.value(),
        cpu_->memoryLoadRetiredStats.FB_HIT.value(), cpu_->memoryLoadRetiredStats.sourceIncompleteResponse.value(),
        cpu_->tmaStats.slots.value(), cpu_->tmaStats.retiring.value(),
        cpu_->tmaStats.badSpeculation.value(), cpu_->tmaStats.frontendBound.value(),
        cpu_->tmaStats.backendBound.value(), cpu_->tmaStats.pending.value());
    auto emitScalar = [this](const statistics::Group *group,
                             const char *path, const char *name) {
        const auto *stat = dynamic_cast<const statistics::ScalarInfo *>(group->resolveStat(path));
        if (stat) std::fprintf(snapshots_, ",\"%s\":%.0f", name, double(stat->value()));
    };
    emitScalar(mmu, "dtb.DTLB_LOAD_MISSES.STLB_HIT", "DTLB_LOAD_MISSES.STLB_HIT");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.WALK_COMPLETED", "DTLB_LOAD_MISSES.WALK_COMPLETED");
    emitScalar(mmu, "dtb.walker.dtlbLoadWalkOutcome.faultSquashed", "model.dtlb.walkCompletedFaultSquashed");
    emitScalar(mmu, "dtb.walker.dtlbLoadWalkOutcome.faultNotSquashed", "model.dtlb.walkCompletedFaultNotSquashed");
    emitScalar(mmu, "dtb.walker.dtlbLoadWalkOutcome.noFaultSquashed", "model.dtlb.walkCompletedNoFaultSquashed");
    emitScalar(mmu, "dtb.walker.dtlbLoadWalkOutcome.noFaultNotSquashed", "model.dtlb.walkCompletedNoFaultNotSquashed");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.WALK_ACTIVE", "DTLB_LOAD_MISSES.WALK_ACTIVE");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.cancelledQueued", "model.mmu.walkCancelledQueued");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.cancelledStarted", "model.mmu.walkCancelledStarted");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.sharedFaultRequests", "model.mmu.walkSharedFaultRequests");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.pteReadRequests", "model.mmu.pteReadRequests");
    emitScalar(mmu, "dtb.walker.DTLB_LOAD_MISSES.pteReadResponses", "model.mmu.pteReadResponses");
    emitScalar(cpu_, "RESOURCE_STALLS.SB", "RESOURCE_STALLS.SB");
    emitScalar(cpu_, "lsq0.storeDrainLineCreditBlocked", "model.lsq.storeDrainLineCreditBlocked");
    emitScalar(cpu_, "lsq0.storeBufferSent", "model.lsq.storeBufferSent");
    emitScalar(cpu_, "lsq0.storeBufferCompleted", "model.lsq.storeBufferCompleted");
    emitScalar(cpu_, "lsq0.storeBufferMerged", "model.lsq.storeBufferMerged");
    emitScalar(cpu_, "lsq0.storePrepareSpeculativeIssued", "model.lsq.storePrepareSpeculativeIssued");
    // Framework passes the actual private L1 and L2 controllers, in that
    // order. Keep their object identities alongside the candidate counters.
    if (stat_objects_.size() >= 2 && stat_objects_[0] && stat_objects_[1]) {
        const auto *l1 = stat_objects_[0];
        const auto *l2 = stat_objects_[1];
        std::fprintf(snapshots_, ",\"model_stat_l1_source\":\"%s\",\"model_stat_l2_source\":\"%s\"",
                     l1->name().c_str(), l2->name().c_str());
        emitScalar(l1, "Dcache.dataFillDemandWaitFullCycles", "L1D_PEND_MISS.FB_FULL");
        emitScalar(l1, "Dcache.dataFillDemandBlockedCycles", "model.l1.fillAdmissionBlockedCycles");
        emitScalar(l1, "Dcache.dataReplacements", "L1D.REPLACEMENT");
        emitScalar(l2, "cache.rfoHits", "L2_RQSTS.RFO_HIT");
        emitScalar(l2, "cache.rfoTrueMisses", "L2_RQSTS.RFO_MISS");
        emitScalar(l2, "cache.storePrepareHits", "model.l2.storePrepareHits");
        emitScalar(l2, "cache.storePrepareTrueMisses", "model.l2.storePrepareTrueMisses");
        emitScalar(l2, "cache.softwarePrefetchRfo", "model.l2.softwarePrefetchRfo");
    }
    emitScalar(cpu_, "frontend.notDeliveredSlots", "frontend.notDeliveredSlots");
    emitScalar(cpu_, "frontend.zeroDeliveryCycles", "frontend.zeroDeliveryCycles");
    emitScalar(cpu_, "frontend.mite.sourceOps", "frontend.mite.sourceOps");
    emitScalar(cpu_, "frontend.dsb.sourceOps", "frontend.dsb.sourceOps");
    std::fprintf(snapshots_, ",\"spec_ctrl\":%lu",
        uint64_t(cpu_->tcBase(0)->readMiscRegNoEffect(X86ISA::misc_reg::SpecCtrl)));
    if (kind[0] == 'E')
        std::fprintf(snapshots_, ",\"completed_work\":%lu", completed_work_);
    else
        std::fputs(",\"completed_work\":null", snapshots_);
    std::fputs("}\n", snapshots_);
    std::fflush(snapshots_);
}
void FixedWorkMarker::observe(const DynInstPtr &inst, uint64_t before)
{
    if (!capturing_ || !inst || !inst->staticInst ||
        (inst->isMicroop() && !inst->isLastMicroop())) return;
    const Addr pc = inst->pcState().instAddr();
    if (result_pc_ && pc == result_pc_ && owner_.isUserInstruction(inst)) {
        result(inst, before);
        return;
    }
    for (unsigned i = 0; i < begins_.size(); ++i) {
        const bool begin = pc == begins_[i], end = pc == ends_[i];
        if (!begin && !end) continue;
        if (!owner_.isUserInstruction(inst)) return;
        auto *tc = cpu_->tcBase(inst->threadNumber);
        const uint64_t root = tc->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) & ~uint64_t(0xfff);
        fatal_if(i != worker_ || !root ||
                 uint64_t(tc->getReg(X86ISA::int_reg::Rdi)) != workids_[i] ||
                 uint64_t(tc->getReg(X86ISA::int_reg::Rsi)) != i,
                 "fixed-work worker/core/workid mismatch");
        fatal_if(owner_.fst_record_count_ != before + 1,
                 "fixed-work NOP must retain exactly one FST record");
        fatal_if((begin && phase_ != 0) || (end && phase_ != 1) ||
                 (end && root != cr3_), "invalid fixed-work BEGIN/END or CR3 continuity");
        if (begin) {
            if (!snapshot_coordinator_) {
                snapshot_origin_ = curTick();
                snapshot_coordinator_ = this;
            }
            cr3_ = root;
            cpu_->beginMemoryObservation();
            cpu_->tmaStats.begin();
            begin_record_ = owner_.fst_record_count_;
            begin_tick_ = curTick();
            begin_macro_ = owner_.functional_instruction_count_;
            snapshot("BEGIN", pc);
            phase_ = 1;
            if (snapshot_coordinator_ == this) scheduleSnapshot();
            owner_.cpl_core_id_ = cores_[worker_];
            TaoTraceNativeAccessRegistry::enableContext(uint32_t(tc->contextId()));
            owner_.initializeCpl(begin_tick_, cpu_->clockPeriod(), true);
        } else {
            end_record_ = owner_.fst_record_count_;
            end_tick_ = curTick();
            end_macro_ = owner_.functional_instruction_count_;
            completed_work_ = uint64_t(tc->getReg(X86ISA::int_reg::Rdx));
            cpu_->tmaStats.end();
            cpu_->endMemoryObservation();
            snapshot("END", pc);
            phase_ = 2;
            // Existing identities still accept responses. Finalize must not
            // admit fresh requests into the closed business window.
            TaoTraceNativeAccessRegistry::disableContext(uint32_t(tc->contextId()));
            owner_.finalizeCplMeasurement();
            if (std::all_of(instances_.begin(), instances_.end(),
                            [](auto *marker) { return marker->phase_ == 2; })) {
                fatal_if(team_ended_, "duplicate fixed-work team END");
                team_ended_ = true;
                // Observe each producer at the last END's actual commit event.
                // Other CPU events at the same tick may still follow.
                for (auto *marker : instances_) {
                    marker->team_end_tick_ = curTick();
                    marker->team_end_record_ = marker->owner_.fst_record_count_;
                }
                if (snapshot_coordinator_ && snapshot_coordinator_->snapshot_event_.scheduled())
                    snapshot_coordinator_->owner_.deschedule(snapshot_coordinator_->snapshot_event_);
                exitSimulationLoopNow(9, {{"kind", "fixed_work_team_end"}});
            }
        }
        return;
    }
}
void FixedWorkMarker::result(const DynInstPtr &inst, uint64_t before)
{
    auto *tc = cpu_->tcBase(inst->threadNumber);
    const uint64_t root = tc->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) & ~uint64_t(0xfff);
    fatal_if(phase_ != 2 || result_ready_ || root != cr3_ ||
             uint64_t(tc->getReg(X86ISA::int_reg::Rdi)) != workids_[worker_] ||
             uint64_t(tc->getReg(X86ISA::int_reg::Rsi)) != worker_ ||
             owner_.fst_record_count_ != before + 1,
             "invalid fixed-work result NOP, identity or phase");
    input_addr_ = tc->getReg(X86ISA::int_reg::Rdx);
    input_bytes_ = tc->getReg(X86ISA::int_reg::Rcx);
    output_addr_ = tc->getReg(X86ISA::int_reg::R8);
    output_bytes_ = tc->getReg(X86ISA::int_reg::R9);
    fatal_if(!input_addr_ || !output_addr_ || !input_bytes_ || !output_bytes_ ||
             input_bytes_ > result_max_bytes_ || output_bytes_ > result_max_bytes_ ||
             input_bytes_ > Addr(-1) - input_addr_ ||
             output_bytes_ > Addr(-1) - output_addr_,
             "fixed-work result buffers exceed the configured bounds");
    result_context_ = tc;
    result_record_ = owner_.fst_record_count_;
    result_tick_ = curTick();
    result_ready_ = true;
    if (std::all_of(instances_.begin(), instances_.end(),
                    [](auto *marker) { return marker->result_ready_; })) {
        fatal_if(!team_ended_, "fixed-work results ready before team END");
        exitSimulationLoopNow(10, {{"kind", "fixed_work_results_ready"}});
    }
}
void FixedWorkMarker::exportResult()
{
    fatal_if(!result_pc_ || capturing_ ||
             !std::all_of(instances_.begin(), instances_.end(),
                          [](auto *marker) { return !marker->capturing_; }) ||
             !result_ready_ || !result_context_ ||
             (result_context_->readMiscRegNoEffect(X86ISA::misc_reg::Cr3) &
              ~uint64_t(0xfff)) != cr3_,
             "fixed-work result address space changed before drained export");
    // The caller has drained O3/Ruby and frozen native accounting. Functional
    // virtual reads must see dirty Ruby data, not just DRAM backing storage.
    TranslatingPortProxy proxy(result_context_);
    const std::string stem = owner_.output_dir_ + "/fixed-work-";
    const std::string suffix = "-core" + std::to_string(cores_[worker_]);
    auto write = [&](const std::string &name, Addr addr, uint64_t size) {
        std::vector<char> bytes(size);
        fatal_if(!proxy.tryReadBlob(addr, bytes.data(), size),
                 "cannot read fixed-work result buffer through virtual memory");
        std::ofstream file(stem + name + suffix + ".bin", std::ios::binary);
        file.write(bytes.data(), size);
        file.close();
        fatal_if(!file, "cannot write fixed-work result buffer");
    };
    write("input", input_addr_, input_bytes_);
    write("output", output_addr_, output_bytes_);
    std::ofstream file(stem + "result" + suffix + ".json");
    file << "{\"schema\":\"gem5-fixed-work-result-v1\",\"status\":\"exported\","
         << "\"completion\":\"result-buffers-drained\",\"worker\":" << worker_
         << ",\"source_core\":" << cores_[worker_] << ",\"workid\":" << workids_[worker_]
         << ",\"cr3\":" << cr3_ << ",\"pc\":" << result_pc_
         << ",\"result_record\":" << result_record_
         << ",\"input_bytes\":" << input_bytes_ << ",\"output_bytes\":" << output_bytes_
         << ",\"input_addr\":" << input_addr_ << ",\"output_addr\":" << output_addr_
         << "}\n";
    file.close();
    fatal_if(!file, "cannot write fixed-work result metadata");
}
void FixedWorkMarker::finish()
{
    fatal_if(!capturing_ || phase_ != 2 || !team_ended_, "incomplete fixed-work capture");
    fatal_if(result_pc_ && !std::all_of(instances_.begin(), instances_.end(),
             [](auto *marker) { return marker->result_ready_; }),
             "fixed-work capture ended before all result buffers were ready");
    fatal_if(!owner_.nativeResponseDrainComplete(),
             "fixed-work native observer did not close after simulation drain");
    owner_.finalizeNativeResponseDiagnostic();
    capturing_ = false;
    owner_.finalizeFst();
    std::fflush(owner_.out_fst_);
    const std::string path = owner_.output_dir_ + "/fixed-work-boundary-core" +
                             std::to_string(cores_[worker_]) + ".json";
    std::ofstream file(path);
    fatal_if(!file, "cannot write fixed-work boundary");
    file << "{\"schema\":\"gem5-fixed-work-boundary-v1\",\"status\":\"complete\","
         << "\"scope\":\"physical-core\",\"task_active_available\":false,"
         << "\"worker\":" << worker_ << ",\"source_core\":" << cores_[worker_]
         << ",\"workid\":" << workids_[worker_] << ",\"cr3\":" << cr3_
         << ",\"score_begin_record\":" << begin_record_
         << ",\"score_end_record\":" << end_record_
         << ",\"team_end_observed_tick\":" << team_end_tick_
         << ",\"team_end_observed_record\":" << team_end_record_
         << ",\"result_tick\":" << result_tick_
         << ",\"result_record\":" << result_record_
         << ",\"execution_end_record\":" << owner_.fst_record_count_
         << ",\"execution_end_tick\":" << curTick()
         << ",\"begin_tick\":" << begin_tick_ << ",\"end_tick\":" << end_tick_
         << ",\"completed_work\":" << completed_work_
         << ",\"begin_macro_after\":" << begin_macro_ << ",\"end_macro_after\":" << end_macro_
         << ",\"execution_macros\":" << owner_.functional_instruction_count_
         << ",\"completion\":\"" << (result_pc_ ? "result-buffers-drained" : "guest-exit")
         << "\""
         << ",\"fst_path\":\"" << owner_.fst_path_ << "\","
         << "\"pte_scope\":\"dynamic-ASMAP-translation-facts;initial-PTE-unknown\","
         << "\"native_cache_window_status\":\"unavailable-no-fixed-work-origin-qualification\"}\n";
    file.close();
    fatal_if(!file, "failed writing fixed-work boundary");
}
} }
