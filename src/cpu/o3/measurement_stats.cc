// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/cpu.hh"

#include "cpu/o3/dyn_inst.hh"

namespace gem5::o3
{

CPU::UopsRetiredStats::UopsRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "UOPS_RETIRED"),
      ADD_STAT(SLOTS, statistics::units::Count::get(),
               "Retired original-model allocation units (functional operations)")
{}

CPU::InstRetiredStats::InstRetiredStats(CPU *cpu)
    : statistics::Group(cpu, "INST_RETIRED"),
      ADD_STAT(ANY, statistics::units::Count::get(),
               "Retired architectural instructions, including NOPs, across this O3 core")
{}

CPU::UnhaltedClockStats::UnhaltedClockStats(CPU *cpu)
    : statistics::Group(cpu, "CPU_CLK_UNHALTED"), owner(cpu),
      ADD_STAT(THREAD, statistics::units::Cycle::get(),
               "Cycles while an O3 thread is active, including unscheduled cycles")
{}

void
CPU::UnhaltedClockStats::account()
{
    const uint64_t now = uint64_t(owner->curCycle());
    for (ThreadID tid = 0; tid < owner->numThreads; ++tid) {
        if (running[tid]) THREAD += now - lastCycle[tid];
        lastCycle[tid] = now;
    }
}

void
CPU::UnhaltedClockStats::setActive(ThreadID tid, bool value)
{
    account();
    running.at(tid) = value;
}

void
CPU::UnhaltedClockStats::preDumpStats()
{
    account();
    statistics::Group::preDumpStats();
}

void
CPU::UnhaltedClockStats::resetStats()
{
    account();
    statistics::Group::resetStats();
}

CPU::TmaStats::TmaStats(CPU *cpu, unsigned allocation_width)
    : statistics::Group(cpu, "tma"), owner(cpu), width(allocation_width),
      ADD_STAT(slots, statistics::units::Count::get(), "Model allocation slots"),
      ADD_STAT(retiring, statistics::units::Count::get(),
               "Allocated slots not known to be discarded by speculation"),
      ADD_STAT(badSpeculation, statistics::units::Count::get(),
               "Discarded speculative slots and recovery bubbles"),
      ADD_STAT(frontendBound, statistics::units::Count::get(),
               "Unfilled slots when the backend can accept work"),
      ADD_STAT(backendBound, statistics::units::Count::get(),
               "Unfilled slots blocked by backend resources"),
      ADD_STAT(pending, statistics::units::Count::get(),
               "Window allocations still in flight at the snapshot"),
      ADD_STAT(retired, statistics::units::Count::get(),
               "Window allocations that retired before the snapshot"),
      ADD_STAT(otherSquashes, statistics::units::Count::get(),
               "Window allocations discarded for a non-speculation reason"),
      ADD_STAT(allocatedOps, statistics::units::Count::get(),
               "Functional operations accepted in the measured window"),
      ADD_STAT(fusedPairs, statistics::units::Count::get(),
               "Deployment uses its original functional operation representation")
{}

void
CPU::TmaStats::emptySlots(uint64_t count, EmptyReason reason)
{
    slots += count;
    switch (reason) {
      case EmptyReason::Frontend: frontendBound += count; break;
      case EmptyReason::Backend: backendBound += count; break;
      case EmptyReason::Recovery: badSpeculation += count; break;
    }
}

void
CPU::TmaStats::accountUntil(uint64_t now)
{
    if (tracking && active && now > lastCycle)
        emptySlots((now - lastCycle) * width, previous);
    lastCycle = now;
}

void
CPU::TmaStats::setActive(bool value)
{
    if (active == value) return;
    accountUntil(uint64_t(owner->curCycle()));
    active = value;
}

void
CPU::TmaStats::begin()
{
    fatal_if(owner->numThreads != 1 || width != 6,
             "TMA slot model requires one thread and renameWidth=6");
    statistics::Group::resetStats();
    outstanding.clear();
    accepted = 0;
    recovery = sawRecoverySquash = false;
    previous = EmptyReason::Frontend;
    lastCycle = uint64_t(owner->curCycle());
    tracking = true;
}

void
CPU::TmaStats::end()
{
    accountUntil(uint64_t(owner->curCycle()));
    pending = outstanding.size();
    tracking = false;
}

void
CPU::TmaStats::allocated(const DynInstPtr &inst)
{
    if (!tracking || !active) return;
    ++allocatedOps; ++accepted; ++slots; ++retiring;
    outstanding.insert(inst->seqNum);
}
void
CPU::TmaStats::onRetire(const DynInstPtr &inst)
{
    if (tracking && outstanding.erase(inst->seqNum)) ++retired;
}
void
CPU::TmaStats::onOtherSquash(const DynInstPtr &inst)
{
    if (tracking && outstanding.erase(inst->seqNum)) ++otherSquashes;
}
void
CPU::TmaStats::squashAfter(InstSeqNum sequence, bool speculation)
{
    if (!tracking) return;
    auto it = outstanding.upper_bound(sequence);
    while (it != outstanding.end()) {
        if (speculation) { retiring -= 1; ++badSpeculation; }
        else ++otherSquashes;
        it = outstanding.erase(it);
    }
    if (speculation) { recovery = true; sawRecoverySquash = false; }
}

void
CPU::TmaStats::renameCycle(bool blocked, bool squashing)
{
    if (!tracking || !active) return;
    const uint64_t now = uint64_t(owner->curCycle());
    if (now > lastCycle + 1) accountUntil(now - 1);
    lastCycle = now;
    panic_if(accepted > width, "TMA accepted more than the allocation width");
    if (recovery && squashing) sawRecoverySquash = true;
    EmptyReason reason = EmptyReason::Frontend;
    if (recovery && squashing)
        reason = EmptyReason::Recovery;
    else if (blocked)
        reason = EmptyReason::Backend;
    else if (recovery && accepted == 0)
        reason = EmptyReason::Recovery;
    emptySlots(width - accepted, reason);
    previous = reason;
    if (recovery && sawRecoverySquash && accepted) {
        recovery = false;
        sawRecoverySquash = false;
    }
    accepted = 0;
}

void
CPU::TmaStats::preDumpStats()
{
    accountUntil(uint64_t(owner->curCycle()));
    if (tracking) pending = outstanding.size();
    statistics::Group::preDumpStats();
}

void
CPU::TmaStats::resetStats()
{
    accountUntil(uint64_t(owner->curCycle()));
    outstanding.clear();
    accepted = 0;
    recovery = sawRecoverySquash = false;
    previous = EmptyReason::Frontend;
    statistics::Group::resetStats();
}

} // namespace gem5::o3
