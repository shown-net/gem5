#include "cpu/probes/roi_retire_collector.hh"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <limits>

#include "base/logging.hh"
#include "base/output.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace
{
constexpr uint64_t RoiRetireExitHandlerId = 8;
constexpr std::array<uint8_t, 8> TraceMagic{
    'R', 'O', 'I', 'F', 'L', 'W', '3', 0};

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
               "All-privilege architectural instructions retired by the core "
               "between workload-user window boundaries"),
      ADD_STAT(workloadUserInstructions, statistics::units::Count::get(),
               "Owner-address-space CPL3 instructions used for workload "
               "progress")
{
}

RoiRetireCollector::RoiRetireCollector(
    const RoiRetireCollectorParams &params)
    : ProbeListenerObject(params), beginPc(params.begin_pc),
      endPc(params.end_pc), traceImageBase(params.trace_image_base),
      traceExecSegments(params.trace_exec_segments.begin(),
                        params.trace_exec_segments.end()),
      windowInsts(params.window_insts),
      windowSchedule(params.window_schedule.begin(),
                     params.window_schedule.end()),
      cpu(dynamic_cast<BaseCPU *>(params.manager)),
      traceChunkRecords(params.trace_chunk_records),
      traceZstdLevel(params.trace_zstd_level), stats(this)
{
    fatal_if(!cpu, "ROI retire collector manager must be a BaseCPU");
    fatal_if(!beginPc || !endPc || beginPc == endPc,
             "ROI begin and end PCs are invalid");
    fatal_if(!traceImageBase || traceExecSegments.empty() ||
             traceExecSegments.size() % 2,
             "ROI PC trace executable segments are invalid");
    for (size_t index = 0; index < traceExecSegments.size(); index += 2)
        fatal_if(traceExecSegments[index] >= traceExecSegments[index + 1],
                 "ROI PC trace executable segment is invalid");
    fatal_if(windowInsts && !windowSchedule.empty(),
             "ROI fixed window and dynamic schedule are mutually exclusive");
    fatal_if(std::any_of(windowSchedule.begin(), windowSchedule.end(),
                         [](uint64_t count) { return count == 0; }),
             "ROI dynamic window sizes must be positive");
    fatal_if(!windowInsts && windowSchedule.empty() &&
                 params.trace_output_file.empty(),
             "ROI collector requires trace output or a window schedule");
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
    if (state == State::WaitingForBegin) {
        if (pc == beginPc && record.originUser &&
            record.addressSpaceId && pendingEvent == Event::None) {
            ownerAddressSpaceId = record.addressSpaceId;
            signal(Event::Begin);
        }
        return;
    }
    if (state != State::Collecting)
        return;
    if (record.originUser && record.addressSpaceId == ownerAddressSpaceId &&
        pc == endPc) {
        finalizeTrace();
        signal(Event::End);
        return;
    }

    ++retired;
    ++stats.instructions;
    if (!record.originUser || record.addressSpaceId != ownerAddressSpaceId)
        return;
    ++workloadUserInstructions_;
    ++stats.workloadUserInstructions;
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
        if (tracePcs.size() == traceChunkRecords)
            flushTraceChunk();
    }
    if (nextBoundary && workloadUserInstructions_ >= nextBoundary) {
        if (!windowSchedule.empty()) {
            ++windowIndex;
            if (windowIndex < windowSchedule.size()) {
                fatal_if(nextBoundary > std::numeric_limits<uint64_t>::max() -
                             windowSchedule[windowIndex],
                         "ROI dynamic window schedule overflow");
                nextBoundary += windowSchedule[windowIndex];
            } else {
                nextBoundary = 0;
            }
        } else {
            fatal_if(nextBoundary > std::numeric_limits<uint64_t>::max() -
                         windowInsts,
                     "ROI fixed window schedule overflow");
            nextBoundary += windowInsts;
        }
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
        windowIndex = 0;
        nextBoundary =
            !windowSchedule.empty() ? windowSchedule.front() : windowInsts;
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

uint64_t
RoiRetireCollector::workloadUserInstructions() const
{
    return workloadUserInstructions_;
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
