# Copyright (c) 2007 The Hewlett-Packard Development Company
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
def macroop SAL_R_I
{
    slli reg, reg, imm, flags=(CF,OF,SF,ZF,PF)
};

def macroop SAL_M_I
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    slli t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAL_P_I
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    slli t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAL_1_R
{
    slli reg, reg, 1, flags=(CF,OF,SF,ZF,PF)
};

def macroop SAL_1_M
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    slli t1, t1, 1, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAL_1_P
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    slli t1, t1, 1, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAL_R_R
{
    sllvalue reg, reg, regm
    shiftflags
};

def macroop SAL_M_R
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    sllvalue t1, t1, reg
    shiftflags
    storedata t1
};

def macroop SAL_P_R
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    sllvalue t1, t1, reg
    shiftflags
    storedata t1
};

def macroop SHLD_R_R
{
    mdbi regm, 0
    sldvalue reg, reg, rcx
    shiftflags
};

def macroop SHLD_M_R
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    mdbi reg, 0
    sldvalue t1, t1, rcx
    shiftflags
    storedata t1
};

def macroop SHLD_P_R
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    mdbi reg, 0
    sldvalue t1, t1, rcx
    shiftflags
    storedata t1
};

def macroop SHLD_R_R_I
{
    sldimmediate reg, reg, regm, imm, flags=(CF,OF,SF,ZF,PF)
};

def macroop SHLD_M_R_I
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    mdbi reg, 0
    sldi t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHLD_P_R_I
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    mdbi reg, 0
    sldi t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHR_R_I
{
    srli reg, reg, imm, flags=(CF,OF,SF,ZF,PF)
};

def macroop SHR_M_I
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    srli t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHR_P_I
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    srli t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHR_1_R
{
    srli reg, reg, 1, flags=(CF,OF,SF,ZF,PF)
};

def macroop SHR_1_M
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    srli t1, t1, 1, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHR_1_P
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    srli t1, t1, 1, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHR_R_R
{
    srlvalue reg, reg, regm
    shiftflags
};

def macroop SHR_M_R
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    srlvalue t1, t1, reg
    shiftflags
    storedata t1
};

def macroop SHR_P_R
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    srlvalue t1, t1, reg
    shiftflags
    storedata t1
};

def macroop SHRD_R_R
{
    mdbi regm, 0
    srdvalue reg, reg, rcx
    shiftflags
};

def macroop SHRD_M_R
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    mdbi reg, 0
    srdvalue t1, t1, rcx
    shiftflags
    storedata t1
};

def macroop SHRD_P_R
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    mdbi reg, 0
    srdvalue t1, t1, rcx
    shiftflags
    storedata t1
};

def macroop SHRD_R_R_I
{
    srdimmediate reg, reg, regm, imm, flags=(CF,OF,SF,ZF,PF)
};

def macroop SHRD_M_R_I
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    mdbi reg, 0
    srdi t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SHRD_P_R_I
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    mdbi reg, 0
    srdi t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAR_R_I
{
    srai reg, reg, imm, flags=(CF,OF,SF,ZF,PF)
};

def macroop SAR_M_I
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    srai t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAR_P_I
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    srai t1, t1, imm, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAR_1_R
{
    srai reg, reg, 1, flags=(CF,OF,SF,ZF,PF)
};

def macroop SAR_1_M
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    srai t1, t1, 1, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAR_1_P
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    srai t1, t1, 1, flags=(CF,OF,SF,ZF,PF)
    storedata t1
};

def macroop SAR_R_R
{
    sravalue reg, reg, regm
    shiftflags
};

def macroop SAR_M_R
{
    ldst t1, seg, sib, disp
    sta seg, sib, disp
    sravalue t1, t1, reg
    shiftflags
    storedata t1
};

def macroop SAR_P_R
{
    ldstrip t1, seg, riprel, disp
    starip seg, riprel, disp
    sravalue t1, t1, reg
    shiftflags
    storedata t1
};
"""
