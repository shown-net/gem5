#include "cpu/probes/roi_retire_collector.hh"

#include <zstd.h>

#include <algorithm>
#include <array>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/probes/roi_retire.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace
{
constexpr uint64_t RoiRetireExitHandlerId = 8;
constexpr std::array<uint8_t, 8> TraceMagic{
    'R', 'O', 'I', 'F', 'L', 'O', 'W', 0};

template <class T>
void
appendLe(std::vector<uint8_t> &out, T value)
{
    for (size_t index = 0; index < sizeof(T); ++index)
        out.push_back(static_cast<uint8_t>(value >> (8 * index)));
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
    if (!out)
        panic("ROI trace write failed");
}

std::vector<uint8_t>
compressTrace(const std::vector<uint8_t> &raw, int level)
{
    ZSTD_CCtx *context = ZSTD_createCCtx();
    if (!context)
        panic("ROI trace zstd context failed");
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    size_t status = ZSTD_CCtx_setParameter(
        context, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(status))
        status = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    if (!ZSTD_isError(status)) {
        status = ZSTD_compress2(
            context, out.data(), out.size(), raw.data(), raw.size());
    }
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(status))
        panic("ROI trace zstd failed: %s", ZSTD_getErrorName(status));
    out.resize(status);
    return out;
}

std::array<uint8_t, 32>
decodeSha256(const std::string &text)
{
    if (text.size() != 64)
        panic("ROI trace ELF SHA-256 is invalid");
    std::array<uint8_t, 32> identity{};
    for (size_t index = 0; index < identity.size(); ++index) {
        try {
            identity[index] = static_cast<uint8_t>(
                std::stoul(text.substr(index * 2, 2), nullptr, 16));
        } catch (...) {
            panic("ROI trace ELF SHA-256 is invalid");
        }
    }
    return identity;
}
} // namespace

RoiRetireCollector::WindowStats::WindowStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(instructions, statistics::units::Count::get(),
               "Architectural instructions retired in the ROI window")
{
}

RoiRetireCollector::RoiRetireCollector(
    const RoiRetireCollectorParams &params)
    : ProbeListenerObject(params), bodyStartPc(params.body_start_pc),
      endPc(params.end_pc), targetExecStart(params.target_exec_start),
      targetExecEnd(params.target_exec_end),
      windowInsts(params.window_insts.begin(), params.window_insts.end()),
      cpu(dynamic_cast<BaseCPU *>(params.manager)),
      traceChunkRecords(params.trace_chunk_records),
      traceZstdLevel(params.trace_zstd_level), stats(this)
{
    fatal_if(!cpu, "ROI retire collector manager must be a BaseCPU");
    fatal_if(!bodyStartPc || !endPc || bodyStartPc == endPc,
             "ROI body-start and end PCs are invalid");
    fatal_if(targetExecStart >= targetExecEnd,
             "ROI target executable range must be non-empty");
    fatal_if(std::any_of(windowInsts.begin(), windowInsts.end(),
                         [](uint64_t count) { return count == 0; }),
             "ROI retired-instruction window targets must be positive");
    if (!params.trace_output_file.empty()) {
        fatal_if(!traceChunkRecords || traceZstdLevel < -5 ||
                     traceZstdLevel > 22,
                 "ROI trace configuration is invalid");
        traceOutput = new std::ofstream(
            simout.resolve(params.trace_output_file),
            std::ios::binary | std::ios::trunc);
        fatal_if(!traceOutput->good(), "cannot initialize ROI binary trace");
        std::vector<uint8_t> header(TraceMagic.begin(), TraceMagic.end());
        header.push_back(1);
        const auto identity = decodeSha256(params.trace_elf_sha256);
        appendBytes(header, identity.data(), identity.size());
        writeBytes(*traceOutput, header);
        tracePcs.reserve(traceChunkRecords);
    }
}

RoiRetireCollector::~RoiRetireCollector()
{
    if (traceOutput) {
        fatal_if(state != State::Ended,
                 "ROI PC trace finalized before the end boundary");
        finalizeTrace();
    }
}

void
RoiRetireCollector::regProbeListeners()
{
    connectListener<RetireListener>(
        this, "ArchitecturalRetire", &RoiRetireCollector::retire);
}

void
RoiRetireCollector::retire(const ArchitecturalRetireRecord &record)
{
    const Addr pc = record.pc;
    if (state == State::WaitingForBodyStart) {
        if (pc == bodyStartPc && pendingEvent == Event::None)
            signal(Event::Begin);
        return;
    }
    if (state != State::Collecting)
        return;
    if (pc == endPc) {
        finalizeTrace();
        signal(Event::End);
        return;
    }

    ++retired;
    ++stats.instructions;
    if (!isTargetUserRetire(record, targetExecStart, targetExecEnd))
        return;
    ++targetExecUserInstructions;
    if (traceOutput) {
        tracePcs.push_back(pc - targetExecStart);
        if (tracePcs.size() == traceChunkRecords)
            flushTraceChunk();
    }
    if (windows < windowInsts.size() &&
        targetExecUserInstructions >= nextBoundary) {
        ++windows;
        if (windows < windowInsts.size())
            nextBoundary += windowInsts[windows];
        signal(Event::Window);
    }
}

void
RoiRetireCollector::signal(Event event)
{
    if (pendingEvent == Event::End)
        return;
    if (pendingEvent == Event::Window && event == Event::End) {
        pendingEvent = event;
        return;
    }
    if (pendingEvent != Event::None)
        return;
    pendingEvent = event;
    cpu->requestRetireCommitStop();
    const char *kind = event == Event::Begin ? "begin" :
                       event == Event::Window ? "window" : "end";
    exitSimulationLoopNow(RoiRetireExitHandlerId, {{"kind", kind}});
}

void
RoiRetireCollector::acknowledge()
{
    fatal_if(pendingEvent == Event::None,
             "ROI retire collector has no pending event");
    if (pendingEvent == Event::Begin) {
        state = State::Collecting;
        nextBoundary = windowInsts.empty() ? 0 : windowInsts.front();
    } else if (pendingEvent == Event::End) {
        state = State::Ended;
    }
    pendingEvent = Event::None;
}

uint64_t
RoiRetireCollector::retiredInstructions() const
{
    return retired;
}

void
RoiRetireCollector::flushTraceChunk()
{
    if (!traceOutput || tracePcs.empty())
        return;
    std::vector<uint8_t> raw;
    raw.reserve(tracePcs.size() * sizeof(uint64_t));
    for (auto value : tracePcs)
        appendLe<uint64_t>(raw, value);
    const auto compressed = compressTrace(raw, traceZstdLevel);
    std::vector<uint8_t> header{'B', 'L', 'K', 0};
    appendLe<uint32_t>(header, tracePcs.size());
    appendLe<uint32_t>(header, compressed.size());
    writeBytes(*traceOutput, header);
    writeBytes(*traceOutput, compressed);
    tracePcs.clear();
}

void
RoiRetireCollector::finalizeTrace()
{
    if (!traceOutput || traceFinalized)
        return;
    flushTraceChunk();
    std::vector<uint8_t> footer{'E', 'N', 'D', 0};
    writeBytes(*traceOutput, footer);
    traceOutput->close();
    delete traceOutput;
    traceOutput = nullptr;
    traceFinalized = true;
}

} // namespace gem5
