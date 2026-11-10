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

#ifndef __ARCH_X86_INSTS_STATICINST_HH__
#define __ARCH_X86_INSTS_STATICINST_HH__

#include "arch/x86/pcstate.hh"
#include "arch/x86/regs/ccr.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/types.hh"
#include "base/trace.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/X86.hh"

namespace gem5
{

namespace X86ISA
{

/**
 * Classes for register indices passed to instruction constructors. Using a
 * wrapper struct for these lets take advantage of the compiler's type
 * checking.
 */
struct GpRegIndex
{
    RegIndex index;
    explicit GpRegIndex(RegIndex idx) : index(idx) {}
};

struct FpRegIndex
{
    RegIndex index;
    explicit FpRegIndex(RegIndex idx) : index(idx) {}
};

struct CtrlRegIndex
{
    RegIndex index;
    explicit CtrlRegIndex(RegIndex idx) : index(idx) {}
};

struct CrRegIndex
{
    RegIndex index;
    explicit CrRegIndex(RegIndex idx) : index(idx) {}
};

struct DbgRegIndex
{
    RegIndex index;
    explicit DbgRegIndex(RegIndex idx) : index(idx) {}
};

struct SegRegIndex
{
    RegIndex index;
    explicit SegRegIndex(RegIndex idx) : index(idx) {}
};

/**
 * Base class for all X86 static instructions.
 */

class X86StaticInst : public StaticInst
{
  public:
    bool hasKnownMicrocodeIdentity() const override
    {
        return (machInst.opcode.type == TwoByteOpcode &&
                uint8_t(machInst.opcode.op) == 0xa2) ||
            (machInst.opcode.type == OneByteOpcode &&
             uint8_t(machInst.opcode.op) == 0xfc) ||
            (machInst.opcode.type == OneByteOpcode &&
             (machInst.legacy.rep || machInst.legacy.repne) &&
             ((uint8_t(machInst.opcode.op) >= 0xa4 &&
               uint8_t(machInst.opcode.op) <= 0xa7) ||
              (uint8_t(machInst.opcode.op) >= 0xaa &&
               uint8_t(machInst.opcode.op) <= 0xaf)));
    }
    bool usesMicrocodeService() const override
    { return hasKnownMicrocodeIdentity(); }
    unsigned microcodePrefixOps() const override
    {
        // One cached/decoded invocation operation followed by the CLD
        // body is an effective mixed-source prior, not a measured SPR split.
        return machInst.opcode.type == OneByteOpcode &&
            uint8_t(machInst.opcode.op) == 0xfc ? 1 : 0;
    }
    unsigned softwarePrefetchKind() const override
    {
        if (machInst.opcode.type != TwoByteOpcode || machInst.modRM.mod == 3)
            return 0;
        const auto opcode = uint8_t(machInst.opcode.op);
        if (opcode == 0x0d)
            return machInst.modRM.reg == 1 ? 4 :
                   machInst.modRM.reg == 0 ? 2 : 0;
        if (opcode != 0x18) return 0;
        switch (machInst.modRM.reg) {
          case 0: return 1;
          case 1: return 2;
          case 2: case 3: return 3;
          default: return 0;
        }
    }
    unsigned sseArithmeticKind() const override
    {
        if (machInst.vex.present || machInst.opcode.type != TwoByteOpcode)
            return 0;
        const auto opcode = uint8_t(machInst.opcode.op);
        // Horizontal and alternating operations use F2 for packed single,
        // unlike the scalar-double meaning of F2 on ordinary arithmetic.
        if (opcode == 0x7c || opcode == 0x7d || opcode == 0xd0)
            return machInst.legacy.op ? 3 : machInst.legacy.repne ? 4 : 0;
        if (opcode == 0x53)
            return machInst.legacy.rep ? 2 :
                !machInst.legacy.op && !machInst.legacy.repne ? 4 : 0;
        if (opcode != 0x51 && opcode != 0x58 && opcode != 0x59 &&
            opcode != 0x5c && opcode != 0x5d && opcode != 0x5e &&
            opcode != 0x5f) return 0;
        if (machInst.legacy.repne) return 1;
        if (machInst.legacy.rep) return 2;
        return machInst.legacy.op ? 3 : 4;
    }
    bool maskedSSEDivideAssist64(ExecContext *) const override;
    bool hasLengthChangingPrefix() const override
    {
        // VEX's normalized mandatory prefixes do not change encoded length.
        // Qualify operand-sized immediates only; fixed imm8, ordinary 66,
        // address-size overrides and REX.W alone are not LCP candidates.
        if (machInst.vex.present || !machInst.legacy.op || machInst.rex.w ||
            machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        if ((opcode <= 0x3d && (opcode & 7) == 5) ||
            (opcode >= 0xb8 && opcode <= 0xbf) ||
            opcode == 0x68 || opcode == 0x69 || opcode == 0x81 ||
            opcode == 0xa9 || opcode == 0xc7)
            return machInst.opSize == 2 || machInst.opSize == 4;
        if (opcode == 0xf7 && machInst.modRM.reg <= 1)
            return machInst.opSize == 2 || machInst.opSize == 4;
        return false;
    }
    bool isArchitecturalNop() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        if (machInst.opcode.type == OneByteOpcode)
            return opcode == 0x90 && !machInst.legacy.rep &&
                !(machInst.mode.submode == SixtyFourBitMode && machInst.rex.b);
        if (machInst.opcode.type != TwoByteOpcode) return false;
        // ENDBR32/64 are in the SPR NOP population. PAUSE, prefetch hints
        // and functional helper NOPs are not inferred from their OpClass.
        return (opcode == 0x1f && machInst.modRM.reg == 0) ||
            (opcode == 0x1e && machInst.legacy.rep &&
             (uint8_t(machInst.modRM) == 0xfa || uint8_t(machInst.modRM) == 0xfb));
    }
    bool isArchitecturalX87() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        return machInst.opcode.type == OneByteOpcode &&
            opcode >= 0xd8 && opcode <= 0xdf;
    }
    bool isArchitecturalBranch() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        if (machInst.opcode.type == TwoByteOpcode)
            return (opcode >= 0x80 && opcode <= 0x8f) || opcode == 0x05 ||
                opcode == 0x07 || opcode == 0x34 || opcode == 0x35;
        if (machInst.opcode.type != OneByteOpcode) return false;
        return (opcode >= 0x70 && opcode <= 0x7f) ||
            (opcode >= 0xe0 && opcode <= 0xe3) || opcode == 0xe8 ||
            opcode == 0xe9 || opcode == 0xeb || opcode == 0x9a ||
            opcode == 0xea || opcode == 0xc2 || opcode == 0xc3 ||
            opcode == 0xca || opcode == 0xcb || opcode == 0xcf ||
            (opcode == 0xff && machInst.modRM.reg >= 2 && machInst.modRM.reg <= 5);
    }
    bool isArchitecturalConditionalBranch() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        return isArchitecturalBranch() &&
            ((machInst.opcode.type == OneByteOpcode &&
              ((opcode >= 0x70 && opcode <= 0x7f) ||
               (opcode >= 0xe0 && opcode <= 0xe3))) ||
             (machInst.opcode.type == TwoByteOpcode && opcode >= 0x80 && opcode <= 0x8f));
    }
    bool isArchitecturalNearCall() const override
    {
        if (machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        return opcode == 0xe8 || (opcode == 0xff && machInst.modRM.reg == 2);
    }
    bool isArchitecturalNearReturn() const override
    {
        if (machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        return opcode == 0xc2 || opcode == 0xc3;
    }
    bool isArchitecturalNearIndirectBranch() const override
    {
        return machInst.opcode.type == OneByteOpcode &&
            uint8_t(machInst.opcode.op) == 0xff &&
            (machInst.modRM.reg == 2 || machInst.modRM.reg == 4);
    }
    bool isArchitecturalFarBranch() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        if (machInst.opcode.type == TwoByteOpcode)
            return opcode == 0x05 || opcode == 0x07 ||
                opcode == 0x34 || opcode == 0x35;
        return machInst.opcode.type == OneByteOpcode &&
            (opcode == 0x9a || opcode == 0xea || opcode == 0xca ||
             opcode == 0xcb || opcode == 0xcf ||
             (opcode == 0xff && (machInst.modRM.reg == 3 || machInst.modRM.reg == 5)));
    }
    bool isRasCall() const override
    {
        if (!isCall() || machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        return (opcode == 0xe8 && machInst.immediate != 0) ||
            (opcode == 0xff && machInst.modRM.reg == 2);
    }
    bool isRasReturn() const override
    {
        if (!isReturn() || machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        return opcode == 0xc2 || opcode == 0xc3;
    }

    bool isArchitecturalPortIO() const override
    {
        if (machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        return (opcode >= 0x6c && opcode <= 0x6f) ||
               (opcode >= 0xe4 && opcode <= 0xe7) ||
               (opcode >= 0xec && opcode <= 0xef);
    }

    bool isArchitecturalLockedMemory() const override
    {
        // XCHG with memory is implicitly locked even without a LOCK prefix.
        const auto opcode = uint8_t(machInst.opcode.op);
        return machInst.legacy.lock ||
            (machInst.opcode.type == OneByteOpcode &&
             (opcode == 0x86 || opcode == 0x87) && machInst.modRM.mod != 3);
    }

    bool isFullGprMove() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        return machInst.opcode.type == OneByteOpcode &&
            (opcode == 0x89 || opcode == 0x8b) && machInst.modRM.mod == 3 &&
            machInst.rex.w && machInst.mode.submode == SixtyFourBitMode;
    }

    bool isZeroExtendingGprMove() const override
    {
        const auto opcode = uint8_t(machInst.opcode.op);
        return machInst.opcode.type == OneByteOpcode &&
            (opcode == 0x89 || opcode == 0x8b) && machInst.modRM.mod == 3 &&
            machInst.opSize == 4 && machInst.mode.submode == SixtyFourBitMode;
    }

    bool isByteTestRegisterFusionCandidate() const
    {
        if (machInst.opcode.type != OneByteOpcode) return false;
        const auto opcode = uint8_t(machInst.opcode.op);
        return opcode == 0xa8 ||
            (machInst.modRM.mod == 3 &&
             (opcode == 0x84 || (opcode == 0xf6 && machInst.modRM.reg == 0)));
    }

    bool canMacroFuseWith(const StaticInst &next) const override
    {
        const auto *branch = dynamic_cast<const X86StaticInst *>(&next);
        if (!branch || machInst.opcode.type != OneByteOpcode) return false;
        const auto bop = uint8_t(branch->machInst.opcode.op);
        unsigned condition;
        if (branch->machInst.opcode.type == OneByteOpcode && bop >= 0x70 && bop <= 0x7f)
            condition = bop & 15;
        else if (branch->machInst.opcode.type == TwoByteOpcode && bop >= 0x80 && bop <= 0x8f)
            condition = bop & 15;
        else return false;

        // A bounded flag-producer/Jcc model. Memory and partial-register
        // expansions are excluded by the allocation accountant's one-op gate.
        const auto opcode = uint8_t(machInst.opcode.op);
        unsigned family = 0; // 1=arithmetic/compare, 2=test/and, 3=inc/dec
        if (opcode == 0x01 || opcode == 0x03 || opcode == 0x05 ||
            opcode == 0x29 || opcode == 0x2b || opcode == 0x2d ||
            opcode == 0x39 || opcode == 0x3b || opcode == 0x3d)
            family = 1;
        else if (opcode == 0x21 || opcode == 0x23 || opcode == 0x25 ||
                 opcode == 0x85 || opcode == 0xa9 ||
                 isByteTestRegisterFusionCandidate() ||
                 (opcode == 0xf7 && machInst.modRM.reg == 0))
            family = 2;
        else if (opcode == 0xff && machInst.modRM.reg <= 1)
            family = 3;
        else if (opcode == 0x81 || opcode == 0x83) {
            const auto group = unsigned(machInst.modRM.reg);
            if (group == 0 || group == 5 || group == 7) family = 1;
            else if (group == 4) family = 2;
        }
        if (family == 2) return true;
        if (family == 3) return condition == 4 || condition == 5 || condition >= 12;
        return family == 1 && ((condition >= 2 && condition <= 7) || condition >= 12);
    }
    bool isNearIndirectBranch() const override
    {
        return isIndirectCtrl() && !isReturn() &&
            machInst.opcode.type == OneByteOpcode &&
            uint8_t(machInst.opcode.op) == 0xff &&
            (machInst.modRM.reg == 2 || machInst.modRM.reg == 4);
    }

    RegVal zeroIdiomResult(RegIndex destination) const override
    {
        const auto &reg = destRegIdx(destination);
        return reg.is(CCRegClass) && reg.index() == cc_reg::_ZapsIdx ?
            0x44 /* PF | ZF; the other arithmetic flags are clear. */ : 0;
    }

    IntResultRole intResultRole(RegIndex destination) const override
    {
        const auto &reg = destRegIdx(destination);
        if (reg.is(IntRegClass))
            return IntResultRole::GPR;
        if (reg.is(CCRegClass)) {
            if (reg.index() == cc_reg::_CfIdx)
                return IntResultRole::CF;
            if (reg.index() == cc_reg::_OfIdx)
                return IntResultRole::OF;
            return IntResultRole::Flags;
        }
        return IntResultRole::Other;
    }

    static void printMnemonic(std::ostream &os, const char *mnemonic);
    static void printMnemonic(std::ostream &os, const char *instMnemonic,
            const char *mnemonic);
    static void printMem(std::ostream &os, uint8_t segment,
            uint8_t scale, RegIndex index, RegIndex base,
            uint64_t disp, uint8_t addressSize, bool rip);

    static void printSegment(std::ostream &os, int segment);

    static void printReg(std::ostream &os, RegId reg, int size);

  protected:
    using ExtMachInst = X86ISA::ExtMachInst;

  public:
    ExtMachInst machInst;

  protected:
    // Constructor.
    X86StaticInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass) :
        StaticInst(mnem, __opClass), machInst(_machInst)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;

    static void divideStep(uint64_t divident, uint64_t divisor,
                           uint64_t &quotient, uint64_t &remainder,
                           Fault &fault);

    static void addCheckUnsignedOverflow(uint64_t &value, uint64_t addend,
                                         Fault &fault);

    static inline uint64_t
    merge(uint64_t into, RegIndex index, uint64_t val, int size)
    {
        X86IntReg reg = into;
        if (index & IntFoldBit) {
            reg.H = val;
            return reg;
        }
        switch(size) {
          case 1:
            reg.L = val;
            break;
          case 2:
            reg.X = val;
            break;
          case 4:
            //XXX Check if this should be zeroed or sign extended
            reg = 0;
            reg.E = val;
            break;
          case 8:
            reg.R = val;
            break;
          default:
            panic("Tried to merge with unrecognized size %d.\n", size);
        }
        return reg;
    }

    static inline uint64_t
    pick(uint64_t from, RegIndex index, int size)
    {
        X86IntReg reg = from;
        DPRINTF(X86, "Picking with size %d\n", size);
        if (index & IntFoldBit)
            return reg.H;
        switch(size) {
          case 1:
            return reg.L;
          case 2:
            return reg.X;
          case 4:
            return reg.E;
          case 8:
            return reg.R;
          default:
            panic("Tried to pick with unrecognized size %d.\n", size);
        }
    }

    static inline int64_t
    signedPick(uint64_t from, RegIndex index, int size)
    {
        X86IntReg reg = from;
        DPRINTF(X86, "Picking with size %d\n", size);
        if (index & IntFoldBit)
            return reg.SH;
        switch(size) {
          case 1:
            return reg.SL;
          case 2:
            return reg.SX;
          case 4:
            return reg.SE;
          case 8:
            return reg.SR;
          default:
            panic("Tried to pick with unrecognized size %d.\n", size);
        }
    }

    void
    advancePC(PCStateBase &pcState) const override
    {
        pcState.as<PCState>().advance();
    }

    void
    advancePC(ThreadContext *tc) const override
    {
        PCState pc = tc->pcState().as<PCState>();
        pc.advance();
        tc->pcState(pc);
    }

    std::unique_ptr<PCStateBase>
    buildRetPC(const PCStateBase &cur_pc,
            const PCStateBase &call_pc) const override
    {
        PCStateBase *ret_pc_ptr = call_pc.clone();
        ret_pc_ptr->as<PCState>().uEnd();
        return std::unique_ptr<PCStateBase>{ret_pc_ptr};
    }
};

} // namespace X86ISA
} // namespace gem5

#endif //__ARCH_X86_INSTS_STATICINST_HH__
