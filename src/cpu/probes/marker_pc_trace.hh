#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include "base/types.hh"

namespace gem5
{
class MarkerPcTrace
{
  public:
    explicit MarkerPcTrace(const std::string &path);
    void initializeTrace();
    void append(Addr pc, Tick commitTick, int64_t fetchTick,
                int32_t dispatchTick, int32_t issueTick,
                int32_t completeTick);
    void finalizeTrace();

  private:
    const std::string outputFile;
    std::ofstream output;
};

// Separate from the six-column committed-PC stream so its existing readers
// retain their format. Records describe retired architectural conditional
// branches; missing direction lookups remain unknown, rather than false.
struct MarkerBranchObservation
{
    uint64_t branchIndex, macroIndex, seqNum;
    uint32_t threadId;
    Addr pc;
    Tick commitTick;
    bool actualTaken, directionValid, originalDirection, predTaken;
    Addr predNextPc, actualNextPc;
    bool directionMispred, nextPcMispred, redirect, commitSquash, btbHit;
};

class MarkerBranchTrace
{
  public:
    explicit MarkerBranchTrace(const std::string &path);
    void initializeTrace();
    void append(const MarkerBranchObservation &observation);
    void finalizeTrace();

  private:
    const std::string outputFile;
    std::ofstream output;
};
} // namespace gem5
