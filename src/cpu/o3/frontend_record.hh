// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_FRONTEND_RECORD_HH__
#define __CPU_O3_FRONTEND_RECORD_HH__

#include <array>
#include <memory>
#include "arch/generic/pcstate.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/allocation_group.hh"
#include "cpu/o3/stage_demand.hh"
#include "cpu/static_inst.hh"
#include "sim/faults.hh"
#include "sim/insttracer.hh"

namespace gem5::o3
{
class CPU;

struct FrontendPrediction
{
    bool taken = false;
    bool directionValid = false;
    bool originalDirection = false;
    bool btbHit = false;
    bool untrackedBranch = false;
};

enum class FrontendSource { Mite, Dsb, Microcode, Lsd };

/** Functional fetch context survives until the IDQ creates the DynInst. */
struct FrontendRecord
{
    StaticInstPtr instruction;
    StaticInstPtr macroop;
    std::unique_ptr<PCStateBase> pc;
    std::unique_ptr<PCStateBase> predictedPC;
    FrontendPrediction prediction;
    InstSeqNum sequence;
    ThreadID thread;
    FrontendSource source;
    Tick fetchedAt;
    bool fromUser = false;
    uint64_t addressSpace = 0;
    std::array<Addr, 2> codeBlocks{};
    unsigned codeBlockCount = 0;
    bool staleCode = false;
    bool notAnInstruction = false;
    bool dsbMiss = false;
    std::shared_ptr<bool> dsbMissExposed;
    Fault fault = NoFault;
    std::unique_ptr<trace::InstRecord> traceData;
    AllocationGroupPtr allocationGroup;
    // These domains do not inherit Rename's macro-fusion accounting.
    StageDemand demand;
    FrontendRecord(CPU *cpu, ThreadID tid, const StaticInstPtr &inst,
                   const StaticInstPtr &macro, const PCStateBase &at,
                   FrontendSource supply, bool trace);
    DynInstPtr materialize(CPU *cpu);
    bool markCodeWrite(ThreadID tid, InstSeqNum after, Addr block)
    {
        if (thread != tid || sequence <= after) return false;
        for (unsigned i = 0; i < codeBlockCount; ++i) {
            if (codeBlocks[i] == block) {
                staleCode = true;
                return true;
            }
        }
        return false;
    }
};

using FrontendRecordPtr = std::unique_ptr<FrontendRecord>;
enum class RepStringKind { None, Copy, Store };
RepStringKind repStringKind(const StaticInst *macro);
enum class NearStackControl {
    None, DirectCall, RegisterCall, MemoryCall, Return, ReturnImmediate
};
NearStackControl nearStackControl(const StaticInst *macro);
StageDemand initialDemand(const StaticInst &instruction,
                          const StaticInst *macro = nullptr);
}
#endif
