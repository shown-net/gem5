/*
 * Copyright (c) 2007-2008 The Hewlett-Packard Development Company
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

#include "arch/x86/tlb.hh"

#include <cstring>
#include <algorithm>
#include <memory>

#include "arch/x86/faults.hh"
#include "mem/functional_translation.hh"
#include "arch/x86/insts/microldstop.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/pseudo_inst_abi.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/regs/msr.hh"
#include "arch/x86/x86_traits.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "mem/packet_access.hh"
#include "mem/page_table.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace X86ISA {

TLB::TLB(const Params &p)
    : BaseTLB(p), configAddress(0),
      size(p.size + (p.spr_split_l1 ?
           (p.dtlb_4k_ways ? 56 : 32) : 0)), baseSize(p.size),
      smallPageWays(p.dtlb_4k_ways ? p.dtlb_4k_ways : p.itlb_4k_ways),
      sprSharedDomains(p.spr_shared_domains), sprSplitL1(p.spr_split_l1),
      nextLevelHitLatency(p.next_level_hit_latency),
      tlb(size), lruSeq(0), m5opRange(p.system->m5opRange()), stats(this)
{
    if (!size)
        fatal("TLBs must have a non-zero size.\n");
    fatal_if(p.itlb_4k_ways && p.dtlb_4k_ways,
             "Select either instruction or data 4K TLB ways");
    fatal_if(p.dtlb_4k_ways && type() != TypeTLB::data,
             "Data 4K associativity applies only to a data TLB");
    fatal_if(smallPageWays && (baseSize % smallPageWays),
             "TLB size must be divisible by its number of 4K ways");
    fatal_if(sprSharedDomains && size != 2048,
             "SPR shared TLB proxy requires 2048 entries");
    if (nextLevel() && type() == TypeTLB::instruction)
        itlbMissStats = std::make_unique<ItlbMissStats>(this);
    if (type() == TypeTLB::data) {
        dtlbLoadMissStats = std::make_unique<DtlbLoadMissStats>(this);
        dtlbStoreMissStats = std::make_unique<DtlbStoreMissStats>(this);
    }

    for (int x = 0; x < size; x++) {
        tlb[x].trieHandle = NULL;
        freeList.push_back(&tlb[x]);
    }

    walker = p.walker;
    walker->setTLB(this);
}

TLB::ItlbMissStats::ItlbMissStats(statistics::Group *parent)
    : statistics::Group(parent, "ITLB_MISSES"),
      ADD_STAT(STLB_HIT, statistics::units::Count::get(),
               "Instruction TLB misses satisfied by the modeled shared TLB")
{}

TLB::DtlbLoadMissStats::DtlbLoadMissStats(statistics::Group *parent)
    : statistics::Group(parent, "DTLB_LOAD_MISSES"),
      ADD_STAT(STLB_HIT, statistics::units::Count::get(),
               "Timing demand-read DTLB misses finding a modeled shared TLB entry; includes speculative lookups")
{}

TLB::DtlbStoreMissStats::DtlbStoreMissStats(statistics::Group *parent)
    : statistics::Group(parent, "DTLB_STORE_MISSES"),
      ADD_STAT(STLB_HIT, statistics::units::Count::get(),
               "Timing demand-write DTLB misses finding the modeled shared TLB")
{}

void
TLB::evictLRU()
{
    // Find the entry with the lowest (and hence least recently updated)
    // sequence number.

    unsigned lru = 0;
    for (unsigned i = 1; i < size; i++) {
        if (tlb[i].lruSeq < tlb[lru].lruSeq)
            lru = i;
    }

    assert(tlb[lru].trieHandle);
    trie.remove(tlb[lru].trieHandle);
    tlb[lru].trieHandle = NULL;
    freeList.push_back(&tlb[lru]);
}

TlbEntry *
TLB::insert(Addr vpn, const TlbEntry &entry, uint64_t pcid, bool fillNextLevel,
            BaseMMU::Mode mode)
{
    const Addr key = entry.global && FullSystem ?
        (vpn & mask(48)) | (Addr(1) << 60) : concAddrPcid(vpn, pcid);

    if (sprSplitL1) {
        const bool data = type() == TypeTLB::data;
        const bool store = data && mode == BaseMMU::Write;
        TlbEntryTrie &index = store ? storeTrie : trie;
        if (auto *existing = index.lookup(key)) return existing;

        unsigned first = 0, last = baseSize;
        const bool large = entry.logBytes == 21 || entry.logBytes == 22;
        unsigned set = 0, ways = 0;
        if (store) {
            first = baseSize + 40;
            last = first + 16;
        } else if (large) {
            ways = data ? 4 : 8;
            set = data ? ((vpn >> 22) & 7) :
                         ((vpn >> entry.logBytes) & 3);
            first = baseSize + set * ways;
            last = first + ways;
        } else if (data && entry.logBytes == 30) {
            first = baseSize + 32;
            last = first + 8;
        }
        auto removeEntry = [&](TlbEntry *victim) {
            TlbEntryTrie &owner = data &&
                unsigned(victim - tlb.data()) >= baseSize + 40 ? storeTrie : trie;
            owner.remove(victim->trieHandle);
            victim->trieHandle = nullptr;
            freeList.push_back(victim);
        };
        if (first == 0 && entry.logBytes == 12) {
            const unsigned target = (vpn >> 12) % (baseSize / smallPageWays);
            unsigned used = 0;
            TlbEntry *victim = nullptr;
            for (unsigned i = 0; i < baseSize; ++i) {
                auto &candidate = tlb[i];
                if (candidate.trieHandle && candidate.logBytes == 12 &&
                    (candidate.vaddr >> 12) % (baseSize / smallPageWays) == target) {
                    ++used;
                    if (!victim || candidate.lruSeq < victim->lruSeq)
                        victim = &candidate;
                }
            }
            if (used >= smallPageWays) removeEntry(victim);
        }
        auto free = std::find_if(freeList.begin(), freeList.end(),
            [&](TlbEntry *candidate) {
                const auto position = unsigned(candidate - tlb.data());
                return position >= first && position < last;
            });
        if (free == freeList.end()) {
            TlbEntry *victim = nullptr;
            if (data && large) {
                const unsigned bits = largePageTree[set];
                const unsigned half = bits & 1;
                const unsigned leaf = (bits >> (half ? 2 : 1)) & 1;
                victim = &tlb[first + 2 * half + leaf];
            } else {
                for (unsigned i = first; i < last; ++i)
                    if (!victim || tlb[i].lruSeq < victim->lruSeq)
                        victim = &tlb[i];
            }
            removeEntry(victim);
            free = std::prev(freeList.end());
        }
        TlbEntry *chosen = *free;
        freeList.erase(free);
        const unsigned way = unsigned(chosen - tlb.data()) - first;
        *chosen = entry;
        chosen->vaddr = vpn;
        chosen->pcid = pcid;
        chosen->lruSeq = nextSeq();
        chosen->trieHandle = index.insert(key, FullSystem ?
            TlbEntryTrie::MaxBits - entry.logBytes : TlbEntryTrie::MaxBits, chosen);
        if (data && large) {
            auto &bits = largePageTree[set];
            const unsigned half = way / 2, leaf = way % 2;
            bits = (bits & ~1U) | (1U - half);
            const unsigned bit = half ? 2 : 1;
            bits = (bits & ~(1U << bit)) | ((1U - leaf) << bit);
        }
        if (fillNextLevel && nextLevel())
            static_cast<TLB *>(nextLevel())->insert(entry.vaddr, entry, pcid);
        return chosen;
    }

    // If somebody beat us to it, just use that existing entry.
    TlbEntry *newEntry = trie.lookup(key);
    if (newEntry) {
        return newEntry;
    }

    if (sprSharedDomains) {
        // Two support domains. A: 4K/2M/4M, B: 4K/1G.
        // 4K placement uses VPN[7] for the domain and VPN[6:0] for
        // the set; large-page indexing is an uncalibrated proxy.
        auto location = [](Addr addr, unsigned log_bytes) {
            const Addr page = addr >> log_bytes;
            const unsigned domain = log_bytes == 12 ? ((page >> 7) & 1) :
                                    log_bytes == 30 ? 1 : 0;
            return std::make_pair(domain, unsigned(page & 127));
        };
        auto target = location(vpn, entry.logBytes);
        unsigned used = 0;
        TlbEntry *victim = nullptr;
        for (auto &candidate : tlb) {
            if (candidate.trieHandle &&
                location(candidate.vaddr, candidate.logBytes) == target) {
                ++used;
                if (!victim || candidate.lruSeq < victim->lruSeq)
                    victim = &candidate;
            }
        }
        if (used >= 8) {
            trie.remove(victim->trieHandle);
            victim->trieHandle = nullptr;
            freeList.push_back(victim);
        }
    } else if (smallPageWays && entry.logBytes == 12) {
        const auto set = (vpn >> 12) % (size / smallPageWays);
        unsigned used = 0;
        TlbEntry *victim = nullptr;
        for (auto &candidate : tlb) {
            if (candidate.trieHandle && candidate.logBytes == 12 &&
                (candidate.vaddr >> 12) % (size / smallPageWays) == set) {
                ++used;
                if (!victim || candidate.lruSeq < victim->lruSeq)
                    victim = &candidate;
            }
        }
        if (used >= smallPageWays) {
            trie.remove(victim->trieHandle);
            victim->trieHandle = nullptr;
            freeList.push_back(victim);
        }
    }
    if (freeList.empty())
        evictLRU();

    newEntry = freeList.front();
    freeList.pop_front();

    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    newEntry->pcid = pcid;
    if (FullSystem) {
        newEntry->trieHandle =
        trie.insert(key, TlbEntryTrie::MaxBits-entry.logBytes, newEntry);
    }
    else {
        newEntry->trieHandle =
        trie.insert(key, TlbEntryTrie::MaxBits, newEntry);
    }
    if (fillNextLevel && nextLevel())
        static_cast<TLB *>(nextLevel())->insert(entry.vaddr, entry, pcid);
    return newEntry;
}

TlbEntry *
TLB::lookup(Addr va, bool update_lru, BaseMMU::Mode mode)
{
    TlbEntryTrie &index = sprSplitL1 && type() == TypeTLB::data &&
        mode == BaseMMU::Write ? storeTrie : trie;
    TlbEntry *entry = index.lookup(va);
    if (!entry && FullSystem)
        entry = index.lookup((va & mask(48)) | (Addr(1) << 60));
    if (entry && update_lru) {
        entry->lruSeq = nextSeq();
        const unsigned pos = unsigned(entry - tlb.data());
        if (sprSplitL1 && type() == TypeTLB::data &&
            pos >= baseSize && pos < baseSize + 32) {
            const unsigned set = (pos - baseSize) / 4, way = (pos - baseSize) % 4;
            auto &bits = largePageTree[set];
            const unsigned half = way / 2, bit = half ? 2 : 1;
            bits = (bits & ~1U) | (1U - half);
            bits = (bits & ~(1U << bit)) | ((1U - way % 2) << bit);
        }
    }
    return entry;
}

void
TLB::flushAll()
{
    ++invalidationGeneration;
    if (walker->pagingStructureCache)
        walker->pagingStructureCache->invalidate();
    if (codeInvalidation) codeInvalidation();
    if (nextLevel()) static_cast<TLB *>(nextLevel())->flushAll();
    DPRINTF(TLB, "Invalidating all entries.\n");
    for (unsigned i = 0; i < size; i++) {
        if (tlb[i].trieHandle) {
            (sprSplitL1 && type() == TypeTLB::data && i >= baseSize + 40 ?
                storeTrie : trie).remove(tlb[i].trieHandle);
            tlb[i].trieHandle = NULL;
            freeList.push_back(&tlb[i]);
        }
    }
}

void
TLB::setConfigAddress(uint32_t addr)
{
    configAddress = addr;
}

void
TLB::flushNonGlobal(int pcid)
{
    ++invalidationGeneration;
    if (walker->pagingStructureCache)
        walker->pagingStructureCache->invalidate(pcid);
    if (codeInvalidation) codeInvalidation();
    if (nextLevel()) static_cast<TLB *>(nextLevel())->flushNonGlobal(pcid);
    DPRINTF(TLB, "Invalidating all non global entries.\n");
    for (unsigned i = 0; i < size; i++) {
        if (tlb[i].trieHandle && !tlb[i].global &&
            (pcid < 0 || tlb[i].pcid == pcid)) {
            (sprSplitL1 && type() == TypeTLB::data && i >= baseSize + 40 ?
                storeTrie : trie).remove(tlb[i].trieHandle);
            tlb[i].trieHandle = NULL;
            freeList.push_back(&tlb[i]);
        }
    }
}

void
TLB::demapPage(Addr va, uint64_t asn)
{
    invalidatePage(va, asn, true);
}

void
TLB::contextChanged()
{
    ++invalidationGeneration;
    if (codeInvalidation) codeInvalidation();
    if (nextLevel()) static_cast<TLB *>(nextLevel())->contextChanged();
}

void
TLB::invalidatePage(Addr va, uint16_t pcid, bool include_global)
{
    ++invalidationGeneration;
    // INVLPG clears all upper-prefix entries for the PCID, independently of
    // VA. Architectural page faults may conservatively use this same scope.
    if (walker->pagingStructureCache)
        walker->pagingStructureCache->invalidate(pcid);
    if (codeInvalidation) codeInvalidation();
    if (nextLevel())
        static_cast<TLB *>(nextLevel())->invalidatePage(va, pcid, include_global);
    // Scan both load and store banks, including overlapping large pages.
    for (unsigned i = 0; i < size; ++i) {
        auto &entry = tlb[i];
        if (entry.trieHandle &&
            ((entry.global && include_global) ||
             (!entry.global && entry.pcid == pcid)) &&
            (va & ~mask(entry.logBytes)) == entry.vaddr) {
            (sprSplitL1 && type() == TypeTLB::data && i >= baseSize + 40 ?
                storeTrie : trie).remove(entry.trieHandle);
            entry.trieHandle = nullptr;
            freeList.push_back(&entry);
        }
    }
}

namespace
{

Cycles
localMiscRegAccess(bool read, RegIndex regNum,
                   ThreadContext *tc, PacketPtr pkt)
{
    if (read) {
        RegVal data = htole(tc->readMiscReg(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->setData((uint8_t *)&data);
    } else {
        RegVal data = htole(tc->readMiscRegNoEffect(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->writeData((uint8_t *)&data);
        tc->setMiscReg(regNum, letoh(data));
    }
    return Cycles(1);
}

} // anonymous namespace

Fault
TLB::translateInt(bool read, RequestPtr req, ThreadContext *tc)
{
    DPRINTF(TLB, "Addresses references internal memory.\n");
    Addr vaddr = req->getVaddr();
    Addr prefix = (vaddr >> 3) & IntAddrPrefixMask;
    if (prefix == IntAddrPrefixCPUID) {
        panic("CPUID memory space not yet implemented!\n");
    } else if (prefix == IntAddrPrefixMSR) {
        vaddr = (vaddr >> 3) & ~IntAddrPrefixMask;

        RegIndex regNum;
        if (!msrAddrToIndex(regNum, vaddr))
            return std::make_shared<GeneralProtection>(0);
        // IA32_PRED_CMD is a write-only command, not readable MSR state.
        if (read && regNum == misc_reg::PredCmd)
            return std::make_shared<GeneralProtection>(0);

        req->setPaddr(req->getVaddr());
        req->setLocalAccessor(
            [read,regNum](ThreadContext *tc, PacketPtr pkt)
            {
                return localMiscRegAccess(read, regNum, tc, pkt);
            }
        );

        recordFunctionalSpecialTranslation(req, fastsim::fst::TranslationKind::kMsr,
            tc->readMiscRegNoEffect(misc_reg::Cr3) & ~uint64_t(0xfff));
        return NoFault;
    } else if (prefix == IntAddrPrefixIO) {
        // TODO If CPL > IOPL or in virtual mode, check the I/O permission
        // bitmap in the TSS.

        auto functionalKind = fastsim::fst::TranslationKind::kIo;
        Addr IOPort = vaddr & ~IntAddrPrefixMask;
        // Make sure the address fits in the expected 16 bit IO address
        // space.
        assert(!(IOPort & ~0xFFFF));
        if (IOPort == 0xCF8 && req->getSize() == 4) {
            functionalKind = fastsim::fst::TranslationKind::kPciConfig;
            req->setPaddr(req->getVaddr());
            req->setLocalAccessor(
                [read](ThreadContext *tc, PacketPtr pkt)
                {
                    return localMiscRegAccess(
                            read, misc_reg::PciConfigAddress, tc, pkt);
                }
            );
        } else if ((IOPort & ~mask(2)) == 0xCFC) {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            Addr configAddress =
                tc->readMiscRegNoEffect(misc_reg::PciConfigAddress);
            if (bits(configAddress, 31, 31)) {
                functionalKind = fastsim::fst::TranslationKind::kPciConfig;
                req->setPaddr(PhysAddrPrefixPciConfig |
                        mbits(configAddress, 30, 2) |
                        (IOPort & mask(2)));
            } else {
                req->setPaddr(PhysAddrPrefixIO | IOPort);
            }
        } else {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(PhysAddrPrefixIO | IOPort);
        }
        recordFunctionalSpecialTranslation(req, functionalKind,
            tc->readMiscRegNoEffect(misc_reg::Cr3) & ~uint64_t(0xfff));
        return NoFault;
    } else {
        panic("Access to unrecognized internal address space %#x.\n",
                prefix);
    }
}

Fault
TLB::finalizePhysical(const RequestPtr &req,
                      ThreadContext *tc, BaseMMU::Mode mode) const
{
    Addr paddr = req->getPaddr();

    if (m5opRange.contains(paddr)) {
        req->setFlags(Request::STRICT_ORDER);
        uint8_t func;
        pseudo_inst::decodeAddrOffset(paddr - m5opRange.start(), func);
        req->setLocalAccessor(
            [func, mode](ThreadContext *tc, PacketPtr pkt) -> Cycles
            {
                uint64_t ret;
                pseudo_inst::pseudoInst<X86PseudoInstABI, true>(tc, func, ret);
                if (mode == BaseMMU::Read)
                    pkt->setLE(ret);
                return Cycles(1);
            }
        );
        recordFunctionalSpecialTranslation(req, fastsim::fst::TranslationKind::kM5,
            tc->readMiscRegNoEffect(misc_reg::Cr3) & ~uint64_t(0xfff));
    } else if (FullSystem) {
        // Check for an access to the local APIC
        LocalApicBase localApicBase =
            tc->readMiscRegNoEffect(misc_reg::ApicBase);
        AddrRange apicRange(localApicBase.base * PageBytes,
                            (localApicBase.base + 1) * PageBytes);

        if (apicRange.contains(paddr)) {
            // The Intel developer's manuals say the below restrictions apply,
            // but the linux kernel, because of a compiler optimization, breaks
            // them.
            /*
            // Check alignment
            if (paddr & ((32/8) - 1))
                return new GeneralProtection(0);
            // Check access size
            if (req->getSize() != (32/8))
                return new GeneralProtection(0);
            */
            // Force the access to be uncacheable.
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(x86LocalAPICAddress(tc->contextId(),
                                              paddr - apicRange.start()));
            recordFunctionalSpecialTranslation(req, fastsim::fst::TranslationKind::kLocalApic,
                tc->readMiscRegNoEffect(misc_reg::Cr3) & ~uint64_t(0xfff));
        }
    }

    return NoFault;
}

