#ifndef __CPU_O3_PROBE_PEREGRINE_TRACE_HH__
#define __CPU_O3_PROBE_PEREGRINE_TRACE_HH__

#include <map>
#include <unordered_map>
#include <vector>

#include "base/output.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/reg_class.hh"
#include "mem/packet.hh"
#include "params/PeregrineTrace.hh"
#include "proto/zstd_protoio.hh"
#include "sim/probe/probe_listener_object.hh"

namespace gem5
{

// PeregrineTrace records every committed instruction between construction and
// simulation exit. It has no internal ROI/window gate: the capture region is
// defined externally, by only instantiating this probe for the desired ROI
// run. Instantiating it for a full-program run traces the entire program.
class PeregrineTrace : public ProbeListenerObject
{
  public:
    PeregrineTrace(const PeregrineTraceParams &p);
    ~PeregrineTrace();

    void regProbeListeners() override;

  private:
    void closeStream();
    void flushChunk();
    void onStatsDump();
    void onExecute(const o3::DynInstPtr &inst);
    void onDataAccess(const o3::DynInstPtr &inst, PacketPtr pkt);
    void onCommit(const o3::DynInstPtr &inst);

    void traceExecute(const o3::DynInstPtr &inst);
    void traceDataAccess(const std::pair<o3::DynInstPtr, PacketPtr> &arg);
    void traceCommit(const o3::DynInstPtr &inst);

    ZstdProtoOutputStream *traceStream;
    const unsigned chunkRecords;
    uint32_t sectionIndex = 0;
    // Memory dependencies are tracked over a finite lookback window (in
    // committed instructions). Writers older than this horizon are evicted,
    // bounding the address map to the recent working set instead of growing
    // with the whole run's footprint.
    const uint64_t memDepLookback;

    struct MemAccessEntry
    {
        Addr addr;
        unsigned size;
    };

    using RegKey = std::pair<ThreadID, RegId>;
    std::map<RegKey, uint64_t> lastRegWriteSequenceId;
    std::unordered_map<Addr, uint64_t> lastMemWriteSequenceId;

    void pruneMemWrites(uint64_t currentKey);

    struct DepSnapshot
    {
        std::vector<uint64_t> regDependencySequenceIds;
        std::vector<MemAccessEntry> readAddrs;
        std::vector<MemAccessEntry> writeAddrs;
        std::vector<uint64_t> memDependencySequenceIds;
    };

    struct MicroOpRecord
    {
        uint32_t id;
        uint32_t classFlags;
        uint32_t fixedExecutionLatency;
        std::vector<uint32_t> deps;
        std::vector<MemAccessEntry> reads;
        std::vector<MemAccessEntry> writes;
    };

    struct TraceRecord
    {
        uint32_t id;
        Addr pc;
        uint32_t classFlags;
        uint32_t branchType;
        bool branchTaken;
        Addr branchTarget;
        uint32_t committedControlOps;
        uint32_t committedMemoryOps;
        uint32_t committedMicroOps;
        uint32_t fixedExecutionLatency;
        uint32_t committedIntAluOps;
        uint32_t committedIntMultOps;
        uint32_t committedIntDivOps;
        uint32_t committedFpAluOps;
        uint32_t committedFpMultDivOps;
        uint32_t committedSimdOps;
        uint32_t committedMatrixOps;
        uint32_t committedLoadOps;
        uint32_t committedStoreOps;
        uint32_t committedSystemOps;
        std::vector<uint32_t> regDeps;
        std::vector<MemAccessEntry> reads;
        std::vector<MemAccessEntry> writes;
        std::vector<uint32_t> memDeps;
        std::vector<MicroOpRecord> microOps;
    };

    struct PendingMacro
    {
        uint64_t dependencyKey = 0;
        Addr pc = 0;
        uint32_t classFlags = 0;
        uint32_t branchType = 0;
        bool branchTaken = false;
        Addr branchTarget = 0;
        uint32_t committedControlOps = 0;
        uint32_t committedMemoryOps = 0;
        uint32_t committedMicroOps = 0;
        uint32_t fixedExecutionLatency = 1;
        uint32_t committedIntAluOps = 0;
        uint32_t committedIntMultOps = 0;
        uint32_t committedIntDivOps = 0;
        uint32_t committedFpAluOps = 0;
        uint32_t committedFpMultDivOps = 0;
        uint32_t committedSimdOps = 0;
        uint32_t committedMatrixOps = 0;
        uint32_t committedLoadOps = 0;
        uint32_t committedStoreOps = 0;
        uint32_t committedSystemOps = 0;
        std::vector<uint64_t> regDependencySequenceIds;
        std::vector<MemAccessEntry> reads;
        std::vector<MemAccessEntry> writes;
        std::vector<uint64_t> memDependencySequenceIds;
        std::vector<uint64_t> microDependencyKeys;
        std::vector<MicroOpRecord> microOps;
    };

    static uint64_t snapshotKey(ThreadID tid, InstSeqNum sn)
    {
        const uint64_t tid_part = static_cast<uint64_t>(tid) << 48;
        const uint64_t sn_part = sn & 0x0000FFFFFFFFFFFFULL;
        return tid_part | sn_part;
    }

    std::unordered_map<uint64_t, uint32_t> emittedRecordIds;
    std::unordered_map<uint64_t, uint32_t> emittedMicroOpIds;
    std::unordered_map<ThreadID, PendingMacro> pendingMacros;
    std::map<uint64_t, DepSnapshot> depSnapshots;
    std::vector<TraceRecord> records;
    uint32_t nextRecordId = 0;
    uint32_t nextMicroOpId = 0;
};

} // namespace gem5

#endif
