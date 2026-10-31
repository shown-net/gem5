#include "cpu/probes/marker_pc_trace.hh"
#include "base/logging.hh"
#include "base/output.hh"
#include "proto/marker_trace.pb.h"
#include "proto/protoio.hh"
#include "sim/core.hh"

namespace gem5
{
MarkerPcTrace::MarkerPcTrace(const std::string &path) : outputFile(path) {}
MarkerPcTrace::~MarkerPcTrace() = default;

void
MarkerPcTrace::initializeTrace()
{
    output = std::make_unique<ProtoOutputStream>(simout.resolve(outputFile));
    ProtoMessage::MarkerTraceHeader header;
    header.set_version(1);
    header.set_trace_kind("committed-pc");
    header.set_tick_freq(sim_clock::Frequency);
    header.set_batch_size(BatchSize);
    output->write(header);
    batch = std::make_unique<ProtoMessage::MarkerPcBatch>();
}

void
MarkerPcTrace::append(Addr pc, Tick commitTick, int64_t fetchTick,
                      int32_t dispatchTick, int32_t issueTick,
                      int32_t completeTick)
{
    batch->add_pc(pc);
    batch->add_commit_tick(commitTick);
    batch->add_fetch_tick(fetchTick);
    batch->add_dispatch_from_fetch(dispatchTick);
    batch->add_issue_from_fetch(issueTick);
    batch->add_complete_from_fetch(completeTick);
    if (batch->pc_size() >= BatchSize)
        flush();
}

void
MarkerPcTrace::flush()
{
    if (!batch || batch->pc_size() == 0)
        return;
    batch->set_row_count(batch->pc_size());
    output->write(*batch);
    batch->Clear();
}

void
MarkerPcTrace::finalizeTrace()
{
    flush();
    batch.reset();
    output.reset();
}

} // namespace gem5
