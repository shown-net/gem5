#include "cpu/probes/marker_pc_trace.hh"
#include "base/logging.hh"
#include "base/output.hh"

namespace gem5
{
MarkerPcTrace::MarkerPcTrace(const std::string &path) : outputFile(path) {}

void
MarkerPcTrace::initializeTrace()
{
    output.open(simout.resolve(outputFile), std::ios::trunc);
    fatal_if(!output, "cannot initialize committed PC trace");
}

void
MarkerPcTrace::append(Addr pc)
{
    output << std::hex << pc << '\n';
    fatal_if(!output, "cannot append committed PC trace");
}

void
MarkerPcTrace::finalizeTrace()
{
    output.close();
    fatal_if(output.fail(), "cannot close committed PC trace");
}
} // namespace gem5
