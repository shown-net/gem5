#include "cpu/o3/probe/roi_instruction_trace.hh"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <zstd.h>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/static_inst.hh"

namespace gem5
{
namespace
{
constexpr std::array<uint8_t, 8> Magic{'R', 'O', 'I', 'F', 'L', 'O', 'W', 0};

template <class T>
void
le(std::vector<uint8_t> &out, T value)
{
    for (size_t index = 0; index < sizeof(T); ++index) {
        out.push_back(static_cast<uint8_t>(value >> (8 * index)));
    }
}

void
bytes(std::vector<uint8_t> &out, const uint8_t *data, size_t size)
{
    out.insert(out.end(), data, data + size);
}

void
write(std::ofstream &out, const std::vector<uint8_t> &value)
{
    out.write(reinterpret_cast<const char *>(value.data()), value.size());
    if (!out)
        panic("ROI trace write failed");
}

std::vector<uint8_t>
pack(const std::vector<uint8_t> &raw, int level)
{
    ZSTD_CCtx *context = ZSTD_createCCtx();
    if (!context)
        panic("ROI trace zstd context failed");
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    size_t status = ZSTD_CCtx_setParameter(
        context, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(status)) {
        status = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    }
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
sha(const std::string &text)
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
}

RoiInstructionTrace::RoiInstructionTrace(
    const RoiInstructionTraceParams &params)
    : ProbeListenerObject(params),
      output(new std::ofstream(simout.resolve(params.output_file),
                               std::ios::binary | std::ios::trunc)),
      chunkRecords(params.chunk_records), zstdLevel(params.zstd_level),
      targetExecStart(params.target_exec_start),
      targetExecEnd(params.target_exec_end), startPc(params.start_pc),
      endPc(params.end_pc)
{
    fatal_if(!output->good() || !chunkRecords || zstdLevel < -5 || zstdLevel > 22,
             "cannot initialize ROI binary trace");
    fatal_if(targetExecStart >= targetExecEnd,
             "target executable range is invalid");
    fatal_if(!startPc || !endPc || startPc == endPc,
             "ROI trace start/end PCs are invalid");
    std::vector<uint8_t> header(Magic.begin(), Magic.end());
    header.push_back(1);
    const auto identity = sha(params.elf_sha256);
    bytes(header, identity.data(), identity.size());
    write(*output, header);
    pcs.reserve(chunkRecords);
}

RoiInstructionTrace::~RoiInstructionTrace()
{
    finalize();
}

void
RoiInstructionTrace::regProbeListeners()
{
    connectListener<RetireListener>(
        this, "SystemRetire", &RoiInstructionTrace::retire);
}

void
RoiInstructionTrace::startTracing()
{
    fatal_if(finalized, "ROI instruction trace cannot restart after finalize");
    fatal_if(tracing, "ROI instruction trace is already active");
    tracing = true;
    bodyActive = false;
}

void
RoiInstructionTrace::stopTracing()
{
    if (!tracing)
        return;
    tracing = false;
    finalize();
}

uint64_t
RoiInstructionTrace::recordCount() const
{
    return records;
}

void
RoiInstructionTrace::retire(const SystemRetireRecord &retired)
{
    if (!tracing)
        return;
    if (!bodyActive) {
        if (retired.pc == startPc)
            bodyActive = true;
        return;
    }
    if (retired.pc == endPc) {
        tracing = false;
        finalize();
        return;
    }
    if (retired.cpl != 3)
        return;
    if (retired.pc < targetExecStart || retired.pc >= targetExecEnd)
        return;
    pcs.push_back(retired.pc - targetExecStart);
    ++records;
    if (pcs.size() == chunkRecords)
        flushChunk();
}

void
RoiInstructionTrace::flushChunk()
{
    if (pcs.empty())
        return;
    std::vector<uint8_t> raw;
    raw.reserve(pcs.size() * sizeof(uint64_t));
    for (auto value : pcs)
        le<uint64_t>(raw, value);
    const auto compressed = pack(raw, zstdLevel);
    std::vector<uint8_t> header{'B', 'L', 'K', 0};
    le<uint32_t>(header, pcs.size());
    le<uint32_t>(header, compressed.size());
    write(*output, header);
    write(*output, compressed);
    pcs.clear();
}

void
RoiInstructionTrace::finalize()
{
    if (finalized || !output)
        return;
    fatal_if(tracing, "ROI instruction trace finalized while active");
    flushChunk();
    std::vector<uint8_t> footer{'E', 'N', 'D', 0};
    write(*output, footer);
    output->close();
    delete output;
    output = nullptr;
    finalized = true;
}
} // namespace gem5
