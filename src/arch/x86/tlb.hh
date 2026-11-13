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

#ifndef __ARCH_X86_TLB_HH__
#define __ARCH_X86_TLB_HH__

#include <functional>
#include <array>
#include <list>
#include <functional>
#include <memory>
#include <vector>

#include "arch/generic/tlb.hh"
#include "arch/x86/pagetable.hh"
#include "base/trie.hh"
#include "mem/request.hh"
#include "params/X86TLB.hh"
#include "sim/stats.hh"
#include "sim/full_system.hh"

namespace gem5
{

class ThreadContext;

namespace X86ISA
{
    class Walker;

    class TLB : public BaseTLB
    {
      protected:
        friend class Walker;

        typedef std::list<TlbEntry *> EntryList;

        uint32_t configAddress;

      public:

        typedef X86TLBParams Params;
        TLB(const Params &p);

        void takeOverFrom(BaseTLB *otlb) override { functional_mmu::lifecycle(this); }

        TlbEntry *lookup(Addr va, bool update_lru = true,
                         BaseMMU::Mode mode = BaseMMU::Read);

        void setConfigAddress(uint32_t addr);
        // Put the context above the 48-bit VA, outside every page offset.
        static Addr concAddrPcid(Addr vpn, uint64_t pcid)
        {
            return FullSystem ? (vpn & mask(48)) | ((pcid & mask(12)) << 48)
                              : vpn;
        }

      protected:

        EntryList::iterator lookupIt(Addr va, bool update_lru = true);

        Walker * walker;

      public:
        Walker *getWalker();

        void flushAll() override;
        void onCodeInvalidation(std::function<void()> callback)
        { codeInvalidation = std::move(callback); }

        void flushNonGlobal(int pcid = -1);
        void contextChanged();
        void invalidatePage(Addr va, uint16_t pcid, bool include_global);

        void demapPage(Addr va, uint64_t asn) override;
        uint64_t generation() const { return invalidationGeneration; }

      protected:
        std::function<void()> codeInvalidation;
        uint64_t invalidationGeneration = 0;
        uint32_t size;
        uint32_t baseSize;
        uint32_t smallPageWays;
        bool sprSharedDomains;
        unsigned sprSharedIndexShift;
        bool sprSplitL1;
        std::array<unsigned, 8> largePageTree{};
        Cycles nextLevelHitLatency;

        std::vector<TlbEntry> tlb;
        // Members retain physical-slot order, including after restoration.
        std::array<std::vector<TlbEntry *>, 256> sharedSets;
        unsigned sharedSet(Addr vpn, unsigned log_bytes) const;
        void indexSharedEntry(TlbEntry *entry);
        void removeEntry(TlbEntry *entry);

        EntryList freeList;

        TlbEntryTrie trie;
        TlbEntryTrie storeTrie;
        uint64_t lruSeq;

        AddrRange m5opRange;

        struct TlbStats : public statistics::Group
        {
            TlbStats(statistics::Group *parent);

            statistics::Scalar rdAccesses;
            statistics::Scalar wrAccesses;
            statistics::Scalar exAccesses;
            statistics::Scalar rdMisses;
            statistics::Scalar wrMisses;
            statistics::Scalar exMisses;
            statistics::Scalar stlbResponses;
            statistics::Scalar stlbSquashes;
            statistics::Scalar stlbRestarts;
        } stats;

        struct ItlbMissStats : public statistics::Group
        {
            explicit ItlbMissStats(statistics::Group *parent);
            statistics::Scalar STLB_HIT;
        };
        std::unique_ptr<ItlbMissStats> itlbMissStats;

        struct DtlbLoadMissStats : public statistics::Group
        {
            explicit DtlbLoadMissStats(statistics::Group *parent);
            statistics::Scalar STLB_HIT;
        };
        std::unique_ptr<DtlbLoadMissStats> dtlbLoadMissStats;
        struct DtlbStoreMissStats : public statistics::Group
        {
            explicit DtlbStoreMissStats(statistics::Group *parent);
            statistics::Scalar STLB_HIT;
        };
        std::unique_ptr<DtlbStoreMissStats> dtlbStoreMissStats;

        Fault translateInt(bool read, RequestPtr req, ThreadContext *tc);

        Fault translate(const RequestPtr &req, ThreadContext *tc,
                BaseMMU::Translation *translation, BaseMMU::Mode mode,
                bool &delayedResponse, bool timing);

      public:

        void evictLRU();

        uint64_t
        nextSeq()
        {
            return ++lruSeq;
        }

        Fault translateAtomic(
            const RequestPtr &req, ThreadContext *tc,
            BaseMMU::Mode mode) override;
        Fault translateFunctional(
            const RequestPtr &req, ThreadContext *tc,
            BaseMMU::Mode mode) override;
        void translateTiming(
            const RequestPtr &req, ThreadContext *tc,
            BaseMMU::Translation *translation, BaseMMU::Mode mode) override;

        /**
         * Do post-translation physical address finalization.
         *
         * Some addresses, for example requests going to the APIC,
         * need post-translation updates. Such physical addresses are
         * remapped into a "magic" part of the physical address space
         * by this method.
         *
         * @param req Request to updated in-place.
         * @param tc Thread context that created the request.
         * @param mode Request type (read/write/execute).
         * @return A fault on failure, NoFault otherwise.
         */
        Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                               BaseMMU::Mode mode) const override;

        TlbEntry *insert(Addr vpn, const TlbEntry &entry, uint64_t pcid,
                         bool fillNextLevel = true,
                         BaseMMU::Mode mode = BaseMMU::Read);

        // Checkpointing
        void serialize(CheckpointOut &cp) const override;
        void unserialize(CheckpointIn &cp) override;

        /**
         * Get the table walker port. This is used for
         * migrating port connections during a CPU takeOverFrom()
         * call. For architectures that do not have a table walker,
         * NULL is returned, hence the use of a pointer rather than a
         * reference. For X86 this method will always return a valid
         * port pointer.
         *
         * @return A pointer to the walker port
         */
        Port *getTableWalkerPort() override;
    };

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_TLB_HH__
