// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_ZERO_IDIOM_INFO_HH__
#define __CPU_ZERO_IDIOM_INFO_HH__
#include <cstdint>
namespace gem5
{
enum class ZeroIdiomKind : uint8_t { None, GprXor, GprSub, XmmXor, XmmSub };
}
#endif
