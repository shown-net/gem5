// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_O3_FRONTEND_RECORD_HH__
#define __CPU_O3_FRONTEND_RECORD_HH__

#include <memory>
#include "arch/generic/pcstate.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
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
};

enum class FrontendSource { Mite, Dsb, Rom };

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
    bool notAnInstruction = false;
    Fault fault = NoFault;
    std::unique_ptr<trace::InstRecord> traceData;

    FrontendRecord(CPU *cpu, ThreadID tid, const StaticInstPtr &inst,
                   const StaticInstPtr &macro, const PCStateBase &at,
                   FrontendSource supply, bool trace);
    DynInstPtr materialize(CPU *cpu);
};

using FrontendRecordPtr = std::unique_ptr<FrontendRecord>;
}
#endif
