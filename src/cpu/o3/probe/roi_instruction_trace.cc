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
      loadBias(params.load_bias), endPc(params.end_pc)
{
    fatal_if(!output->good() || !chunkRecords || zstdLevel < -5 || zstdLevel > 22,
             "cannot initialize ROI binary trace");
    std::vector<uint8_t> header(Magic.begin(), Magic.end());
    header.push_back(1);
    header.push_back(1);
    le<uint32_t>(header, chunkRecords);
    const auto identity = sha(params.elf_sha256);
    bytes(header, identity.data(), identity.size());
    write(*output, header);
    pcs.reserve(chunkRecords);
    flags.reserve(chunkRecords);
    sizes.reserve(chunkRecords);
}

RoiInstructionTrace::~RoiInstructionTrace()
{
    finalize();
}

void
RoiInstructionTrace::regProbeListeners()
{
    connectListener<RetireListener>(
        this, "ArchitecturalRetire", &RoiInstructionTrace::retire);
}

void
RoiInstructionTrace::startTracing()
{
    fatal_if(finalized, "ROI instruction trace cannot restart after finalize");
    fatal_if(tracing, "ROI instruction trace is already active");
    tracing = true;
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
RoiInstructionTrace::retire(const std::pair<StaticInstPtr, Addr> &retired)
{
    if (!tracing || retired.second == endPc)
        return;
    const auto pc = retired.second - loadBias;
    pcs.push_back(pc);
    flags.push_back(retired.first->flagsValue());
    sizes.push_back(retired.first->size());
    ++records;
    if (pcs.size() == chunkRecords)
        flushChunk();
}

void
RoiInstructionTrace::flushChunk()
{
    if (pcs.empty())
        return;
    fatal_if(pcs.size() != flags.size() || pcs.size() != sizes.size(),
             "ROI trace columns lost alignment");
    struct Section
    {
        uint8_t id;
        std::vector<uint8_t> raw;
        std::vector<uint8_t> compressed;
    };
    std::vector<Section> sections;
    auto add = [&](uint8_t id) -> Section & {
        sections.push_back({id, {}, {}});
        return sections.back();
    };
    auto &pc = add(1);
    for (auto value : pcs)
        le<uint64_t>(pc.raw, value);
    auto &size = add(2);
    for (auto value : sizes)
        size.raw.push_back(value);
    auto &gem5Flags = add(4);
    for (auto value : flags)
        le<uint64_t>(gem5Flags.raw, value);
    for (auto &section : sections)
        section.compressed = pack(section.raw, zstdLevel);
    std::vector<uint8_t> header{'B', 'L', 'K', 0};
    le<uint64_t>(header, records - pcs.size());
    le<uint32_t>(header, pcs.size());
    header.push_back(sections.size());
    for (const auto &section : sections) {
        header.push_back(section.id);
        le<uint32_t>(header, section.raw.size());
        le<uint32_t>(header, section.compressed.size());
    }
    write(*output, header);
    for (const auto &section : sections)
        write(*output, section.compressed);
    ++chunks;
    pcs.clear();
    flags.clear();
    sizes.clear();
}

void
RoiInstructionTrace::finalize()
{
    if (finalized || !output)
        return;
    fatal_if(tracing, "ROI instruction trace finalized while active");
    flushChunk();
    std::vector<uint8_t> footer{'E', 'N', 'D', 0};
    le<uint64_t>(footer, records);
    le<uint64_t>(footer, chunks);
    write(*output, footer);
    output->close();
    delete output;
    output = nullptr;
    finalized = true;
}
} // namespace gem5
