#ifndef __MEM_TAOTRACE_OBSERVATION_HH__
#define __MEM_TAOTRACE_OBSERVATION_HH__

#include <variant>

#include "mem/taotrace_response.hh"

namespace gem5
{

// Read-only observations from the owner of the Marker/native lifecycle.
// Commit precedes its load outcome; End follows all final load outcomes.
struct TaoTraceMemoryBoundary
{
    bool begin;
};

struct TaoTraceMemoryCommit
{
    uint32_t context;
    uint64_t seq;
    bool first, last, load, special;
};

struct TaoTraceMemoryLoadOutcome
{
    uint32_t context;
    uint64_t seq;
    bool resolved;
    TaoTraceNativeAccessRegistry::Snapshot native;
};

using TaoTraceMemoryObservation = std::variant<
    TaoTraceMemoryBoundary, TaoTraceMemoryCommit, TaoTraceMemoryLoadOutcome>;

} // namespace gem5

#endif
