#ifndef __PROTO_ZSTD_PROTOIO_HH__
#define __PROTO_ZSTD_PROTOIO_HH__

#include <google/protobuf/message.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace gem5
{

class ZstdProtoOutputStream
{
  public:
    ZstdProtoOutputStream(const std::string &filename, int compression_level);
    ~ZstdProtoOutputStream();

    void writeDelimited(const google::protobuf::Message &message);
    void close();

  private:
    void write(const void *data, size_t size, bool end_frame = false);
    std::ofstream file;
    void *stream;
    bool closed;
};

} // namespace gem5

#endif