Fault
TLB::translate(const RequestPtr &req,
        ThreadContext *tc, BaseMMU::Translation *translation,
        BaseMMU::Mode mode, bool &delayedResponse, bool timing)
{
    // Retries and failed translations cannot expose an older Request's facts.
    req->removeExtension<FunctionalTranslationExtension>();
    fastsim::fst::TranslationMapping functionalMapping;
    bool functionalLeaf = false;
    Request::Flags flags = req->getFlags();
    int seg = flags & SegmentFlagMask;
    bool storeCheck = flags & Request::READ_MODIFY_WRITE;

    delayedResponse = false;

    // If this is true, we're dealing with a request to a non-memory address
    // space.
    if (seg == segment_idx::Ms) {
        return translateInt(mode == BaseMMU::Read, req, tc);
    }

    Addr vaddr = req->getVaddr();
    DPRINTF(TLB, "Translating vaddr %#x.\n", vaddr);

    HandyM5Reg m5Reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);

    const Addr logAddrSize = (flags >> AddrSizeFlagShift) & AddrSizeFlagMask;
    const int addrSize = 8 << logAddrSize;
    const Addr addrMask = mask(addrSize);

    // If protected mode has been enabled...
    if (m5Reg.prot) {
        DPRINTF(TLB, "In protected mode.\n");
        // If we're not in 64-bit mode, do protection/limit checks
        if (m5Reg.mode != LongMode) {
            DPRINTF(TLB, "Not in long mode. Checking segment protection.\n");

            // CPUs won't know to use CS when building fetch requests, so we
            // need to override the value of "seg" here if this is a fetch.
            if (mode == BaseMMU::Execute)
                seg = segment_idx::Cs;

            SegAttr attr = tc->readMiscRegNoEffect(misc_reg::segAttr(seg));
            // Check for an unusable segment.
            if (attr.unusable) {
                DPRINTF(TLB, "Unusable segment.\n");
                return std::make_shared<GeneralProtection>(0);
            }
            bool expandDown = false;
            if (seg >= segment_idx::Es && seg <= segment_idx::Hs) {
                if (!attr.writable && (mode == BaseMMU::Write || storeCheck)) {
                    DPRINTF(TLB, "Tried to write to unwritable segment.\n");
                    return std::make_shared<GeneralProtection>(0);
                }
                if (!attr.readable && mode == BaseMMU::Read) {
                    DPRINTF(TLB, "Tried to read from unreadble segment.\n");
                    return std::make_shared<GeneralProtection>(0);
                }
                expandDown = attr.expandDown;

            }
            Addr base = tc->readMiscRegNoEffect(misc_reg::segBase(seg));
            Addr limit = tc->readMiscRegNoEffect(misc_reg::segLimit(seg));
            Addr offset;
            if (mode == BaseMMU::Execute)
                offset = vaddr - base;
            else
                offset = (vaddr - base) & addrMask;
            Addr endOffset = offset + req->getSize() - 1;
            if (expandDown) {
                DPRINTF(TLB, "Checking an expand down segment.\n");
                warn_once("Expand down segments are untested.\n");
                if (offset <= limit || endOffset <= limit)
                    return std::make_shared<GeneralProtection>(0);
            } else {
                if (offset > limit || endOffset > limit) {
                    DPRINTF(TLB, "Segment limit check failed, "
                            "offset = %#x limit = %#x.\n", offset, limit);
                    return std::make_shared<GeneralProtection>(0);
                }
            }
        }
        if (m5Reg.submode != SixtyFourBitMode && addrSize != 64)
            vaddr &= mask(32);
        // If paging is enabled, do the translation.
        if (m5Reg.paging) {
            DPRINTF(TLB, "Paging enabled.\n");
            // The vaddr already has the segment base applied.

            //Appending the pcid (last 12 bits of CR3) to the
            //page aligned vaddr if pcide is set
            CR4 cr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
            Addr pageAlignedVaddr = vaddr & (~mask(X86ISA::PageShift));
            CR3 cr3 = tc->readMiscRegNoEffect(misc_reg::Cr3);
            uint64_t pcid;

            if (cr4.pcide)
                pcid = cr3.pcid;
            else
                pcid = 0x000;

            pageAlignedVaddr = concAddrPcid(pageAlignedVaddr, pcid);
            TlbEntry *entry = lookup(pageAlignedVaddr, true, mode);

            switch (mode) {
                case BaseMMU::Read:
                    stats.rdAccesses++;
                    break;
                case BaseMMU::Write:
                    stats.wrAccesses++;
                    break;
                case BaseMMU::Execute:
                    stats.exAccesses++;
                    break;
                default:
                    panic("Invalid mode\n");
                    break;
            }
            if (!entry) {
                DPRINTF(TLB, "Handling a TLB miss for "
                        "address %#x at pc %#x.\n",
                        vaddr, tc->pcState().instAddr());
                switch (mode) {
                    case BaseMMU::Read:
                        stats.rdMisses++;
                        break;
                    case BaseMMU::Write:
                        stats.wrMisses++;
                        break;
                    case BaseMMU::Execute:
                        stats.exMisses++;
                        break;
                    default:
                        panic("Invalid mode\n");
                        break;
                }
                if (FullSystem && nextLevel()) {
                    auto *shared = static_cast<TLB *>(nextLevel());
                    if (auto *hit = shared->lookup(pageAlignedVaddr)) {
                        if (mode == BaseMMU::Execute && itlbMissStats)
                            ++itlbMissStats->STLB_HIT;
                        if (mode == BaseMMU::Read && timing && translation &&
                            !req->isPrefetch() && dtlbLoadMissStats)
                            ++dtlbLoadMissStats->STLB_HIT;
                        if (mode == BaseMMU::Write && timing && translation &&
                            !req->isPrefetch() && dtlbStoreMissStats)
                            ++dtlbStoreMissStats->STLB_HIT;
                        if (timing && translation && nextLevelHitLatency > Cycles(0)) {
                            auto *cpu = tc->getCpuPtr();
                            const TlbEntry saved = *hit;
                            const auto localGeneration = invalidationGeneration;
                            const auto sharedGeneration = shared->invalidationGeneration;
                            const auto savedCr3 = tc->readMiscRegNoEffect(misc_reg::Cr3);
                            const auto savedCr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
                            const auto savedEfer = tc->readMiscRegNoEffect(misc_reg::Efer);
                            cpu->schedule(new EventFunctionWrapper(
                                [this, shared, req, tc, translation, mode, saved,
                                 pcid, localGeneration, sharedGeneration,
                                 savedCr3, savedCr4, savedEfer] {
                                    if (translation->squashed()) {
                                        if (mode == BaseMMU::Execute)
                                            ++stats.stlbSquashes;
                                        translation->finish(std::make_shared<UnimpFault>(
                                            "Squashed Inst"), req, tc, mode);
                                    } else {
                                        if (localGeneration == invalidationGeneration &&
                                            sharedGeneration == shared->invalidationGeneration &&
                                            savedCr3 == tc->readMiscRegNoEffect(misc_reg::Cr3) &&
                                            savedCr4 == tc->readMiscRegNoEffect(misc_reg::Cr4) &&
                                            savedEfer == tc->readMiscRegNoEffect(misc_reg::Efer)) {
                                            insert(saved.vaddr, saved, pcid, false, mode);
                                            if (mode == BaseMMU::Execute)
                                                ++stats.stlbResponses;
                                        } else if (mode == BaseMMU::Execute) {
                                            ++stats.stlbRestarts;
                                        }
                                        translateTiming(req, tc, translation, mode);
                                    }
                                }, name() + ".stlbResponse", true),
                                cpu->clockEdge(nextLevelHitLatency));
                            delayedResponse = true;
                            return NoFault;
                        }
                        entry = insert(hit->vaddr, *hit, pcid, false, mode);
                    }
                }
                if (!entry && FullSystem) {
                    Fault fault = walker->start(tc, translation, req, mode);
                    if (timing || fault != NoFault) {
                        // This gets ignored in atomic mode.
                        delayedResponse = true;
                        return fault;
                    }
                    entry = lookup(pageAlignedVaddr, true, mode);
                    assert(entry);
                } else if (!entry) {
                    Process *p = tc->getProcessPtr();
                    const EmulationPageTable::Entry *pte =
                        p->pTable->lookup(vaddr);
                    if (!pte) {
                        return std::make_shared<PageFault>(vaddr, true, mode,
                                                           true, false);
                    } else {
                        Addr alignedVaddr = p->pTable->pageAlign(vaddr);
                        DPRINTF(TLB, "Mapping %#x to %#x\n", alignedVaddr,
                                pte->paddr);
                        entry = insert(alignedVaddr, TlbEntry(
                                p->pTable->pid(), alignedVaddr, pte->paddr,
                                pte->flags & EmulationPageTable::Uncacheable,
                                pte->flags & EmulationPageTable::ReadOnly),
                                pcid, true, mode);
                    }
                    DPRINTF(TLB, "Miss was serviced.\n");
                }
            }

            DPRINTF(TLB, "Entry found with paddr %#x, "
                    "doing protection checks.\n", entry->paddr);
            // Do paging protection checks.
            bool inUser = m5Reg.cpl == 3 && !(flags & CPL0FlagBit);
            CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
            Efer efer = tc->readMiscRegNoEffect(misc_reg::Efer);
            bool badWrite = (!entry->writable && (inUser || cr0.wp));
            if ((inUser && !entry->user) ||
                (mode == BaseMMU::Write && badWrite) ||
                (nextLevel() && mode == BaseMMU::Execute &&
                 efer.nxe && entry->noExec)) {
                // The page must have been present to get into the TLB in
                // the first place. We'll assume the reserved bits are
                // fine even though we're not checking them.
                return std::make_shared<PageFault>(vaddr, true, mode, inUser,
                                                   false);
            }
            if (storeCheck && badWrite) {
                // This would fault if this were a write, so return a page
                // fault that reflects that happening.
                return std::make_shared<PageFault>(
                    vaddr, true, BaseMMU::Write, inUser, false);
            }

            Addr paddr = entry->paddr | (vaddr & mask(entry->logBytes));
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, paddr);
            req->setPaddr(paddr);
            if (FullSystem) {
                functionalMapping = entry->functionalTranslation;
                // A restored entry still proves leaf geometry, but no path.
                completeFunctionalTranslationLeaf(functionalMapping,
                    vaddr & ~mask(entry->logBytes), entry->paddr,
                    entry->logBytes, entry->global);
                Efer efer = tc->readMiscRegNoEffect(misc_reg::Efer);
                functionalMapping.address_space_id = efer.lma ?
                    Addr(cr3.longPdtb) << 12 : (cr4.pae ?
                    Addr(cr3.paePdtb) << 5 : Addr(cr3.pdtb) << 12);
                functionalMapping.pcid = pcid;
                functionalLeaf = true;
            }
            if (entry->uncacheable)
                req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            if (req->getFlags() & Request::REP_BULK) {
                const unsigned pat_index = (unsigned(entry->patBit) << 2) |
                    (unsigned(entry->uncacheable) << 1) | unsigned(entry->pwt);
                const RegVal pat = tc->readMiscRegNoEffect(misc_reg::Pat);
                if (((pat >> (8 * pat_index)) & 0xff) != 6)
                    return std::make_shared<RepRestart>();
            }
        } else {
            //Use the address which already has segmentation applied.
            DPRINTF(TLB, "Paging disabled.\n");
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
            req->setPaddr(vaddr);
        }
    } else {
        // Real mode
        DPRINTF(TLB, "In real mode.\n");
        DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
        req->setPaddr(vaddr);
    }

    if (functionalLeaf) {
        auto extension = std::make_shared<FunctionalTranslationExtension>();
        extension->fragment.virtual_address = req->getVaddr();
        extension->fragment.physical_address = req->getPaddr();
        extension->fragment.size = req->getSize();
        extension->fragment.mapping = functionalMapping;
        req->setExtension(extension);
    }
    Fault finalFault = finalizePhysical(req, tc, mode);
    if (finalFault == NoFault && (req->getFlags() & Request::REP_BULK)) {
        const Addr paddr = req->getPaddr();
        if (req->isUncacheable() || req->isStrictlyOrdered() ||
            req->isLocalAccess() || !tc->getSystemPtr()->isMemAddr(paddr) ||
            !tc->getSystemPtr()->isMemAddr(paddr + req->getSize() - 1))
            finalFault = std::make_shared<RepRestart>();
    }
    if (finalFault != NoFault)
        req->removeExtension<FunctionalTranslationExtension>();
    return finalFault;
}

