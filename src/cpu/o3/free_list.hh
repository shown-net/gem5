/*
 * Copyright (c) 2016-2018 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder. You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#ifndef __CPU_O3_FREE_LIST_HH__
#define __CPU_O3_FREE_LIST_HH__

#include <algorithm>
#include <array>
#include <iostream>
#include <queue>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/regfile.hh"
#include "debug/FreeList.hh"

namespace gem5
{

namespace o3
{

class UnifiedRenameMap;

/**
 * Free list for a single class of registers (e.g., integer
 * or floating point).  Because the register class is implicitly
 * determined by the rename map instance being accessed, all
 * architectural register index parameters and values in this class
 * are relative (e.g., %fp2 is just index 2).
 */
class SimpleFreeList
{
  private:

    /** The actual free list */
    std::queue<PhysRegIdPtr> freeRegs;
    // An entry remains occupied until all its data fragments are reusable.
    // Rename aliases are counted by PhysRegId before reaching this layer.
    std::vector<int> regEntries;
    std::vector<unsigned> entryUsers;
    std::queue<unsigned> freeEntries;
    std::vector<bool> entryWide;
    unsigned freeWideEntries = 0;

  public:

    SimpleFreeList() {};

    void
    initEntries(unsigned entries, unsigned wide_entries = 0)
    {
        if (!entries)
            return;
        regEntries.assign(freeRegs.size(), -1);
        entryUsers.assign(entries, 0);
        panic_if(wide_entries > entries,
                 "Wide PRF capability exceeds the shared entry budget");
        entryWide.assign(entries, false);
        freeWideEntries = wide_entries;
        for (unsigned i = 0; i < entries; ++i)
            freeEntries.push(i);
    }

    /** Add a physical register to the free list */
    bool addReg(PhysRegIdPtr reg)
    {
        // Initial population has no rename owner. Otherwise a physical
        // register is reusable only after every architectural version/alias
        // has left the rename history.
        if (!reg->numRenameReferences() || reg->releaseRenameReference()) {
            if (!regEntries.empty()) {
                const int entry = regEntries[reg->index()];
                if (entry >= 0) {
                    assert(entryUsers[entry]);
                    if (--entryUsers[entry] == 0) {
                        if (entryWide[entry]) {
                            entryWide[entry] = false;
                            ++freeWideEntries;
                        }
                        freeEntries.push(entry);
                    }
                    regEntries[reg->index()] = -1;
                }
            }
            freeRegs.push(reg);
            return true;
        }
        return false;
    }

    /** Add physical registers to the free list */
    template<class InputIt>
    void
    addRegs(InputIt first, InputIt last) {
        std::for_each(first, last, [this](typename InputIt::value_type& reg) {
            freeRegs.push(&reg);
        });
    }

    /** Get the next available register from the free list */
    PhysRegIdPtr getReg(PhysRegIdPtr entry_peer = nullptr)
    {
        assert(!freeRegs.empty());
        PhysRegIdPtr free_reg = freeRegs.front();
        freeRegs.pop();
        free_reg->allocateRenameReference();
        if (!regEntries.empty()) {
            unsigned entry;
            if (entry_peer) {
                const int peer_entry = regEntries[entry_peer->index()];
                assert(peer_entry >= 0);
                entry = peer_entry;
            } else {
                assert(!freeEntries.empty());
                entry = freeEntries.front();
                freeEntries.pop();
            }
            assert(regEntries[free_reg->index()] == -1);
            regEntries[free_reg->index()] = entry;
            ++entryUsers[entry];
        }
        return free_reg;
    }

    /** Return the number of free registers on the list. */
    unsigned numFreeRegs() const { return freeRegs.size(); }
    unsigned numFreeRegEntries() const
    {
        return regEntries.empty() ? numFreeRegs() : freeEntries.size();
    }

    unsigned numFreeWideEntries() const { return freeWideEntries; }
    void markWide(PhysRegIdPtr reg)
    {
        panic_if(regEntries.empty(), "Wide PRF demand needs entry accounting");
        const int entry = regEntries.at(reg->index());
        assert(entry >= 0);
        if (!entryWide[entry]) {
            panic_if(!freeWideEntries, "Wide PRF entry without reservation");
            --freeWideEntries;
            entryWide[entry] = true;
        }
    }

    /** True iff there are free registers on the list. */
    bool hasFreeRegs() const { return !freeRegs.empty(); }
};


/**
 * FreeList class that simply holds the list of free integer and floating
 * point registers.  Can request for a free register of either type, and
 * also send back free registers of either type.  This is a very simple
 * class, but it should be sufficient for most implementations.  Like all
 * other classes, it assumes that the indices for the floating point
 * registers starts after the integer registers end.  Hence the variable
 * numPhysicalIntRegs is logically equivalent to the baseFP dependency.
 * Note that while this most likely should be called FreeList, the name
 * "FreeList" is used in a typedef within the CPU Policy, and therefore no
 * class can be named simply "FreeList".
 * @todo: Give a better name to the base FP dependency.
 */
class UnifiedFreeList
{
  private:

    /** The object name, for DPRINTF.  We have to declare this
     *  explicitly because Scoreboard is not a SimObject. */
    const std::string _name;

    std::array<SimpleFreeList, CCRegClass + 1> freeLists;

    /**
     * The register file object is used only to distinguish integer
     * from floating-point physical register indices.
     */
    PhysRegFile *regFile;

    /*
     * We give UnifiedRenameMap internal access so it can get at the
     * internal per-class free lists and associate those with its
     * per-class rename maps. See UnifiedRenameMap::init().
     */
    friend class UnifiedRenameMap;

  public:
    /** Constructs a free list.
     *  @param _numPhysicalIntRegs Number of physical integer registers.
     *  @param reservedIntRegs Number of integer registers already
     *                         used by initial mappings.
     *  @param _numPhysicalFloatRegs Number of physical fp registers.
     *  @param reservedFloatRegs Number of fp registers already
     *                           used by initial mappings.
     */
    UnifiedFreeList(const std::string &_my_name, PhysRegFile *_regFile,
                    unsigned float_entries = 0, unsigned float_wide_entries = 0);

    /** Gives the name of the freelist. */
    std::string name() const { return _name; };

    /** Gets a free register of type type. */
    PhysRegIdPtr getReg(RegClassType type, PhysRegIdPtr entry_peer = nullptr)
    {
        return freeLists[type].getReg(entry_peer);
    }

    unsigned numFreeFloatEntries() const
    {
        return freeLists[FloatRegClass].numFreeRegEntries();
    }
    unsigned numFreeFloatWideEntries() const
    { return freeLists[FloatRegClass].numFreeWideEntries(); }
    void markFloatWide(PhysRegIdPtr reg)
    { freeLists[FloatRegClass].markWide(reg); }

    /** Adds a register back to the free list. */
    template<class InputIt>
    void
    addRegs(InputIt first, InputIt last)
    {
        std::for_each(first, last, [this](auto &reg) { addReg(&reg); });
    }

    /** Adds a register back to the free list. */
    bool
    addReg(PhysRegIdPtr freed_reg)
    {
        return freeLists[freed_reg->classValue()].addReg(freed_reg);
    }

    /** Checks if there are any free registers of type type. */
    bool
    hasFreeRegs(RegClassType type) const
    {
        return freeLists[type].hasFreeRegs();
    }

    /** Returns the number of free registers of type type. */
    unsigned
    numFreeRegs(RegClassType type) const
    {
        return freeLists[type].numFreeRegs();
    }
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FREE_LIST_HH__
