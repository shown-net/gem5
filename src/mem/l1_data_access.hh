// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_L1_DATA_ACCESS_HH__
#define __MEM_L1_DATA_ACCESS_HH__

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

#include "base/extensible.hh"
#include "base/types.hh"

namespace gem5
{

class Request;

// Per-fragment, page-offset-only index preparation.
class L1IndexPreparation : public Extension<Request, L1IndexPreparation>
{
  public:
    L1IndexPreparation(Tick ready, Addr index) : readyTick(ready), index(index) {}
    Tick readyTick;
    Addr index;
    bool consumed = false;
    std::unique_ptr<ExtensionBase> clone() const override
    {
        return std::make_unique<L1IndexPreparation>(*this);
    }
};

// Synchronous request-side query for L1 data service. Unlike tryTiming, a
// failed query does not register a retry or block the port. The caller
// records an access only after the timing request is accepted.
class L1DataAccessResource
{
  public:
    virtual ~L1DataAccessResource() = default;
    virtual bool bankAvailable(Addr address, uint64_t epoch) const = 0;
    virtual void noteBankAccess(Addr address, uint64_t epoch) = 0;
    virtual bool widthAvailable(bool is_load, unsigned bytes,
                                uint64_t epoch) const = 0;
    virtual void noteWidthAccess(bool is_load, unsigned bytes,
                                 uint64_t epoch) = 0;
    virtual bool loadWidthExhausted(uint64_t epoch) const = 0;
};

// Shared implementation of the existing SPR request-side budget. The Ruby
// L1D owns one; other timing ports use it locally in LSQ when selected.
class L1DataAccessBudget : public L1DataAccessResource
{
  private:
    std::array<unsigned, 16> bankUses{};
    uint64_t bankEpoch = uint64_t(-1);
    unsigned loadWidthUsed = 0;
    unsigned storeWidthUsed = 0;
    uint64_t widthEpoch = uint64_t(-1);

  public:
    bool bankAvailable(Addr address, uint64_t epoch) const override
    {
        return bankEpoch != epoch || bankUses[(address >> 2) & 15] < 2;
    }
    void noteBankAccess(Addr address, uint64_t epoch) override
    {
        if (bankEpoch != epoch) {
            bankUses.fill(0);
            bankEpoch = epoch;
        }
        auto &uses = bankUses[(address >> 2) & 15];
        assert(uses < 2);
        ++uses;
    }
    bool widthAvailable(bool is_load, unsigned bytes,
                        uint64_t epoch) const override
    {
        const unsigned units = (bytes + 31) / 32;
        const unsigned used = widthEpoch == epoch ?
            (is_load ? loadWidthUsed : storeWidthUsed) : 0;
        return used + units <= (is_load ? 4 : 2);
    }
    void noteWidthAccess(bool is_load, unsigned bytes,
                         uint64_t epoch) override
    {
        if (widthEpoch != epoch) {
            loadWidthUsed = storeWidthUsed = 0;
            widthEpoch = epoch;
        }
        assert(widthAvailable(is_load, bytes, epoch));
        (is_load ? loadWidthUsed : storeWidthUsed) += (bytes + 31) / 32;
    }
    bool loadWidthExhausted(uint64_t epoch) const override
    { return widthEpoch == epoch && loadWidthUsed >= 4; }
};

class L1DataAccessPort
{
  public:
    virtual ~L1DataAccessPort() = default;
    virtual L1DataAccessResource *l1DataAccess() = 0;
};

} // namespace gem5
#endif
