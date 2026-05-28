/*
 * Copyright (c) 2014, 2017, 2020, 2023, 2025 Arm Limited
 * All rights reserved
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
 * Copyright (c) 2001-2005 The Regents of The University of Michigan
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

#ifndef __INSTRECORD_HH__
#define __INSTRECORD_HH__

#include <memory>
#include <string>

#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/inst_res.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "params/InstTracer.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class ThreadContext;

namespace trace {

class InstRecord
{
  protected:
    Tick when;

    // The following fields are initialized by the constructor and
    // thus guaranteed to be valid.
    ThreadContext *thread;
    // need to make this ref-counted so it doesn't go away before we
    // dump the record
    StaticInstPtr staticInst;
    std::unique_ptr<PCStateBase> pc;
    StaticInstPtr macroStaticInst;

    // The remaining fields are only valid for particular instruction
    // types (e.g, addresses for memory ops) or when particular
    // options are enabled (e.g., tracing full register contents).
    // Each data field has an associated valid flag to indicate
    // whether the data field is valid.

    /*** @defgroup mem
     * @{
     * Memory request information in the instruction accessed memory.
     * @see mem_valid
     */
    Addr addr = 0; ///< The address that was accessed
    Addr size = 0; ///< The size of the memory request
    unsigned flags = 0; ///< The flags that were assigned to the request.

    /** @} */

    /** @defgroup data
     * If this instruction wrote any data values they're recorded here
     * WARNING: Instructions are quite loose with with what they write
     * since many instructions write multiple values (e.g. destintation
     * register, flags, status, ...) This only captures the last write.
     * @TODO fix this and record all destintations that an instruction writes
     * @see data_status
     */
    union Data
    {
        ~Data() {}
        Data() {}
        uint64_t asInt = 0;
        double asDouble;
        InstResult asReg;
    } data;

    /** @defgroup fetch_seq
     * This records the serial number that the instruction was fetched in.
     * @see fetch_seq_valid
     */
    InstSeqNum fetch_seq = 0;

    /** @defgroup commit_seq
     * This records the instruction number that was committed in the pipeline
     * @see cp_seq_valid
     */
    InstSeqNum cp_seq = 0;

    /** @ingroup data
     * What size of data was written?
     */
    enum DataStatus
    {
        DataInvalid = 0,
        DataInt8 = 1,   // set to equal number of bytes
        DataInt16 = 2,
        DataInt32 = 4,
        DataInt64 = 8,
        DataDouble = 3,
        DataReg = 5
    } dataStatus = DataInvalid;

    /** @ingroup memory
     * Are the memory fields in the record valid?
     */
    bool mem_valid = false;

    /** @ingroup fetch_seq
     * Are the fetch sequence number fields valid?
     */
    bool fetch_seq_valid = false;
    /** @ingroup commit_seq
     * Are the commit sequence number fields valid?
     */
    bool cp_seq_valid = false;

    /** is the predicate for execution this inst true or false (not execed)?
     */
    bool predicate = true;

    /**
     * Did the execution of this instruction fault? (requires ExecFaulting
     * to be enabled)
     */
    bool faulting = false;

    Tick tao_fetch_tick = 0;
    Tick tao_decode_tick = 0;
    Tick tao_rename_tick = 0;
    Tick tao_dispatch_tick = 0;
    Tick tao_issue_tick = 0;
    Tick tao_complete_tick = 0;
    Tick tao_commit_tick = 0;
    Tick tao_store_tick = 0;
    bool tao_stage_ticks_valid = false;

    uint64_t tao_src_reg_bitmap = 0;
    uint64_t tao_dst_reg_bitmap = 0;
    uint64_t tao_src_reg_bitmap_high = 0;
    uint64_t tao_dst_reg_bitmap_high = 0;
    bool tao_reg_bitmap_valid = false;

    bool tao_branch_mispred = false;
    bool tao_branch_mispred_valid = false;
    Addr tao_branch_target = 0;
    bool tao_branch_target_valid = false;
    bool tao_squashed = false;
    bool tao_squashed_valid = false;
    std::string tao_data_access_level;
    bool tao_data_access_level_valid = false;
    bool tao_icache_miss = false;
    bool tao_icache_miss_valid = false;
    bool tao_tlb_miss = false;
    bool tao_tlb_miss_valid = false;
    bool tao_itlb_miss = false;
    bool tao_itlb_miss_valid = false;
    bool tao_dtlb_miss = false;
    bool tao_dtlb_miss_valid = false;
    bool tao_dumped = false;

  public:
    InstRecord(Tick _when, ThreadContext *_thread,
               const StaticInstPtr _staticInst, const PCStateBase &_pc,
               const StaticInstPtr _macroStaticInst=nullptr)
        : when(_when), thread(_thread), staticInst(_staticInst),
        pc(_pc.clone()), macroStaticInst(_macroStaticInst)
    {}

    virtual ~InstRecord()
    {
        if (dataStatus == DataReg)
            data.asReg.~InstResult();
    }

    void setWhen(Tick new_when) { when = new_when; }
    void
    setMem(Addr a, Addr s, unsigned f)
    {
        addr = a;
        size = s;
        flags = f;
        mem_valid = true;
    }

    template <typename T, size_t N>
    void
    setData(std::array<T, N> d)
    {
        data.asInt = d[0];
        dataStatus = (DataStatus)sizeof(T);
        static_assert(sizeof(T) == DataInt8 || sizeof(T) == DataInt16 ||
                      sizeof(T) == DataInt32 || sizeof(T) == DataInt64,
                      "Type T has an unrecognized size.");
    }

    void
    setData(uint64_t d)
    {
        data.asInt = d;
        dataStatus = DataInt64;
    }
    void
    setData(uint32_t d)
    {
        data.asInt = d;
        dataStatus = DataInt32;
    }
    void
    setData(uint16_t d)
    {
        data.asInt = d;
        dataStatus = DataInt16;
    }
    void
    setData(uint8_t d)
    {
        data.asInt = d;
        dataStatus = DataInt8;
    }

    void setData(int64_t d) { setData((uint64_t)d); }
    void setData(int32_t d) { setData((uint32_t)d); }
    void setData(int16_t d) { setData((uint16_t)d); }
    void setData(int8_t d)  { setData((uint8_t)d); }

    void
    setData(double d)
    {
        data.asDouble = d;
        dataStatus = DataDouble;
    }

    void
    setData(const RegClass &reg_class, RegVal val)
    {
        new(&data.asReg) InstResult(reg_class, val);
        switch (reg_class.type()) {
            case IntRegClass:
            case MiscRegClass:
            case CCRegClass:
                dataStatus = DataInt64;
                break;
            case FloatRegClass:
                dataStatus = DataDouble;
                break;
            default:
                dataStatus = DataReg;
                break;
        }
    }

    void
    setData(const RegClass &reg_class, const void *val)
    {
        new(&data.asReg) InstResult(reg_class, val);
        switch (reg_class.type()) {
            case IntRegClass:
            case MiscRegClass:
            case CCRegClass:
                dataStatus = DataInt64;
                break;
            case FloatRegClass:
                dataStatus = DataDouble;
                break;
            default:
                dataStatus = DataReg;
                break;
        }
    }

    void
    setFetchSeq(InstSeqNum seq)
    {
        fetch_seq = seq;
        fetch_seq_valid = true;
    }

    void
    setCPSeq(InstSeqNum seq)
    {
        cp_seq = seq;
        cp_seq_valid = true;
    }

    void setPredicate(bool val) { predicate = val; }

    void setFaulting(bool val) { faulting = val; }

    void
    setTAOStageTicks(Tick fetch, Tick decode, Tick rename, Tick dispatch,
                     Tick issue, Tick complete, Tick commit, Tick store)
    {
        tao_fetch_tick = fetch;
        tao_decode_tick = decode;
        tao_rename_tick = rename;
        tao_dispatch_tick = dispatch;
        tao_issue_tick = issue;
        tao_complete_tick = complete;
        tao_commit_tick = commit;
        tao_store_tick = store;
        tao_stage_ticks_valid = true;
    }

    void
    setTAORegBitmaps(uint64_t src_bitmap, uint64_t dst_bitmap)
    {
        tao_src_reg_bitmap = src_bitmap;
        tao_dst_reg_bitmap = dst_bitmap;
        tao_src_reg_bitmap_high = 0;
        tao_dst_reg_bitmap_high = 0;
        tao_reg_bitmap_valid = true;
    }

    void
    setTAORegBitmaps(uint64_t src_bitmap, uint64_t src_bitmap_high,
                     uint64_t dst_bitmap, uint64_t dst_bitmap_high)
    {
        tao_src_reg_bitmap = src_bitmap;
        tao_src_reg_bitmap_high = src_bitmap_high;
        tao_dst_reg_bitmap = dst_bitmap;
        tao_dst_reg_bitmap_high = dst_bitmap_high;
        tao_reg_bitmap_valid = true;
    }

    void
    setTAOBranchMispred(bool val)
    {
        tao_branch_mispred = val;
        tao_branch_mispred_valid = true;
    }

    void
    setTAOBranchTarget(Addr target)
    {
        tao_branch_target = target;
        tao_branch_target_valid = true;
    }

    void
    setTAOSquashed(bool val)
    {
        tao_squashed = val;
        tao_squashed_valid = true;
    }

    void
    setTAODataAccessLevel(const std::string &level)
    {
        tao_data_access_level = level;
        tao_data_access_level_valid = true;
    }

    void
    setTAOICacheMiss(bool val)
    {
        tao_icache_miss = val;
        tao_icache_miss_valid = true;
    }

    void
    setTAOTLBMiss(bool val)
    {
        tao_tlb_miss = val;
        tao_tlb_miss_valid = true;
    }

    void
    setTAOITLBMiss(bool val)
    {
        tao_itlb_miss = val;
        tao_itlb_miss_valid = true;
        setTAOTLBMiss(tao_tlb_miss || val);
    }

    void
    setTAODTLBMiss(bool val)
    {
        tao_dtlb_miss = val;
        tao_dtlb_miss_valid = true;
        setTAOTLBMiss(tao_tlb_miss || val);
    }

    void setTAODumped() { tao_dumped = true; }
    bool getTAODumped() const { return tao_dumped; }

    virtual void dump() = 0;

  public:
    Tick getWhen() const { return when; }
    ThreadContext *getThread() const { return thread; }
    StaticInstPtr getStaticInst() const { return staticInst; }
    const PCStateBase &getPCState() const { return *pc; }
    StaticInstPtr getMacroStaticInst() const { return macroStaticInst; }

    Addr getAddr() const { return addr; }
    Addr getSize() const { return size; }
    unsigned getFlags() const { return flags; }
    bool getMemValid() const { return mem_valid; }

    uint64_t getIntData() const { return data.asInt; }
    double getFloatData() const { return data.asDouble; }
    int getDataStatus() const { return dataStatus; }

    InstSeqNum getFetchSeq() const { return fetch_seq; }
    bool getFetchSeqValid() const { return fetch_seq_valid; }

    InstSeqNum getCpSeq() const { return cp_seq; }
    bool getCpSeqValid() const { return cp_seq_valid; }

    bool getFaulting() const { return faulting; }
    bool getPredicate() const { return predicate; }

    bool getTAOStageTicksValid() const { return tao_stage_ticks_valid; }
    Tick getTAOFetchTick() const { return tao_fetch_tick; }
    Tick getTAODecodeTick() const { return tao_decode_tick; }
    Tick getTAORenameTick() const { return tao_rename_tick; }
    Tick getTAODispatchTick() const { return tao_dispatch_tick; }
    Tick getTAOIssueTick() const { return tao_issue_tick; }
    Tick getTAOCompleteTick() const { return tao_complete_tick; }
    Tick getTAOCommitTick() const { return tao_commit_tick; }
    Tick getTAOStoreTick() const { return tao_store_tick; }

    bool getTAORegBitmapValid() const { return tao_reg_bitmap_valid; }
    uint64_t getTAOSrcRegBitmap() const { return tao_src_reg_bitmap; }
    uint64_t getTAODstRegBitmap() const { return tao_dst_reg_bitmap; }
    uint64_t getTAOSrcRegBitmapHigh() const { return tao_src_reg_bitmap_high; }
    uint64_t getTAODstRegBitmapHigh() const { return tao_dst_reg_bitmap_high; }

    bool getTAOBranchMispredValid() const { return tao_branch_mispred_valid; }
    bool getTAOBranchMispred() const { return tao_branch_mispred; }
    bool getTAOBranchTargetValid() const { return tao_branch_target_valid; }
    Addr getTAOBranchTarget() const { return tao_branch_target; }
    bool getTAOSquashedValid() const { return tao_squashed_valid; }
    bool getTAOSquashed() const { return tao_squashed; }
    bool getTAODataAccessLevelValid() const { return tao_data_access_level_valid; }
    const std::string &getTAODataAccessLevel() const { return tao_data_access_level; }
    bool getTAOICacheMissValid() const { return tao_icache_miss_valid; }
    bool getTAOICacheMiss() const { return tao_icache_miss; }
    bool getTAOTLBMissValid() const { return tao_tlb_miss_valid; }
    bool getTAOTLBMiss() const { return tao_tlb_miss; }
    bool getTAOITLBMissValid() const { return tao_itlb_miss_valid; }
    bool getTAOITLBMiss() const { return tao_itlb_miss; }
    bool getTAODTLBMissValid() const { return tao_dtlb_miss_valid; }
    bool getTAODTLBMiss() const { return tao_dtlb_miss; }
};

