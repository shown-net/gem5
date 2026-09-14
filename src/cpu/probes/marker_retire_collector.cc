#include "cpu/probes/marker_retire_collector.hh"

#include <zstd.h>

#include <array>
#include <limits>

#include "base/logging.hh"
#include "base/output.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace
{
constexpr uint64_t MarkerRetireExitHandlerId = 8;
constexpr std::array<uint8_t, 8> TraceMagic{'P', 'C', 'F', 'L',
                                            'O', 'W', '1', 0};

template <class T>
void
appendLe(std::vector<uint8_t> &out, T value)
{
    for (size_t index = 0; index < sizeof(T); ++index) {
        out.push_back(static_cast<uint8_t>(value >> (8 * index)));
    }
}

void
appendBytes(std::vector<uint8_t> &out, const uint8_t *data, size_t size)
{
    out.insert(out.end(), data, data + size);
}

void
writeBytes(std::ofstream &out, const std::vector<uint8_t> &value)
{
    out.write(reinterpret_cast<const char *>(value.data()), value.size());
    if (!out) {
        panic("PC trace write failed");
    }
}

std::vector<uint8_t>
compressTrace(const std::vector<uint8_t> &raw, int level)
{
    ZSTD_CCtx *context = ZSTD_createCCtx();
    if (!context) {
        panic("PC trace zstd context failed");
    }
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    size_t status =
        ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(status)) {
        status = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    }
    if (!ZSTD_isError(status)) {
        status = ZSTD_compress2(context, out.data(), out.size(), raw.data(),
                                raw.size());
    }
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(status)) {
        panic("PC trace zstd failed: %s", ZSTD_getErrorName(status));
    }
    out.resize(status);
    return out;
}

std::array<uint8_t, 32>
decodeSha256(const std::string &text)
{
    if (text.size() != 64) {
        panic("PC trace ELF SHA-256 is invalid");
    }
    std::array<uint8_t, 32> identity{};
    for (size_t index = 0; index < identity.size(); ++index) {
        try {
            identity[index] = static_cast<uint8_t>(
                std::stoul(text.substr(index * 2, 2), nullptr, 16));
        } catch (...) {
            panic("PC trace ELF SHA-256 is invalid");
        }
    }
    return identity;
}
} // namespace

MarkerRetireCollector::MarkerRetireCollector(
    const MarkerRetireCollectorParams &params)
    : ProbeListenerObject(params),
      markerPc(params.marker_pc),
      traceImageBase(params.trace_image_base),
      traceExecSegments(params.trace_exec_segments.begin(),
                        params.trace_exec_segments.end()),
      cpu(dynamic_cast<BaseCPU *>(params.manager)),
      traceOutputFile(params.trace_output_file),
      traceElfSha256(params.trace_elf_sha256),
      traceChunkRecords(params.trace_chunk_records),
      traceZstdLevel(params.trace_zstd_level)
{
    fatal_if(!cpu, "marker retire collector manager must be a BaseCPU");
    fatal_if(!markerPc, "marker retire collector requires a marker PC");
    if (!traceOutputFile.empty()) {
        fatal_if(!traceImageBase || traceExecSegments.empty() ||
                     traceExecSegments.size() % 2,
                 "PC trace executable segments are invalid");
        for (size_t index = 0; index < traceExecSegments.size(); index += 2) {
            fatal_if(traceExecSegments[index] >= traceExecSegments[index + 1],
                     "PC trace executable segment is invalid");
        }
        fatal_if(!traceChunkRecords || traceZstdLevel < -5 ||
                     traceZstdLevel > 22,
                 "PC trace configuration is invalid");
        decodeSha256(traceElfSha256);
    }
}

MarkerRetireCollector::~MarkerRetireCollector()
{
    if (traceOutput) {
        fatal_if(state != State::Ended,
                 "PC trace finalized before the end marker");
        finalizeTrace();
    }
}

