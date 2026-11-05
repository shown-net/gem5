// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_DIRECTEDTEST_LOAD_LINKED_UPGRADE_GENERATOR_HH__
#define __CPU_DIRECTEDTEST_LOAD_LINKED_UPGRADE_GENERATOR_HH__

#include "cpu/testers/directedtest/DirectedGenerator.hh"
#include "params/LoadLinkedUpgradeGenerator.hh"

namespace gem5
{

// A strictly response-ordered, two-port LL/SC protocol control.
class LoadLinkedUpgradeGenerator : public DirectedGenerator
{
  public:
    explicit LoadLinkedUpgradeGenerator(const LoadLinkedUpgradeGeneratorParams &p)
        : DirectedGenerator(p) {}
    bool initiate() override;
    void checkResponse(uint32_t proc, PacketPtr pkt) override;
    void performCallback(uint32_t proc, Addr address) override;

  private:
    unsigned phase = 0;
    bool pending = false;
};

} // namespace gem5
#endif
