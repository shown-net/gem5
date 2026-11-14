// SPDX-License-Identifier: BSD-3-Clause
#ifndef __ARCH_X86_SSE_MATH_HH__
#define __ARCH_X86_SSE_MATH_HH__

#include <cstdint>
#include "sim/faults.hh"

namespace gem5
{
class ExecContext;
namespace X86ISA
{
enum class SSEOperation { Add, Subtract, Multiply, Divide, Sqrt };

// One architectural operation aggregates status from all active lanes. The
// state is transient; sticky bits are merged only when this operation commits.
class SSEMath
{
    const uint32_t control;
    uint8_t status = 0;
    uint64_t element(uint64_t left, uint64_t right, unsigned bytes,
                     SSEOperation operation);
  public:
    explicit SSEMath(ExecContext *xc);
    uint64_t binary(uint64_t left, uint64_t right, unsigned bytes,
                    SSEOperation operation)
    { return element(left, right, bytes, operation); }
    uint64_t sqrt(uint64_t value, unsigned bytes)
    { return element(value, 0, bytes, SSEOperation::Sqrt); }
    uint64_t packedBinary(uint64_t left, uint64_t right, unsigned bytes,
                          SSEOperation operation);
    uint64_t packedSqrt(uint64_t value, unsigned bytes);
    Fault finish(ExecContext *xc) const;
};
}
}
#endif
