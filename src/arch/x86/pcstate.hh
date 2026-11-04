/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ARCH_X86_PCSTATE_HH__
#define __ARCH_X86_PCSTATE_HH__

#include <array>
#include "fst_instruction_fetch.hh"
#include "arch/generic/pcstate.hh"
#include "sim/serialize.hh"

namespace gem5
{

namespace X86ISA
{

class PCState : public GenericISA::UPCState<8>
{
  protected:
    using Base = GenericISA::UPCState<8>;

    uint8_t _size = 0;
    std::array<uint8_t,15> _functionalBytes{};
    uint8_t _functionalByteCount = 0;
    uint64_t _functionalDecoderContext = 0;
    std::shared_ptr<const fastsim::fst::InstructionFetchFacts> _instructionTranslation;

  public:
    PCStateBase *clone() const override { return new PCState(*this); }

    void
    update(const PCStateBase &other) override
    {
        Base::update(other);
        auto &pcstate = other.as<PCState>();
        _size = pcstate._size;
        _functionalBytes = pcstate._functionalBytes;
        _functionalByteCount = pcstate._functionalByteCount;
        _functionalDecoderContext = pcstate._functionalDecoderContext;
        _instructionTranslation = pcstate._instructionTranslation;
    }

    void
    set(Addr val) override
    {
        Base::set(val);
        _size = 0;
        _functionalByteCount = 0;
        _instructionTranslation.reset();
    }

    PCState(const PCState &other) : Base(other), _size(other._size),
        _functionalBytes(other._functionalBytes),
        _functionalByteCount(other._functionalByteCount),
        _functionalDecoderContext(other._functionalDecoderContext),
        _instructionTranslation(other._instructionTranslation) {}
    PCState &operator=(const PCState &other) = default;
    PCState() {}
    explicit PCState(Addr val) { set(val); }

    void
    setNPC(Addr val)
    {
        Base::setNPC(val);
        _size = 0;
        _functionalByteCount = 0;
        _instructionTranslation.reset();
    }

    uint8_t size() const { return _size; }
    void size(uint8_t newSize) { _size = newSize; if (!newSize) { _functionalByteCount=0; _instructionTranslation.reset(); } }
    const std::array<uint8_t,15>& functionalBytes() const { return _functionalBytes; }
    uint8_t functionalByteCount() const { return _functionalByteCount; }
    uint64_t functionalDecoderContext() const { return _functionalDecoderContext; }
    void functionalBytes(const std::array<uint8_t,15>& bytes, uint8_t count, uint64_t context) {
        _functionalBytes=bytes; _functionalByteCount=count; _functionalDecoderContext=context;
        _instructionTranslation.reset();
    }

    const auto& instructionTranslation() const { return _instructionTranslation; }
    void instructionTranslation(std::shared_ptr<const fastsim::fst::InstructionFetchFacts> facts) {
        _instructionTranslation=std::move(facts);
    }

    bool
    branching() const override
    {
        return (this->npc() != this->pc() + size()) ||
               (this->nupc() != this->upc() + 1);
    }

    void
    advance() override
    {
        Base::advance();
        _size = 0;
        _functionalByteCount = 0;
        _instructionTranslation.reset();
    }

    void
    uEnd()
    {
        Base::uEnd();
        _size = 0;
        _functionalByteCount = 0;
        _instructionTranslation.reset();
    }

    void
    serialize(CheckpointOut &cp) const override
    {
        Base::serialize(cp);
        SERIALIZE_SCALAR(_size);
    }

    void
    unserialize(CheckpointIn &cp) override
    {
        Base::unserialize(cp);
        UNSERIALIZE_SCALAR(_size);
        // Reconstructed by decoding; old checkpoints carry no byte witness.
        _functionalByteCount = 0;
        _instructionTranslation.reset();
    }
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_PCSTATE_HH__
