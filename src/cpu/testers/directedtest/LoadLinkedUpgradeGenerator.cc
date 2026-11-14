// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/testers/directedtest/LoadLinkedUpgradeGenerator.hh"

#include "base/logging.hh"
#include "cpu/testers/directedtest/RubyDirectedTester.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

namespace
{
constexpr Addr sharedLine = 0x1000;
constexpr Addr invalidLine = 0x2000;
constexpr uint8_t value = 0x5a;
}

bool
LoadLinkedUpgradeGenerator::initiate()
{
    panic_if(pending || phase >= 5, "invalid LL control generator state");
    const bool locked = phase >= 3;
    const auto address = phase == 4 ? invalidLine : sharedLine;
    const auto port = phase == 2 || phase == 3 ? 1 : 0;
    Request::Flags flags;
    if (locked)
        flags.set(Request::LLSC);
    auto request = std::make_shared<Request>(address, 1, flags, requestorId);
    const auto command = phase == 0 ? MemCmd::WriteReq :
                         locked ? MemCmd::LoadLockedReq : MemCmd::ReadReq;
    auto *packet = new Packet(request, command);
    packet->allocate();
    if (phase == 0)
        packet->getPtr<uint8_t>()[0] = value;
    if (!m_directed_tester->getCpuPort(port)->sendTimingReq(packet)) {
        delete packet;
        return false;
    }
    pending = true;
    return true;
}

void
LoadLinkedUpgradeGenerator::checkResponse(uint32_t proc, PacketPtr packet)
{
    const auto expected_port = phase == 2 || phase == 3 ? 1u : 0u;
    const auto expected_address = phase == 4 ? invalidLine : sharedLine;
    panic_if(!pending || proc != expected_port || packet->getAddr() != expected_address,
             "LL control response identity mismatch at phase %u", phase);
    if (phase != 0) {
        const uint8_t expected = phase == 4 ? 0 : value;
        panic_if(packet->getConstPtr<uint8_t>()[0] != expected,
                 "LL control returned wrong value at phase %u", phase);
    }
}

void
LoadLinkedUpgradeGenerator::performCallback(uint32_t proc, Addr address)
{
    pending = false;
    if (++phase == 5)
        m_directed_tester->incrementCycleCompletions();
}

} // namespace gem5
