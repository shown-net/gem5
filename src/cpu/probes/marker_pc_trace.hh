#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "base/types.hh"
#include "config/have_protobuf.hh"
#if !HAVE_PROTOBUF
#include "base/logging.hh"
#endif

class ProtoOutputStream;
namespace ProtoMessage
{
class MarkerPcBatch;
class MarkerBranchBatch;
}

namespace gem5
{
#if HAVE_PROTOBUF
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
#else
class MarkerPcTrace
{
  public:
    explicit MarkerPcTrace(const std::string &) { fatal("Marker PC trace requires protobuf"); }
    void initializeTrace() {}
    void append(Addr, Tick, int64_t, int32_t, int32_t, int32_t) {}
    void finalizeTrace() {}
};
#endif

} // namespace gem5
