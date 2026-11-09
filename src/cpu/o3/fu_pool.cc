/*
 * Copyright (c) 2012-2013, 2025 Arm Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/fu_pool.hh"
#include <algorithm>
#include <limits>

#include <algorithm>
#include <sstream>

#include "cpu/func_unit.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

////////////////////////////////////////////////////////////////////////////
//
//  A pool of function units
//

inline void
FUPool::FUIdxQueue::addFU(int fu_idx)
{
    funcUnitsIdx.push_back(fu_idx);
    ++size;
}

inline int
FUPool::FUIdxQueue::getFU()
{
    int retval = funcUnitsIdx[idx++];

    if (idx == size)
        idx = 0;

    return retval;
}

FUPool::~FUPool()
{
    for (FuncUnit* fu : funcUnits) {
        delete fu;
    }
}


// Constructor
FUPool::PoolStats::PoolStats(FUPool *pool)
    : statistics::Group(pool),
      ADD_STAT(issuedByUnit, statistics::units::Count::get(),
               "Model FU reservations, including paired store-data slots"),
      ADD_STAT(fpDividerCrossClassBlocked, statistics::units::Count::get(),
               "FP divide grants blocked by the shared divider after their own op-class interval")
{}
void
FUPool::bindIntegerResources(ExecutionPorts *ports, ResultForwarding *results,
                              IntExecutionResource *alu, IntExecutionResource *shift,
                              IntExecutionResource *slow, IntExecutionResource *bm,
                              IntExecutionResource *branch,
                              IntegerDivider *divider,
                              const std::vector<IntegerALU *> &alus)
{
    integerDivider = divider;
    fatal_if(ports && (!results || (!alu && alus.empty())),
             "Integer execution requires ports, ALUs and result forwarding");
    fatal_if(alu && !alus.empty(), "ALU family and individual ALUs duplicate resources");
    fatal_if(executionPorts && executionPorts != ports,
             "One FU coordinator cannot belong to different cores");
    executionPorts = ports;
    fatal_if(ports && numFU != 12,
             "Shared execution ports require one FU slot per physical port");
    forwarding = results;
    integerResources = {nullptr, alu, shift, slow, bm, branch};
    individualALUs = !alus.empty();
    integerALUs.fill(nullptr);
    for (auto unit : alus) {
        auto port = unit->connection();
        fatal_if(!ports || ports->get(port->number) != port || integerALUs[port->number],
                 "ALU must connect to one unique shared execution port");
        integerALUs[port->number] = unit;
    }
}

bool
FUPool::usesIntegerResources(const DynInstPtr &inst) const
{
    if (inst->repFunctional()) return false;
    if (inst->staticInst->intDivide().bytes && integerDivider)
        return true;
    if (inst->staticInst->intExecution().family == IntExecutionFamily::Divider)
        return false;
    // x86's branch lowering may have no integer-family tag. A macro-fused
    // follower is still the functional branch half of its producer's grant;
    // sending it through the generic FU path would consume a second port.
    if (executionPorts && inst->fusedFollower())
        return integerResources.at(unsigned(IntExecutionFamily::Branch));
    const auto family = inst->staticInst->intExecution().family;
    return executionPorts && family != IntExecutionFamily::None &&
        (integerResources.at(unsigned(family)) ||
         (family == IntExecutionFamily::ALU && individualALUs));
}

uint16_t
FUPool::portMask(const DynInstPtr &inst) const
{
    if (!executionPorts || !inst->issueSlots())
        return 0;
    if (usesIntegerResources(inst)) {
        const auto mask = inst->staticInst->intExecution().ports;
        return inst->fusedLeader() ? mask & 0x41 : mask;
    }
    uint16_t mask = capablePorts[inst->opClass()];
    if (inst->fusedLeader() ||
        (inst->isControl() && inst->opClass() == IntAluOp))
        mask &= 0x41;
    return mask;
}

void
FUPool::startAllocationCycle(Cycles cycle)
{
    if (!haveBindingCycle || bindingCycle != cycle) {
        // Dispatches in the preceding cycle have already removed their load.
        // Keep this snapshot unchanged throughout the current admission batch.
        allocationLoadSnapshot = pendingPortLoads;
        batchMaskUses.clear();
        bindingCycle = cycle;
        haveBindingCycle = true;
    }
}

void
FUPool::bindPort(const DynInstPtr &inst, Cycles cycle)
{
    assert(inst->boundExecutionPort == -1);
    if (inst->repFunctional()) return;
    const uint16_t mask = portMask(inst);
    if (!mask)
        return;
    startAllocationCycle(cycle);
    std::vector<unsigned> candidates;
    for (unsigned port = 0; port < 12; ++port)
        if (mask & (1u << port)) candidates.push_back(port);
    std::sort(candidates.begin(), candidates.end(), [this](auto a, auto b) {
        return allocationLoadSnapshot[a] != allocationLoadSnapshot[b] ?
            allocationLoadSnapshot[a] < allocationLoadSnapshot[b] : a > b;
    });
    const unsigned minimum = allocationLoadSnapshot[candidates.front()];
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [this, minimum](auto p) {
            return allocationLoadSnapshot[p] >= minimum + 5;
        }), candidates.end());
    const unsigned port = candidates[batchMaskUses[mask]++ % candidates.size()];
    inst->boundExecutionPort = port;
    ++pendingPortLoads[port];
}

void
FUPool::releasePortBinding(const DynInstPtr &inst)
{
    if (inst->boundExecutionPort < 0)
        return;
    const unsigned port = inst->boundExecutionPort;
    assert(port < pendingPortLoads.size() && pendingPortLoads[port]);
    --pendingPortLoads[port];
    inst->boundExecutionPort = -1;
}

bool
FUPool::issueInteger(CPU *cpu, const DynInstPtr &inst,
                     std::function<void()> available, std::function<void()> complete,
                     std::function<void()> issued_action)
{
    if (inst->staticInst->intDivide().bytes && integerDivider)
        return integerDivider->issue(cpu, forwarding, this, inst,
                                     available, complete, issued_action);
    const auto &info = inst->staticInst->intExecution();
    if (inst->fusedFollower()) {
        const Tick ready = forwarding->availableAt(inst, 12);
        if (ready > curTick()) {
            if (ready != std::numeric_limits<Tick>::max()) forwarding->defer(inst, ready);
            return false;
        }
        forwarding->clearWait(inst);
        integerResources.at(unsigned(IntExecutionFamily::Branch))->admit(
            cpu, forwarding, inst, 12, available, complete);
        return true;
    }
    const uint16_t candidates = inst->boundExecutionPort >= 0 ?
        1u << inst->boundExecutionPort : portMask(inst);
    const auto grant_family = inst->fusedLeader() ? IntExecutionFamily::Branch : info.family;
    const auto resourceAt = [this, &info](unsigned port) -> IntExecutionResource * {
        return info.family == IntExecutionFamily::ALU && individualALUs
            ? integerALUs[port] : integerResources.at(unsigned(info.family));
    };
    uint16_t eligible = 0;
    Tick next_path = std::numeric_limits<Tick>::max();
    for (unsigned port = 0; port < 12; ++port) {
        if (!(candidates & (1 << port)))
            continue;
        auto resource = resourceAt(port);
        if (!resource)
            continue;
        const Tick ready = forwarding->availableAt(inst, port);
        if (ready <= curTick() && !unitBusy[port] &&
            resource->accepts(info.family, port, curTick()))
            eligible |= 1 << port;
        else if (ready > curTick())
            next_path = std::min(next_path, ready);
    }
    const int port = executionPorts->probe(grant_family, candidates, curTick(), eligible);
    if (port < 0) {
        if (next_path != std::numeric_limits<Tick>::max())
            forwarding->defer(inst, next_path);
        return false;
    }
    forwarding->clearWait(inst);
    executionPorts->commit(grant_family, candidates, port, curTick());
    resourceAt(port)->admit(cpu, forwarding, inst, port, available, complete);
    cpu->recordExecutionAction(inst, port);
    return true;
}

void
FUPool::takeOverFrom()
{
    pendingPortLoads.fill(0);
    allocationLoadSnapshot.fill(0);
    batchMaskUses.clear();
    haveBindingCycle = false;
    if (forwarding)
        forwarding->reset();
}

FUPool::FUPool(const Params &p)
    : SimObject(p), storeDataPorts(p.storeDataPorts), branchPorts(p.branchPorts),
      stats(this)
{
    numFU = 0;

    funcUnits.clear();

    maxOpLatencies.fill(Cycles(0));
    pipelined.fill(true);

    //
    //  Iterate through the list of FUDescData structures
    //
    for (FUDesc *i : p.FUList) {
        //
        //  Don't bother with this if we're not going to create any FU's
        //
        if (i->number) {
            //
            //  Create the FuncUnit object from this structure
            //   - add the capabilities listed in the FU's operation
            //     description
            //
            //  We create the first unit, then duplicate it as needed
            //
            FuncUnit *fu = new FuncUnit;
            std::array<Cycles, Num_OpClasses> intervals;
            intervals.fill(Cycles(1));

            for (OpDesc *j : i->opDescList) {
                fatal_if(j->issueLat < Cycles(1),
                         "FU issue interval must be positive");
                intervals[j->opClass] = j->issueLat;
                // indicate that this pool has this capability
                capabilityList.set(j->opClass);

                // Add each of the FU's that will have this capability to the
                // appropriate queue.
                for (int k = 0; k < i->number; ++k) {
                    fuPerCapList[j->opClass].addFU(numFU + k);
                    if (numFU + k < 12)
                        capablePorts[j->opClass] |= 1u << (numFU + k);
                }

                // indicate that this FU has the capability
                fu->addCapability(j->opClass, j->opLat, j->pipelined);

                if (j->opLat > maxOpLatencies[j->opClass])
                    maxOpLatencies[j->opClass] = j->opLat;

                if (!j->pipelined)
                    pipelined[j->opClass] = false;
            }

            numFU++;

            //  Add the appropriate number of copies of this FU to the list
            fu->name = i->name() + "(0)";
            funcUnits.push_back(fu);
            issueIntervals.push_back(intervals);

            for (int c = 1; c < i->number; ++c) {
                std::ostringstream s;
                numFU++;
                FuncUnit *fu2 = new FuncUnit(*fu);

                s << i->name() << "(" << c << ")";
                fu2->name = s.str();
                funcUnits.push_back(fu2);
                issueIntervals.push_back(intervals);
            }
        }
    }

    unitBusy.resize(numFU);
    nextIssue.resize(numFU);
    fpDividerNextIssue.resize(numFU, Cycles(0));
    for (auto &ready : nextIssue)
        ready.fill(Cycles(0));
    pairedUnit.resize(numFU, -1);
    stats.issuedByUnit.init(numFU);
    for (int i = 0; i < numFU; ++i)
        stats.issuedByUnit.subname(i, "unit" + std::to_string(i));
    for (auto idx : storeDataPorts)
        fatal_if(idx >= static_cast<unsigned>(numFU), "Store-data port index outside FU pool");
    for (auto idx : branchPorts)
        fatal_if(idx >= static_cast<unsigned>(numFU), "Branch port index outside FU pool");

    for (int i = 0; i < numFU; i++) {
        unitBusy[i] = false;
    }
}

bool
FUPool::isCapable(OpClass capability)
{
    //  If this pool doesn't have the specified capability,
    //  return this information to the caller
    return capabilityList[capability];
}

bool
FUPool::fpDividerReady(OpClass capability, Cycles cycle)
{
    const bool divide = capability == FloatDivOp ||
        capability == SimdFloatDivOp || capability == X86FloatSqrt32Op ||
        capability == X86FloatSqrt64Op;
    if (!divide || !capabilityList[capability]) return true;
    bool shared_blocked = false;
    for (int unit = 0; unit < numFU; ++unit) {
        if (!funcUnits[unit]->provides(capability) || unitBusy[unit] ||
            cycle < nextIssue[unit][capability] ||
            (executionPorts && !executionPorts->get(unit)->available(curTick())))
            continue;
        if (cycle >= fpDividerNextIssue[unit]) return true;
        shared_blocked = true;
    }
    // One class-head admission probe replaces identical probes of all younger
    // operations. This remains an attempt count, not blocked cycles.
    if (shared_blocked) ++stats.fpDividerCrossClassBlocked;
    return false;
}

int
FUPool::getUnit(CPU *cpu, const DynInstPtr &inst, OpClass capability, Cycles cycle,
               bool store, bool branch,
               int bound_port)
{
    //  If this pool doesn't have the specified capability,
    //  return this information to the caller
    if (!capabilityList[capability])
        return NoCapableFU;

    int data_idx = -1;
    if (store && !storeDataPorts.empty()) {
        for (auto index : storeDataPorts) {
            if (!unitBusy[index] && (!executionPorts ||
                executionPorts->get(index)->available(curTick()))) {
                data_idx = index;
                break;
            }
        }
        if (data_idx < 0) return NoFreeFU;
    }

    int fu_idx = bound_port >= 0 ? bound_port : fuPerCapList[capability].getFU();
    int start_idx = fu_idx;
    const bool fp_divide = capability == FloatDivOp ||
        capability == SimdFloatDivOp || capability == X86FloatSqrt32Op ||
        capability == X86FloatSqrt64Op;
    const auto dividerAvailable = [&](int unit) {
        if (!fp_divide || cycle >= fpDividerNextIssue[unit]) return true;
        ++stats.fpDividerCrossClassBlocked;
        return false;
    };

    // Iterate through the circular queue if needed, stopping if we've reached
    // the first element again.
    while (unitBusy[fu_idx] || cycle < nextIssue[fu_idx][capability] ||
           !dividerAvailable(fu_idx) ||
           fu_idx == data_idx ||
           (executionPorts && !executionPorts->get(fu_idx)->available(curTick())) ||
           (branch && !branchPorts.empty() &&
            std::find(branchPorts.begin(), branchPorts.end(), fu_idx) ==
                branchPorts.end())) {
        if (bound_port >= 0)
            return NoFreeFU;
        fu_idx = fuPerCapList[capability].getFU();
        if (fu_idx == start_idx) {
            // No FU available
            return NoFreeFU;
        }
    }

    assert(fu_idx < numFU);

    unitBusy[fu_idx] = true;
    nextIssue[fu_idx][capability] = cycle + issueIntervals[fu_idx][capability];
    if (fp_divide)
        fpDividerNextIssue[fu_idx] = nextIssue[fu_idx][capability];
    if (fp_divide) {
        cpu->dividerStats.start(true);
        // The admitted fixed-latency FU operation remains in execution until
        // its completion/free boundary, including speculative instructions.
        cpu->schedule(new EventFunctionWrapper([cpu] {
            cpu->dividerStats.finish(true);
        }, name() + ".dividerComplete", true),
            cpu->clockEdge(getOpLatency(capability)));
    }
    if (executionPorts)
        executionPorts->commit(IntExecutionFamily::None, 1 << fu_idx,
                               fu_idx, curTick());
    ++stats.issuedByUnit[fu_idx];
    if (data_idx >= 0) {
        unitBusy[data_idx] = true;
        if (executionPorts)
            executionPorts->commit(IntExecutionFamily::None, 1 << data_idx,
                                   data_idx, curTick());
        ++stats.issuedByUnit[data_idx];
        pairedUnit[fu_idx] = data_idx;
    }

    cpu->recordExecutionAction(inst, executionPorts ? fu_idx : -1);
    if (data_idx >= 0)
        cpu->recordExecutionAction(inst, executionPorts ? data_idx : -1);

    return fu_idx;
}

void
FUPool::freeUnitNextCycle(int fu_idx)
{
    assert(unitBusy[fu_idx]);
    if (pairedUnit[fu_idx] >= 0) {
        unitsToBeFreed.push_back(pairedUnit[fu_idx]);
        pairedUnit[fu_idx] = -1;
    }
    unitsToBeFreed.push_back(fu_idx);
}

void
FUPool::processFreeUnits()
{
    while (!unitsToBeFreed.empty()) {
        int fu_idx = unitsToBeFreed.back();
        unitsToBeFreed.pop_back();

        assert(unitBusy[fu_idx]);

        unitBusy[fu_idx] = false;
    }
}

void
FUPool::dump()
{
    std::cout << "Function Unit Pool (" << name() << ")\n";
    std::cout << "======================================\n";
    std::cout << "Free List:\n";

    for (int i = 0; i < numFU; ++i) {
        if (unitBusy[i]) {
            continue;
        }

        std::cout << "  [" << i << "] : ";

        std::cout << funcUnits[i]->name << " ";

        std::cout << "\n";
    }

    std::cout << "======================================\n";
    std::cout << "Busy List:\n";
    for (int i = 0; i < numFU; ++i) {
        if (!unitBusy[i]) {
            continue;
        }

        std::cout << "  [" << i << "] : ";

        std::cout << funcUnits[i]->name << " ";

        std::cout << "\n";
    }
}

bool
FUPool::isDrained() const
{
    if (std::any_of(pendingPortLoads.begin(), pendingPortLoads.end(),
                    [](unsigned load) { return load != 0; }))
        return false;
    for (auto unit : integerALUs)
        if (unit && !unit->empty())
            return false;
    for (auto resource : integerResources)
        if (resource && !resource->empty())
            return false;
    if (forwarding && !forwarding->empty())
        return false;
    bool is_drained = true;
    for (int i = 0; i < numFU; i++)
        is_drained = is_drained && !unitBusy[i];

    return is_drained;
}

} // namespace o3
} // namespace gem5
