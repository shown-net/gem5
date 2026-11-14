/*
 * Copyright (c) 2011 Google
 * Copyright (c) 2015 Advanced Micro Devices, Inc.
 * All rights reserved.
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

#ifndef __ARCH_X86_MEMHELPERS_HH__
#define __ARCH_X86_MEMHELPERS_HH__

#include <array>

#include "base/types.hh"
#include "cpu/exec_context.hh"
#include "sim/byteswap.hh"
#include "sim/insttracer.hh"

namespace gem5
{

namespace X86ISA
{

/// Initiate a read from memory in timing mode.
static Fault
initiateMemRead(ExecContext *xc, trace::InstRecord *traceData, Addr addr,
                unsigned dataSize, Request::Flags flags)
{
    const std::vector<bool> byte_enable(dataSize, true);
    return xc->initiateMemRead(addr, dataSize, flags, byte_enable);
}

// REP carries at most one cache line. The active prefix is the exact access
// width, not eight independent reads or an instantaneous buffer copy.
static Fault
readRepMemAtomic(ExecContext *xc, trace::InstRecord *traceData, Addr addr,
                 std::array<uint64_t, 8> &mem, unsigned bytes,
                 Request::Flags flags)
{
    assert(bytes <= sizeof(mem));
    mem.fill(0);
    if (!bytes) return NoFault;
    std::array<uint8_t, 64> data{};
    const std::vector<bool> enabled(bytes, true);
    Fault fault = xc->readMem(addr, data.data(), bytes, flags, enabled);
    if (fault == NoFault) {
        for (unsigned i = 0; i < bytes; ++i)
            mem[i / 8] |= uint64_t(data[i]) << (8 * (i % 8));
        if (traceData) traceData->setData(mem[0]);
    }
    return fault;
}

static void
getRepMem(PacketPtr pkt, std::array<uint64_t, 8> &mem, unsigned bytes,
          trace::InstRecord *traceData)
{
    assert(bytes <= sizeof(mem));
    mem.fill(0);
    // A statically predicted REP control path may reach a zero-count
    // load. The LSQ completes its suppressed access without a packet.
    if (!bytes) return;
    assert(pkt && pkt->getSize() == bytes);
    const auto *data = pkt->getConstPtr<uint8_t>();
    for (unsigned i = 0; i < bytes; ++i)
        mem[i / 8] |= uint64_t(data[i]) << (8 * (i % 8));
    if (traceData) traceData->setData(mem[0]);
}

static Fault
writeRepMem(ExecContext *xc, trace::InstRecord *traceData,
            const std::array<uint64_t, 8> &mem, unsigned bytes, Addr addr,
            Request::Flags flags, uint64_t *res)
{
    assert(bytes <= sizeof(mem));
    if (!bytes) return NoFault;
    std::array<uint8_t, 64> data{};
    for (unsigned i = 0; i < bytes; ++i)
        data[i] = mem[i / 8] >> (8 * (i % 8));
    if (traceData) traceData->setData(mem[0]);
    const std::vector<bool> enabled(bytes, true);
    return xc->writeMem(data.data(), bytes, addr, flags, res, enabled);
}

static void
getMem(PacketPtr pkt, uint64_t &mem, unsigned dataSize,
       trace::InstRecord *traceData)
{
    switch (dataSize) {
      case 1:
        mem = pkt->getLE<uint8_t>();
        break;
      case 2:
        mem = pkt->getLE<uint16_t>();
        break;
      case 4:
        mem = pkt->getLE<uint32_t>();
        break;
      case 8:
        mem = pkt->getLE<uint64_t>();
        break;
      default:
        panic("Unhandled size in getMem.\n");
    }
    if (traceData)
        traceData->setData(mem);
}

template <typename T, size_t N>
static void
getPackedMem(PacketPtr pkt, std::array<uint64_t, N> &mem, unsigned dataSize)
{
    std::array<T, N> real_mem = pkt->getLE<std::array<T, N> >();
    for (int i = 0; i < N; i++)
        mem[i] = real_mem[i];
}

template <size_t N>
static void
getMem(PacketPtr pkt, std::array<uint64_t, N> &mem, unsigned dataSize,
       trace::InstRecord *traceData)
{
    switch (dataSize) {
      case 4:
        getPackedMem<uint32_t, N>(pkt, mem, dataSize);
        break;
      case 8:
        getPackedMem<uint64_t, N>(pkt, mem, dataSize);
        break;
      default:
        panic("Unhandled element size in getMem.\n");
    }
    if (traceData)
        traceData->setData(mem[0]);
}


static Fault
readMemAtomic(ExecContext *xc, trace::InstRecord *traceData, Addr addr,
              uint64_t &mem, unsigned dataSize, Request::Flags flags)
{
    memset(&mem, 0, sizeof(mem));
    const std::vector<bool> byte_enable(dataSize, true);
    Fault fault = xc->readMem(addr, (uint8_t *)&mem, dataSize,
                              flags, byte_enable);
    if (fault == NoFault) {
        // If LE to LE, this is a nop, if LE to BE, the actual data ends up
        // in the right place because the LSBs where at the low addresses on
        // access. This doesn't work for BE guests.
        mem = letoh(mem);
        if (traceData)
            traceData->setData(mem);
    }
    return fault;
}

template <typename T, size_t N>
static Fault
readPackedMemAtomic(ExecContext *xc, Addr addr, std::array<uint64_t, N> &mem,
                    unsigned flags)
{
    std::array<T, N> real_mem;
    // Size is fixed at compilation time. Make a static vector.
    constexpr auto size = sizeof(T) * N;
    static const std::vector<bool> byte_enable(size, true);
    Fault fault = xc->readMem(addr, (uint8_t *)&real_mem,
                              size, flags, byte_enable);
    if (fault == NoFault) {
        real_mem = letoh(real_mem);
        for (int i = 0; i < N; i++)
            mem[i] = real_mem[i];
    }
    return fault;
}

template <size_t N>
static Fault
readMemAtomic(ExecContext *xc, trace::InstRecord *traceData, Addr addr,
              std::array<uint64_t, N> &mem, unsigned dataSize,
              unsigned flags)
{
    Fault fault = NoFault;

    switch (dataSize) {
      case 4:
        fault = readPackedMemAtomic<uint32_t, N>(xc, addr, mem, flags);
        break;
      case 8:
        fault = readPackedMemAtomic<uint64_t, N>(xc, addr, mem, flags);
        break;
      default:
        panic("Unhandled element size in readMemAtomic\n");
    }
    if (fault == NoFault && traceData)
        traceData->setData(mem[0]);
    return fault;
}

template <typename T, size_t N>
static Fault
writePackedMem(ExecContext *xc, std::array<uint64_t, N> &mem, Addr addr,
               unsigned flags, uint64_t *res)
{
    std::array<T, N> real_mem;
    for (int i = 0; i < N; i++)
        real_mem[i] = mem[i];
    real_mem = htole(real_mem);
    // Size is fixed at compilation time. Make a static vector.
    constexpr auto size = sizeof(T) * N;
    static const std::vector<bool> byte_enable(size, true);
    return xc->writeMem((uint8_t *)&real_mem, size,
                        addr, flags, res, byte_enable);
}

static Fault
writeMemTiming(ExecContext *xc, trace::InstRecord *traceData, uint64_t mem,
               unsigned dataSize, Addr addr, Request::Flags flags,
               uint64_t *res)
{
    if (traceData)
        traceData->setData(mem);
    mem = htole(mem);
    const std::vector<bool> byte_enable(dataSize, true);
    return xc->writeMem((uint8_t *)&mem, dataSize, addr, flags,
                        res, byte_enable);
}

template <size_t N>
static Fault
writeMemTiming(ExecContext *xc, trace::InstRecord *traceData,
               std::array<uint64_t, N> &mem, unsigned dataSize,
               Addr addr, unsigned flags, uint64_t *res)
{
    if (traceData)
        traceData->setData(mem[0]);

    switch (dataSize) {
      case 4:
        return writePackedMem<uint32_t, N>(xc, mem, addr, flags, res);
      case 8:
        return writePackedMem<uint64_t, N>(xc, mem, addr, flags, res);
      default:
        panic("Unhandled element size in writeMemTiming.\n");
    }
}

static Fault
writeMemAtomic(ExecContext *xc, trace::InstRecord *traceData, uint64_t mem,
               unsigned dataSize, Addr addr, Request::Flags flags,
               uint64_t *res)
{
    if (traceData)
        traceData->setData(mem);
    uint64_t host_mem = htole(mem);
    const std::vector<bool> byte_enable(dataSize, true);
    Fault fault = xc->writeMem((uint8_t *)&host_mem, dataSize, addr,
                               flags, res, byte_enable);
    if (fault == NoFault && res)
        *res = letoh(*res);
    return fault;
}

template <size_t N>
static Fault
writeMemAtomic(ExecContext *xc, trace::InstRecord *traceData,
               std::array<uint64_t, N> &mem, unsigned dataSize,
               Addr addr, unsigned flags, uint64_t *res)
{
    if (traceData)
        traceData->setData(mem[0]);

    Fault fault;
    switch (dataSize) {
      case 4:
        fault = writePackedMem<uint32_t, N>(xc, mem, addr, flags, res);
        break;
      case 8:
        fault = writePackedMem<uint64_t, N>(xc, mem, addr, flags, res);
        break;
      default:
        panic("Unhandled element size in writeMemAtomic.\n");
    }

    if (fault == NoFault && res)
        *res = letoh(*res);

    return fault;
}

} // namespace X86ISA
} // namespace gem5

#endif
