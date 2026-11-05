#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace gem5::functional_fst
{
enum class StoreRole { Other, Address, Data };

// SA/SD are paired only when the target decoder proves a two-member macro,
// and the next retained record belongs to the same context, ASID and PC.
class StorePairer
{
  public:
    std::uint64_t observe(std::uint64_t ordinal, std::uint32_t context,
                          std::uint64_t asid, std::uint64_t pc,
                          std::uint32_t microPc, StoreRole role,
                          bool twoMemberMacro, bool abandoned)
    {
        if (abandoned) pending = 0;
        if (role == StoreRole::Data) {
            const auto match = pending && ordinal == nextOrdinal &&
                context == owner && asid == space && pc == address &&
                microPc == 1 && twoMemberMacro;
            const auto result = match ? pending : 0;
            pending = 0;
            return result;
        }
        pending = 0;
        if (role != StoreRole::Address || !twoMemberMacro || microPc != 0)
            return 0;
        if (ordinal == std::numeric_limits<std::uint64_t>::max() ||
            nextGroup == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("functional store pair identity overflow");
        pending = ++nextGroup;
        nextOrdinal = ordinal + 1;
        owner = context;
        space = asid;
        address = pc;
        return pending;
    }
  private:
    std::uint64_t pending = 0, nextGroup = 0, nextOrdinal = 0;
    std::uint64_t space = 0, address = 0;
    std::uint32_t owner = 0;
};
} // namespace gem5::functional_fst
