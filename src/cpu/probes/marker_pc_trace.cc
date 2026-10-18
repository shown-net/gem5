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

MarkerBranchTrace::MarkerBranchTrace(const std::string &path)
    : outputFile(path)
{}

void
MarkerBranchTrace::initializeTrace()
{
    output.open(simout.resolve(outputFile), std::ios::trunc);
    fatal_if(!output, "cannot initialize committed branch trace");
}

void
MarkerBranchTrace::append(const MarkerBranchObservation &b)
{
    output << std::boolalpha << "{\"schema\":1,\"branch_index\":"
           << b.branchIndex << ",\"macro_index\":" << b.macroIndex
           << ",\"seq_num\":" << b.seqNum
           << ",\"thread_id\":" << b.threadId << ",\"pc\":\"0x"
           << std::hex << b.pc << std::dec << "\",\"commit_tick\":"
           << b.commitTick << ",\"actual_taken\":" << b.actualTaken
           << ",\"direction_valid\":" << b.directionValid
           << ",\"original_direction\":";
    if (b.directionValid) output << b.originalDirection;
    else output << "null";
    output << ",\"pred_taken\":" << b.predTaken
           << ",\"pred_next_pc\":\"0x" << std::hex << b.predNextPc
           << "\",\"actual_next_pc\":\"0x" << b.actualNextPc << std::dec
           << "\",\"direction_mispred\":";
    if (b.directionValid) output << b.directionMispred;
    else output << "null";
    output << ",\"next_pc_mispred\":" << b.nextPcMispred
           << ",\"redirect\":" << b.redirect
           << ",\"commit_squash\":" << b.commitSquash
           << ",\"btb_hit\":" << b.btbHit << "}\n";
    fatal_if(!output, "cannot append committed branch trace");
}

void
MarkerBranchTrace::finalizeTrace()
{
    output.close();
    fatal_if(output.fail(), "cannot close committed branch trace");
}
} // namespace gem5