Fault
TLB::translateAtomic(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Mode mode)
{
    bool delayedResponse;
    // CLFLUSHOPT/WB/FLUSH should be treated as read for protection checks
    if (req->isCacheClean())
        mode = BaseMMU::Read;
    return TLB::translate(req, tc, NULL, mode, delayedResponse, false);
}

Fault
TLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Mode mode)
{
    // CLFLUSHOPT/WB/FLUSH should be treated as read for protection checks
    if (req->isCacheClean())
        mode = BaseMMU::Read;
    unsigned logBytes;
    const Addr vaddr = req->getVaddr();
    Addr addr = vaddr;
    Addr paddr = 0;
    if (FullSystem) {
        Fault fault = walker->startFunctional(tc, addr, logBytes, mode);
        if (fault != NoFault)
            return fault;
        paddr = insertBits(addr, logBytes - 1, 0, vaddr);
    } else {
        Process *process = tc->getProcessPtr();
        const auto *pte = process->pTable->lookup(vaddr);

        if (!pte && mode != BaseMMU::Execute) {
            // Check if we just need to grow the stack.
            if (process->fixupFault(vaddr)) {
                // If we did, lookup the entry for the new page.
                pte = process->pTable->lookup(vaddr);
            }
        }

        if (!pte)
            return std::make_shared<PageFault>(vaddr, true, mode, true, false);

        paddr = pte->paddr | process->pTable->pageOffset(vaddr);
    }
    DPRINTF(TLB, "Translated (functional) %#x -> %#x.\n", vaddr, paddr);
    req->setPaddr(paddr);
    return NoFault;
}

