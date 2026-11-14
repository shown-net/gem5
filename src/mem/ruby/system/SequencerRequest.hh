// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_RUBY_SYSTEM_SEQUENCER_REQUEST_HH__
#define __MEM_RUBY_SYSTEM_SEQUENCER_REQUEST_HH__

#include <cassert>

#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/ruby/protocol/RubyRequestType.hh"

namespace gem5::ruby
{

// Request identity and observation remain associated with the Sequencer.
// A cache service may retain a copy while a response snapshot is queued.
struct SequencerRequest
{
    PacketPtr pkt;
    RubyRequestType m_type;
    RubyRequestType m_second_type;
    Cycles issue_time;
    uint8_t admissionDataSource = 0;
    bool issuedToL1 = false;
    RequestPtr serviceParent;
    bool serviceParentIsWrite = false;

    SequencerRequest(PacketPtr pkt, RubyRequestType type,
                     RubyRequestType second_type, Cycles issue_time)
        : pkt(pkt), m_type(type), m_second_type(second_type),
          issue_time(issue_time)
    {}

    bool functionalWrite(Packet *func_pkt) const
    {
        assert(func_pkt->isWrite());
        return func_pkt->trySatisfyFunctional(pkt);
    }
};

} // namespace gem5::ruby
#endif
