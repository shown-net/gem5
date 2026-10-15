#pragma once
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
    void append(Addr pc);
    void finalizeTrace();

  private:
    const std::string outputFile;
    std::ofstream output;
};
} // namespace gem5