void
TLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    bool delayedResponse;
    assert(translation);
    // CLFLUSHOPT/WB/FLUSH should be treated as read for protection checks
    BaseMMU::Mode orig_mode = mode;
    if (req->isCacheClean())
        mode = BaseMMU::Read;
    Fault fault =
        TLB::translate(req, tc, translation, mode, delayedResponse, true);
    if (!delayedResponse)
        translation->finish(fault, req, tc, orig_mode);
    else
        translation->markDelayed();
}

Walker *
TLB::getWalker()
{
    return walker;
}

TLB::TlbStats::TlbStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(rdAccesses, statistics::units::Count::get(),
               "TLB accesses on read requests"),
      ADD_STAT(wrAccesses, statistics::units::Count::get(),
               "TLB accesses on write requests"),
      ADD_STAT(exAccesses, statistics::units::Count::get(),
               "TLB accesses on execute (instr) requests"),
      ADD_STAT(rdMisses, statistics::units::Count::get(),
               "TLB misses on read requests"),
      ADD_STAT(wrMisses, statistics::units::Count::get(),
               "TLB misses on write requests"),
      ADD_STAT(exMisses, statistics::units::Count::get(),
               "TLB misses on execute (instr) requests"),
      ADD_STAT(stlbResponses, statistics::units::Count::get(),
               "Delayed instruction STLB responses delivered to the first-level TLB"),
      ADD_STAT(stlbSquashes, statistics::units::Count::get(),
               "Delayed instruction STLB responses cancelled after squash"),
      ADD_STAT(stlbRestarts, statistics::units::Count::get(),
               "Delayed instruction STLB responses restarted after invalidation or context change")
{
}

