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

#include <sstream>
#include <map>
#include <set>

#include "cpu/func_unit.hh"

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
FUPool::AllocationStats::AllocationStats(statistics::Group *parent)
    : statistics::Group(parent, "custom"),
      ADD_STAT(allocatedByUnit, statistics::units::Count::get(),
               "Successful FU allocations by pool-local instance and op class"),
      ADD_STAT(timingIssued, statistics::units::Count::get(),
               "Successful per-form timing admissions"),
      ADD_STAT(serviceRejected, statistics::units::Count::get(),
               "Per-form attempts rejected by a shared service")
{
}

FUPool::FUPool(const Params &p)
    : SimObject(p), allocationStats(this)
{
    numFU = 0;

    funcUnits.clear();

    maxOpLatencies.fill(Cycles(0));
    pipelined.fill(true);
    std::map<std::string, int> namedPorts;

    //
    //  Iterate through the list of FUDescData structures
    //
    for (FUDesc *i : p.FUList) {
        //
        //  Don't bother with this if we're not going to create any FU's
        //
        if (i->number) {
            const auto &port = i->timingPort;
            if (!port.empty()) {
                fatal_if(i->number != 1 || namedPorts.count(port),
                         "Timing ports must name unique single FU instances");
                namedPorts[port] = numFU;
            }
            //
            //  Create the FuncUnit object from this structure
            //   - add the capabilities listed in the FU's operation
            //     description
            //
            //  We create the first unit, then duplicate it as needed
            //
            FuncUnit *fu = new FuncUnit;

            for (OpDesc *j : i->opDescList) {
                // indicate that this pool has this capability
                capabilityList.set(j->opClass);

                // Add each of the FU's that will have this capability to the
                // appropriate queue.
                for (int k = 0; k < i->number; ++k)
                    fuPerCapList[j->opClass].addFU(numFU + k);

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

            for (int c = 1; c < i->number; ++c) {
                std::ostringstream s;
                numFU++;
                FuncUnit *fu2 = new FuncUnit(*fu);

                s << i->name() << "(" << c << ")";
                fu2->name = s.str();
                funcUnits.push_back(fu2);
            }
        }
    }

    unitBusy.resize(numFU);
    std::map<std::string, int> units;
    for (const auto &name : p.timingUnits) {
        fatal_if(name.empty() || units.count(name), "Invalid timing unit name");
        units[name] = nextAccept.size();
        nextAccept.emplace_back(0);
    }
    for (const auto *entry : p.instructionTimings) {
        auto &binding = timingBindings[unsigned(entry->form)];
        fatal_if(binding.entry, "Duplicate instruction timing form");
        binding.entry = entry;
        std::set<std::string> seen;
        for (const auto &port : entry->ports) {
            fatal_if(!namedPorts.count(port) || !seen.insert(port).second,
                     "Unknown or duplicate timing port: %s", port);
            binding.ports.push_back(namedPorts.at(port));
        }
        if (!entry->unit.empty()) {
            fatal_if(!units.count(entry->unit), "Unknown timing unit: %s", entry->unit);
            binding.unit = units.at(entry->unit);
        }
    }
    allocationStats.timingIssued.init(unsigned(InstructionForm::Count) - 1);
    allocationStats.serviceRejected.init(unsigned(InstructionForm::Count) - 1);
    const char *forms[] = {"add64", "imul64", "divsd"};
    for (unsigned i = 0; i < unsigned(InstructionForm::Count) - 1; ++i) {
        allocationStats.timingIssued.subname(i, forms[i]);
        allocationStats.serviceRejected.subname(i, forms[i]);
    }
    allocationStats.allocatedByUnit.init(numFU, Num_OpClasses)
        .flags(statistics::total);
    allocationStats.allocatedByUnit.ysubnames(enums::OpClassStrings);
    for (int i = 0; i < numFU; ++i)
        allocationStats.allocatedByUnit.subname(i, "fu" + std::to_string(i));

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

int
FUPool::getUnit(OpClass capability)
{
    //  If this pool doesn't have the specified capability,
    //  return this information to the caller
    if (!capabilityList[capability])
        return NoCapableFU;

    int fu_idx = fuPerCapList[capability].getFU();
    int start_idx = fu_idx;

    // Iterate through the circular queue if needed, stopping if we've reached
    // the first element again.
    while (unitBusy[fu_idx]) {
        fu_idx = fuPerCapList[capability].getFU();
        if (fu_idx == start_idx) {
            // No FU available
            return NoFreeFU;
        }
    }

    assert(fu_idx < numFU);

    unitBusy[fu_idx] = true;

    return fu_idx;
}

void
FUPool::freeUnitNextCycle(int fu_idx)
{
    assert(unitBusy[fu_idx]);
    unitsToBeFreed.push_back(fu_idx);
}

int
FUPool::getUnit(const InstructionTiming &entry, OpClass capability, Cycles now)
{
    auto &binding = timingBindings[unsigned(entry.form)];
    assert(binding.entry == &entry);
    if (binding.unit >= 0 && now < nextAccept[binding.unit]) {
        allocationStats.serviceRejected[unsigned(entry.form) - 1]++;
        return NoFreeFU;
    }
    for (unsigned n = 0; n < binding.ports.size(); ++n) {
        const unsigned pos = (binding.cursor + n) % binding.ports.size();
        const int idx = binding.ports[pos];
        if (unitBusy[idx]) continue;
        // Commit both constraints only after all have passed.
        unitBusy[idx] = true;
        binding.cursor = (pos + 1) % binding.ports.size();
        if (binding.unit >= 0)
            nextAccept[binding.unit] = now + entry.issueInterval;
        allocationStats.allocatedByUnit[idx][capability]++;
        allocationStats.timingIssued[unsigned(entry.form) - 1]++;
        return idx;
    }
    return NoFreeFU;
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
    bool is_drained = true;
    for (int i = 0; i < numFU; i++)
        is_drained = is_drained && !unitBusy[i];

    return is_drained;
}

} // namespace o3
} // namespace gem5
