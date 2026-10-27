#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "base/types.hh"

class ProtoOutputStream;
namespace ProtoMessage
{
class MarkerPcBatch;
class MarkerBranchBatch;
}

namespace gem5
{
class MarkerPcTrace
{
  public:
    explicit MarkerPcTrace(const std::string &path);
    ~MarkerPcTrace();
    void initializeTrace();
    void append(Addr pc, Tick commitTick, int64_t fetchTick,
                int32_t dispatchTick, int32_t issueTick,
                int32_t completeTick);
    void finalizeTrace();

  private:
    void flush();
    static constexpr uint32_t BatchSize = 4096;
    const std::string outputFile;
    std::unique_ptr<ProtoOutputStream> output;
    std::unique_ptr<ProtoMessage::MarkerPcBatch> batch;
};

// Branch records use a separate protobuf stream. They describe retired
// architectural conditional branches; missing direction lookups remain
// unknown, rather than false.
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
    ~MarkerBranchTrace();
    void initializeTrace();
    void append(const MarkerBranchObservation &observation);
    void finalizeTrace();

  private:
    void flush();
    static constexpr uint32_t BatchSize = 4096;
    const std::string outputFile;
    std::unique_ptr<ProtoOutputStream> output;
    std::unique_ptr<ProtoMessage::MarkerBranchBatch> batch;
};
} // namespace gem5
