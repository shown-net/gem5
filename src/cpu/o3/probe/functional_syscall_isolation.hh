#pragma once

#include <cstdint>

namespace gem5::functional_fst
{
// FST v7 syscall metadata validity bits. Baseline observations remain in
// the collector; only the serialized functional copy is changed.
template <class Row>
void clearNativeSyscallTimes(Row &row)
{
    constexpr std::uint16_t nativeTimes = (1u << 4) | (1u << 5);
    row.pre_timestamp_us = 0;
    row.post_timestamp_us = 0;
    row.valid_fields &= static_cast<std::uint16_t>(~nativeTimes);
}
} // namespace gem5::functional_fst
