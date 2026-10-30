// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_INT_EXECUTION_INFO_HH__
#define __CPU_INT_EXECUTION_INFO_HH__

#include <cstdint>

namespace gem5
{

enum class IntExecutionFamily : uint8_t { None, ALU, SHFT, SlowInt, BM, Branch };
enum class IntResultRole : uint8_t { Any, GPR, CF, OF, Flags, Other };

struct IntExecutionInfo
{
    IntExecutionFamily family = IntExecutionFamily::None;
    uint16_t ports = 0;
    unsigned resultLatency = 1;
};

} // namespace gem5
#endif
