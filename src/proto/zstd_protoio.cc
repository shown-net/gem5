#include "proto/zstd_protoio.hh"

#include <zstd.h>

#include "base/logging.hh"

namespace gem5
{

namespace
{
void
writeVarint(std::vector<uint8_t> &bytes, uint32_t value)
{
    while (value >= 0x80) {
        bytes.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    bytes.push_back(static_cast<uint8_t>(value));
}
}

ZstdProtoOutputStream::ZstdProtoOutputStream(
    const std::string &filename, int compression_level)
    : file(filename, std::ios::binary | std::ios::trunc), stream(nullptr),
      closed(false)
{
    if (!file.good())
        panic("Could not open %s for zstd trace output", filename);
    stream = ZSTD_createCStream();
    if (!stream || ZSTD_isError(ZSTD_CCtx_reset(
                       static_cast<ZSTD_CCtx *>(stream),
                       ZSTD_reset_session_and_parameters)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(
            static_cast<ZSTD_CCtx *>(stream),
            ZSTD_c_compressionLevel, compression_level)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(
            static_cast<ZSTD_CCtx *>(stream), ZSTD_c_checksumFlag, 1)))
        panic("Unable to initialize zstd trace stream");
}

ZstdProtoOutputStream::~ZstdProtoOutputStream()
{
    close();
}

void
ZstdProtoOutputStream::write(const void *data, size_t size, bool end_frame)
{
    ZSTD_inBuffer input{data, size, 0};
    std::vector<char> output(ZSTD_CStreamOutSize());
    do {
        ZSTD_outBuffer out{output.data(), output.size(), 0};
        const size_t remaining = ZSTD_compressStream2(
            static_cast<ZSTD_CStream *>(stream), &out, &input,
            end_frame ? ZSTD_e_end : ZSTD_e_continue);
        if (ZSTD_isError(remaining))
            panic("zstd trace write failed: %s", ZSTD_getErrorName(remaining));
        file.write(output.data(), out.pos);
        if (!file.good())
            panic("zstd trace file write failed");
        if (end_frame && input.pos == input.size && remaining == 0)
            break;
    } while (input.pos < input.size || end_frame);
}

void
ZstdProtoOutputStream::writeDelimited(const google::protobuf::Message &message)
{
    const size_t payload_size = message.ByteSizeLong();
    if (payload_size > UINT32_MAX)
        panic("zstd trace protobuf message too large");
    std::vector<uint8_t> bytes;
    bytes.reserve(payload_size + 5);
    writeVarint(bytes, static_cast<uint32_t>(payload_size));
    const size_t prefix = bytes.size();
    bytes.resize(prefix + payload_size);
    if (!message.SerializeToArray(bytes.data() + prefix, payload_size))
        panic("Unable to serialize zstd trace protobuf message");
    write(bytes.data(), bytes.size());
}

void
ZstdProtoOutputStream::close()
{
    if (closed)
        return;
    write(nullptr, 0, true);
    const size_t result = ZSTD_freeCStream(
        static_cast<ZSTD_CStream *>(stream));
    if (ZSTD_isError(result))
        panic("zstd trace close failed: %s", ZSTD_getErrorName(result));
    stream = nullptr;
    file.close();
    if (!file)
        panic("zstd trace file close failed");
    closed = true;
}

} // namespace gem5
