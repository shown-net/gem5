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
} // namespace gem5
