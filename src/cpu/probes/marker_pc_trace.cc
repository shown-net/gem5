#include "cpu/probes/marker_pc_trace.hh"

#include <zstd.h>

#include <array>
#include <limits>

#include "base/logging.hh"
#include "base/output.hh"

namespace gem5
{
namespace
{
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

MarkerPcTrace::MarkerPcTrace(Addr imageBase, const std::vector<Addr> &segments,
                             const std::string &path,
                             const std::string &identity)
    : traceImageBase(imageBase),
      traceExecSegments(segments),
      traceOutputFile(path),
      traceElfSha256(identity)
{
    if (!path.empty()) {
        fatal_if(!imageBase || segments.empty() || segments.size() % 2,
                 "invalid Marker PC trace executable segments");
        decodeSha256(identity);
    }
}
MarkerPcTrace::~MarkerPcTrace()
{
    if (traceOutput) {
        finalizeTrace();
    }
}
void
MarkerPcTrace::append(Addr pc)
{
    if (!traceOutput) {
        return;
    }
    bool target = false;
    for (size_t i = 0; i < traceExecSegments.size(); i += 2) {
        if (pc >= traceExecSegments[i] && pc < traceExecSegments[i + 1]) {
            target = true;
            break;
        }
    }
    tracePcs.push_back(target ? pc - traceImageBase
                              : std::numeric_limits<uint64_t>::max());
    if (tracePcs.size() == traceChunkRecords) {
        flushTraceChunk();
    }
}

void
MarkerPcTrace::initializeTrace()
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
MarkerPcTrace::flushTraceChunk()
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
MarkerPcTrace::finalizeTrace()
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
