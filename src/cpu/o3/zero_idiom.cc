// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/o3/zero_idiom.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5::o3
{
ZeroIdiom::Stats::Stats(statistics::Group *parent) :
    statistics::Group(parent),
    ADD_STAT(accepted, statistics::units::Count::get(), "Rename-eliminated operations"),
    ADD_STAT(completed, statistics::units::Count::get(), "Operations with all constant results ready"),
    ADD_STAT(retired, statistics::units::Count::get(), "Eliminated operations retired"),
    ADD_STAT(cancelled, statistics::units::Count::get(), "Eliminated operations cancelled"),
    ADD_STAT(kinds, statistics::units::Count::get(), "Accepted operations by semantic kind")
{
    kinds.init(5);
    kinds.subname(0, "none");
    kinds.subname(1, "gpr_xor");
    kinds.subname(2, "gpr_sub");
    kinds.subname(3, "xmm_xor");
    kinds.subname(4, "xmm_sub");
}
ZeroIdiom::ZeroIdiom(const ZeroIdiomParams &p) : SimObject(p), stats(this) {}

void ZeroIdiom::admit(const DynInstPtr &inst)
{
    panic_if(!inst->staticInst->isZeroIdiom() || inst->numSrcRegs() != 0 ||
             inst->getFault() != NoFault, "Invalid zero-idiom admission");
    panic_if(!operations.emplace(inst->seqNum, inst->threadNumber).second,
             "Zero idiom admitted twice");
    inst->prepareResultValues(12, curTick());
    inst->evaluateResultValues(curTick());
    inst->writeResultValues();
    inst->setExecuted();
    inst->setResultsWoken();
    stats.accepted++;
    stats.completed++;
    stats.kinds[unsigned(inst->staticInst->zeroIdiom())]++;
}
void ZeroIdiom::retire(ThreadID tid, InstSeqNum through)
{
    for (auto it = operations.begin(); it != operations.end() && it->first <= through;) {
        if (it->second == tid) {
            it = operations.erase(it);
            stats.retired++;
        } else ++it;
    }
    checkDrain();
}
void ZeroIdiom::cancel(InstSeqNum sequence)
{
    if (operations.erase(sequence)) stats.cancelled++;
    checkDrain();
}
void ZeroIdiom::checkDrain()
{
    if (empty() && drainState() == DrainState::Draining) signalDrainDone();
}
DrainState ZeroIdiom::drain()
{
    return empty() ? DrainState::Drained : DrainState::Draining;
}
}