void
TLB::serialize(CheckpointOut &cp) const
{
    // Only store the entries in use.
    uint32_t _size = size - freeList.size();
    SERIALIZE_SCALAR(_size);
    SERIALIZE_SCALAR(lruSeq);

    uint32_t _count = 0;
    for (uint32_t x = 0; x < size; x++) {
        if (tlb[x].trieHandle != NULL) {
            if (sprSplitL1) paramOut(cp, csprintf("slot%d", _count), x);
            tlb[x].serializeSection(cp, csprintf("Entry%d", _count++));
        }
    }
    if (sprSplitL1)
        for (unsigned i = 0; i < largePageTree.size(); ++i)
            paramOut(cp, csprintf("largePageTree%d", i), largePageTree[i]);
}

void
TLB::unserialize(CheckpointIn &cp)
{
    // Do not allow to restore with a smaller tlb.
    uint32_t _size;
    UNSERIALIZE_SCALAR(_size);
    if (_size > size) {
        fatal("TLB size less than the one in checkpoint!");
    }

    UNSERIALIZE_SCALAR(lruSeq);
    for (uint32_t x = 0; x < _size; x++) {
        TlbEntry restored;
        restored.unserializeSection(cp, csprintf("Entry%d", x));
        unsigned slot = 0;
        if (sprSplitL1) paramIn(cp, csprintf("slot%d", x), slot);
        TlbEntry *newEntry = sprSplitL1 ? &tlb.at(slot) : freeList.front();
        freeList.remove(newEntry);
        *newEntry = restored;
        TlbEntryTrie &index = sprSplitL1 && type() == TypeTLB::data &&
            slot >= baseSize + 40 ? storeTrie : trie;
        const Addr key = newEntry->global && FullSystem ?
            (newEntry->vaddr & mask(48)) | (Addr(1) << 60) :
            concAddrPcid(newEntry->vaddr, newEntry->pcid);
        newEntry->trieHandle = index.insert(key, FullSystem ?
            TlbEntryTrie::MaxBits - newEntry->logBytes :
            TlbEntryTrie::MaxBits, newEntry);
    }
    if (sprSplitL1)
        for (unsigned i = 0; i < largePageTree.size(); ++i)
            paramIn(cp, csprintf("largePageTree%d", i), largePageTree[i]);
}

Port *
TLB::getTableWalkerPort()
{
    return &walker->getPort("port");
}

} // namespace X86ISA
} // namespace gem5