void
MarkerRetireCollector::regProbeListeners()
{
    connectListener<RetireListener>(this, "ArchitecturalRetire",
                                    &MarkerRetireCollector::retire);
}

void
MarkerRetireCollector::retire(const ArchitecturalRetireRecord &record)
{
    const Addr pc = record.pc;
    if (state == State::WaitingForBegin) {
        if (pc == markerPc && record.originUser && record.addressSpaceId &&
            pendingEvent == Event::None) {
            ownerAddressSpaceId = record.addressSpaceId;
            signal(Event::Begin);
        }
        return;
    }
    if (state != State::Collecting) {
        return;
    }
    if (!record.originUser || record.addressSpaceId != ownerAddressSpaceId) {
        return;
    }
    if (traceOutput) {
        bool targetPc = false;
        for (size_t index = 0; index < traceExecSegments.size(); index += 2) {
            if (pc >= traceExecSegments[index] &&
                pc < traceExecSegments[index + 1]) {
                targetPc = true;
                break;
            }
        }
        tracePcs.push_back(targetPc ? pc - traceImageBase
                                    : std::numeric_limits<uint64_t>::max());
        if (tracePcs.size() == traceChunkRecords) {
            flushTraceChunk();
        }
    }
    if (pc == markerPc) {
        finalizeTrace();
        signal(Event::End);
        return;
    }
}

void
MarkerRetireCollector::signal(Event event)
{
    if (pendingEvent == Event::End) {
        return;
    }
    if (pendingEvent != Event::None) {
        return;
    }
    pendingEvent = event;
    cpu->requestRetireCommitStop();
    const char *kind = event == Event::Begin ? "begin" : "end";
    exitSimulationLoopNow(MarkerRetireExitHandlerId, {{"kind", kind}});
}

void
MarkerRetireCollector::acknowledge()
{
    fatal_if(pendingEvent == Event::None,
             "marker retire collector has no pending event");
    if (pendingEvent == Event::Begin) {
        initializeTrace();
        state = State::Collecting;
    } else if (pendingEvent == Event::End) {
        state = State::Ended;
    }
    pendingEvent = Event::None;
}

void
MarkerRetireCollector::initializeTrace()
{
    if (traceOutputFile.empty() || traceOutput) {
        return;
    }
    traceOutput = new std::ofstream(simout.resolve(traceOutputFile),
                                    std::ios::binary | std::ios::trunc);
    fatal_if(!traceOutput->good(), "cannot initialize PC trace");
    std::vector<uint8_t> header(TraceMagic.begin(), TraceMagic.end());
    header.push_back(1);
    const auto identity = decodeSha256(traceElfSha256);
    appendBytes(header, identity.data(), identity.size());
    writeBytes(*traceOutput, header);
    tracePcs.reserve(traceChunkRecords);
}

void
MarkerRetireCollector::flushTraceChunk()
{
    if (!traceOutput || tracePcs.empty()) {
        return;
    }
    std::vector<uint8_t> raw;
    raw.reserve(tracePcs.size() * sizeof(uint64_t));
    for (auto value : tracePcs) {
        appendLe<uint64_t>(raw, value);
    }
    const auto compressed = compressTrace(raw, traceZstdLevel);
    std::vector<uint8_t> header{'B', 'L', 'K', 0};
    appendLe<uint32_t>(header, tracePcs.size());
    appendLe<uint32_t>(header, compressed.size());
    writeBytes(*traceOutput, header);
    writeBytes(*traceOutput, compressed);
    tracePcs.clear();
}

void
MarkerRetireCollector::finalizeTrace()
{
    if (!traceOutput || traceFinalized) {
        return;
    }
    flushTraceChunk();
    std::vector<uint8_t> footer{'E', 'N', 'D', 0};
    writeBytes(*traceOutput, footer);
    traceOutput->close();
    delete traceOutput;
    traceOutput = nullptr;
    traceFinalized = true;
}

} // namespace gem5
