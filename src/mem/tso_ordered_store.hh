// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_TSO_ORDERED_STORE_HH__
#define __MEM_TSO_ORDERED_STORE_HH__

#include "base/extensible.hh"
#include "mem/request.hh"

namespace gem5
{
/** A committed ordinary store whose visibility is ordered by Ruby L0. */
class TSOOrderedStore : public Extension<Request, TSOOrderedStore>
{
  public:
    // Number of retired stores represented by this request. This is metadata,
    // not an additional queue entry or a store-service budget.
    unsigned logicalStores = 1;

    // Set synchronously only by a Sequencer using the supporting protocol.
    bool acceptedBySequencer = false;

    std::unique_ptr<ExtensionBase> clone() const override
    { return std::make_unique<TSOOrderedStore>(*this); }
};
}
#endif
