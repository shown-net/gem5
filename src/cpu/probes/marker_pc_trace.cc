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
MarkerPcTrace::append(Addr pc, Tick commitTick, int64_t fetchTick,
                      int32_t dispatchTick, int32_t issueTick,
                      int32_t completeTick)
{
    output << std::hex << pc << '\t' << std::dec << commitTick << '\t'
           << fetchTick << '\t' << dispatchTick << '\t' << issueTick << '\t'
           << completeTick << '\n';
    fatal_if(!output, "cannot append committed PC trace");
}

void
MarkerPcTrace::finalizeTrace()
{
    output.close();
    fatal_if(output.fail(), "cannot close committed PC trace");
}
} // namespace gem5