/**
 * The base InstDisassembler class provides a one-API interface
 * to disassemble the instruction passed as a first argument.
 * It also provides a base implementation which is
 * simply calling the StaticInst::disassemble method, which
 * is the usual interface for disassembling
 * a gem5 instruction.
 */
class InstDisassembler : public SimObject
{
  public:
    InstDisassembler(const SimObjectParams &params)
      : SimObject(params)
    {}

    virtual std::string
    disassemble(StaticInstPtr inst,
                const PCStateBase &pc,
                const loader::SymbolTable *symtab) const
    {
        return inst->disassemble(pc.instAddr(), symtab);
    }
};

class InstTracer : public SimObject
{
  public:
    PARAMS(InstTracer);
    InstTracer(const Params &p)
      : SimObject(p), disassembler(p.disassembler)
    {}

    virtual ~InstTracer() {}

    virtual InstRecord *
        getInstRecord(Tick when, ThreadContext *tc,
                const StaticInstPtr staticInst, const PCStateBase &pc,
                const StaticInstPtr macroStaticInst=nullptr) = 0;

    virtual InstRecord *
    getInstRecord(Tick when, ThreadContext *tc, const ExecContext *xc,
                  const StaticInstPtr staticInst, const PCStateBase &pc,
                  const StaticInstPtr macroStaticInst = nullptr)
    {
        return getInstRecord(when, tc, staticInst, pc, macroStaticInst);
    };

    std::string
    disassemble(StaticInstPtr inst,
                const PCStateBase &pc,
                const loader::SymbolTable *symtab=nullptr) const
    {
        return disassembler->disassemble(inst, pc, symtab);
    }

  private:
    InstDisassembler *disassembler;
};

} // namespace trace
} // namespace gem5

#endif // __INSTRECORD_HH__
