#pragma once
#include <fstream>
#include <string>
#include <vector>

#include "base/types.hh"

namespace gem5
{
// Optional PC trace encoding only. TaoTrace owns population and boundaries.
class MarkerPcTrace
{
  public:
    MarkerPcTrace(Addr imageBase, const std::vector<Addr> &segments,
                  const std::string &path, const std::string &identity);
    ~MarkerPcTrace();
    void initializeTrace();
    void append(Addr pc);
    void finalizeTrace();

  private:
    void flushTraceChunk();
    const Addr traceImageBase;
    const std::vector<Addr> traceExecSegments;
    const std::string traceOutputFile, traceElfSha256;
    std::ofstream *traceOutput = nullptr;
    const unsigned traceChunkRecords = 65536;
    const int traceZstdLevel = 1;
    bool traceFinalized = false;
    std::vector<Addr> tracePcs;
};
} // namespace gem5
