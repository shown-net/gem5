# Copyright (c) 2007-2008 The Hewlett-Packard Development Company
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2008 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

microcode = """
def macroop BT_R_I {
    btvaluei t0, reg, imm
};

def macroop BT_M_I {
    ld t1, seg, sib, disp
    btvaluei t0, t1, imm
};

def macroop BT_P_I {
    ldrip t1, seg, riprel, disp
    btvaluei t0, t1, imm
};

def macroop BT_R_R {
    btvalue t0, reg, regm
};

def macroop BT_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    ld t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
};

def macroop BT_P_R {
    rdip t7
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    ld t1, seg, [dsz, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
};

def macroop BTC_R_I {
    btcvaluei reg, reg, imm
};

def macroop BTC_M_I {
    limm t1, imm, dataSize=asz
    # This fudges just a tiny bit, but it's reasonable to expect the
    # microcode generation logic to have the log of the various sizes
    # floating around as well.
    limm t4, 1
    roli t4, t4, imm
    ldst t1, seg, sib, disp
    sexti t0, t1, imm, flags=(CF,)
    xor t1, t1, t4
    st t1, seg, sib, disp
};

def macroop BTC_P_I {
    rdip t7, dataSize=asz
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    ldst t1, seg, riprel, disp
    sexti t0, t1, imm, flags=(CF,)
    xor t1, t1, t4
    st t1, seg, riprel, disp
};

def macroop BTC_LOCKED_M_I {
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    mfence
    ldstl t1, seg, sib, disp
    sexti t0, t1, imm, flags=(CF,)
    xor t1, t1, t4
    stul t1, seg, sib, disp
    mfence
};

def macroop BTC_LOCKED_P_I {
    rdip t7, dataSize=asz
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    mfence
    ldstl t1, seg, riprel, disp
    sexti t0, t1, imm, flags=(CF,)
    xor t1, t1, t4
    stul t1, seg, riprel, disp
    mfence
};

def macroop BTC_R_R {
    btcvalue reg, reg, regm
};

def macroop BTC_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    ldst t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
    xor t1, t1, t4
    st t1, seg, [scale, index, t3], disp
};

def macroop BTC_P_R {
    rdip t7, dataSize=asz
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    ldst t1, seg, [dsz, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
    xor t1, t1, t4
    st t1, seg, [dsz, t3, t7], disp
};

def macroop BTC_LOCKED_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    mfence
    ldstl t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
    xor t1, t1, t4
    stul t1, seg, [scale, index, t3], disp
    mfence
};

def macroop BTC_LOCKED_P_R {
    rdip t7, dataSize=asz
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    mfence
    ldstl t1, seg, [dsz, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
    xor t1, t1, t4
    stul t1, seg, [dsz, t3, t7], disp
    mfence
};

def macroop BTR_R_I {
    btrvaluei reg, reg, imm
};

def macroop BTR_M_I {
    limm t1, imm, dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    roli t4, t4, imm
    ldst t1, seg, sib, disp
    sexti t0, t1, imm, flags=(CF,)
    and t1, t1, t4
    st t1, seg, sib, disp
};

def macroop BTR_P_I {
    rdip t7, dataSize=asz
    limm t1, imm, dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    roli t4, t4, imm
    ldst t1, seg, riprel, disp
    sexti t0, t1, imm, flags=(CF,)
    and t1, t1, t4
    st t1, seg, riprel, disp
};

def macroop BTR_LOCKED_M_I {
    limm t1, imm, dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    roli t4, t4, imm
    mfence
    ldstl t1, seg, sib, disp
    sexti t0, t1, imm, flags=(CF,)
    and t1, t1, t4
    stul t1, seg, sib, disp
    mfence
};

def macroop BTR_LOCKED_P_I {
    rdip t7, dataSize=asz
    limm t1, imm, dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    roli t4, t4, imm
    mfence
    ldstl t1, seg, riprel, disp
    sexti t0, t1, imm, flags=(CF,)
    and t1, t1, t4
    stul t1, seg, riprel, disp
    mfence
};

def macroop BTR_R_R {
    btrvalue reg, reg, regm
};

def macroop BTR_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    rol t4, t4, reg
    ldst t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
    and t1, t1, t4
    st t1, seg, [scale, index, t3], disp
};

def macroop BTR_P_R {
    rdip t7, dataSize=asz
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    rol t4, t4, reg
    ldst t1, seg, [dsz, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
    and t1, t1, t4
    st t1, seg, [dsz, t3, t7], disp
};

def macroop BTR_LOCKED_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    rol t4, t4, reg
    mfence
    ldstl t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
    and t1, t1, t4
    stul t1, seg, [scale, index, t3], disp
    mfence
};

def macroop BTR_LOCKED_P_R {
    rdip t7, dataSize=asz
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    limm t4, "(uint64_t(-(2ULL)))"
    rol t4, t4, reg
    mfence
    ldstl t1, seg, [dsz, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
    and t1, t1, t4
    stul t1, seg, [dsz, t3, t7], disp
    mfence
};

def macroop BTS_R_I {
    btsvaluei reg, reg, imm
};

def macroop BTS_M_I {
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    ldst t1, seg, sib, disp
    sexti t0, t1, imm, flags=(CF,)
    or t1, t1, t4
    st t1, seg, sib, disp
};

def macroop BTS_P_I {
    rdip t7, dataSize=asz
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    ldst t1, seg, riprel, disp
    sexti t0, t1, imm, flags=(CF,)
    or t1, t1, t4
    st t1, seg, riprel, disp
};

def macroop BTS_LOCKED_M_I {
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    mfence
    ldstl t1, seg, sib, disp
    sexti t0, t1, imm, flags=(CF,)
    or t1, t1, t4
    stul t1, seg, sib, disp
    mfence
};

def macroop BTS_LOCKED_P_I {
    rdip t7, dataSize=asz
    limm t1, imm, dataSize=asz
    limm t4, 1
    roli t4, t4, imm
    mfence
    ldstl t1, seg, riprel, disp
    sexti t0, t1, imm, flags=(CF,)
    or t1, t1, t4
    stul t1, seg, riprel, disp
    mfence
};

def macroop BTS_R_R {
    btsvalue reg, reg, regm
};

def macroop BTS_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    ldst t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
    or t1, t1, t4
    st t1, seg, [scale, index, t3], disp
};

def macroop BTS_P_R {
    rdip t7, dataSize=asz
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    ldst t1, seg, [1, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
    or t1, t1, t4
    st t1, seg, [1, t3, t7], disp
};

def macroop BTS_LOCKED_M_R {
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    mfence
    ldstl t1, seg, [scale, index, t3], disp
    sext t0, t1, reg, flags=(CF,)
    or t1, t1, t4
    stul t1, seg, [scale, index, t3], disp
    mfence
};

def macroop BTS_LOCKED_P_R {
    rdip t7, dataSize=asz
    srai t2, reg, 3, dataSize=asz
    srai t3, t2, ldsz, dataSize=asz
    lea t3, flatseg, [dsz, t3, base], dataSize=asz
    limm t4, 1
    rol t4, t4, reg
    mfence
    ldstl t1, seg, [1, t3, t7], disp
    sext t0, t1, reg, flags=(CF,)
    or t1, t1, t4
    stul t1, seg, [1, t3, t7], disp
    mfence
};
"""
