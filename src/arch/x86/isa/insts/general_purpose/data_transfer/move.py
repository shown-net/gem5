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

#
# Regular moves
#

def macroop MOV_R_MI {
    ld reg, seg, [0, t0, t0], imm
};

def macroop MOV_MI_R {
    limm t1, imm, dataSize=asz
    sta seg, [1, t0, t1]
    storedata reg
};

def macroop MOV_R_R {
    mov reg, reg, regm
};

def macroop MOV_M_R {
    sta seg, sib, disp
    storedata reg
};

def macroop MOV_P_R {
    starip seg, riprel, disp
    storedata reg
};

def macroop MOV_R_M {
    ld reg, seg, sib, disp
};

def macroop MOV_R_P {
    ldrip reg, seg, riprel, disp
};

def macroop MOV_R_I {
    limm reg, imm
};

def macroop MOV_M_I {
    sta seg, sib, disp
    storedatawide imm
};

def macroop MOV_P_I {
    starip seg, riprel, disp
    storedatawide imm
};

#
# Sign extending moves
#

def macroop MOVSXD_R_R {
    sexti reg, regm, "env.dataSize == 2 ? 15 : 31"
};

def macroop MOVSXD_R_M {
    ldsxd reg, seg, sib, disp
};

def macroop MOVSXD_R_P {
    ldsxdrip reg, seg, riprel, disp
};

def macroop MOVSX_B_R_R {
    sextbyte reg, byteregm
};

def macroop MOVSX_B_R_M {
    ldsxb reg, seg, sib, disp
};

def macroop MOVSX_B_R_P {
    ldsxbrip reg, seg, riprel, disp
};

def macroop MOVSX_W_R_R {
    sexti reg, regm, 15
};

def macroop MOVSX_W_R_M {
    ldsxw reg, seg, sib, disp
};

def macroop MOVSX_W_R_P {
    ldsxwrip reg, seg, riprel, disp
};

#
# Zero extending moves
#

def macroop MOVZX_B_R_R {
    zextbyte reg, byteregm
};

def macroop MOVZX_B_R_M {
    ldzxb reg, seg, sib, disp
};

def macroop MOVZX_B_R_P {
    ldzxbrip reg, seg, riprel, disp
};

def macroop MOVZX_W_R_R {
    zexti reg, regm, 15
};

def macroop MOVZX_W_R_M {
    ldzxw reg, seg, sib, disp
};

def macroop MOVZX_W_R_P {
    ldzxwrip reg, seg, riprel, disp
};

def macroop MOV_C_R {
    .serialize_after
    .adjust_env maxOsz
    wrcr cr, regm
};

def macroop MOV_R_C {
    .serialize_after
    .adjust_env maxOsz
    rdcr reg, crm
};

def macroop MOV_D_R {
    .serialize_after
    .adjust_env maxOsz
    wrdr dr, regm
};

def macroop MOV_R_D {
    .adjust_env maxOsz
    rddr reg, drm
};

def macroop MOV_R_S {
    rdsel reg, srm
};

def macroop MOV_M_S {
    rdsel t1, sr
    st t1, seg, sib, disp, dataSize=2
};

def macroop MOV_P_S {
    rdip t7
    rdsel t1, sr
    st t1, seg, riprel, disp, dataSize=2
};

def macroop MOV_REAL_S_R {
    zexti t2, regm, 15, dataSize=8
    slli t3, t2, 4, dataSize=8
    wrsel sr, regm
    wrbase sr, t3, dataSize=8
};

def macroop MOV_REAL_S_M {
    ld t1, seg, sib, disp, dataSize=2
    zexti t2, t1, 15, dataSize=8
    slli t3, t2, 4, dataSize=8
    wrsel sr, t1
    wrbase sr, t3, dataSize=8
};

def macroop MOV_REAL_S_P {
    panic "RIP relative addressing shouldn't happen in real mode"
};

def macroop MOV_S_R {
    andi t0, regm, 0xFC, flags=(EZF,), dataSize=2
    br label("processDescriptor"), flags=(CEZF,)
    andi t2, regm, 0xF8, dataSize=8
    andi t0, regm, 0x4, flags=(EZF,), dataSize=2
    br label("globalDescriptor"), flags=(CEZF,)
    ld t3, tsl, [1, t0, t2], dataSize=8, addressSize=8
    br label("processDescriptor")
globalDescriptor:
    ld t3, tsg, [1, t0, t2], dataSize=8, addressSize=8
processDescriptor:
    chks regm, t3, dataSize=8
    wrdl sr, t3, regm
    wrsel sr, regm
};

def macroop MOV_S_M {
    ld t1, seg, sib, disp, dataSize=2
    andi t0, t1, 0xFC, flags=(EZF,), dataSize=2
    br label("processDescriptor"), flags=(CEZF,)
    andi t2, t1, 0xF8, dataSize=8
    andi t0, t1, 0x4, flags=(EZF,), dataSize=2
    br label("globalDescriptor"), flags=(CEZF,)
    ld t3, tsl, [1, t0, t2], dataSize=8, addressSize=8
    br label("processDescriptor")
globalDescriptor:
    ld t3, tsg, [1, t0, t2], dataSize=8, addressSize=8
processDescriptor:
    chks t1, t3, dataSize=8
    wrdl sr, t3, t1
    wrsel sr, t1
};

def macroop MOV_S_P {
    ldrip t1, seg, riprel, disp, dataSize=2
    andi t0, t1, 0xFC, flags=(EZF,), dataSize=2
    br label("processDescriptor"), flags=(CEZF,)
    andi t2, t1, 0xF8, dataSize=8
    andi t0, t1, 0x4, flags=(EZF,), dataSize=2
    br label("globalDescriptor"), flags=(CEZF,)
    ld t3, tsl, [1, t0, t2], dataSize=8, addressSize=8
    br label("processDescriptor")
globalDescriptor:
    ld t3, tsg, [1, t0, t2], dataSize=8, addressSize=8
processDescriptor:
    chks t1, t3, dataSize=8
    wrdl sr, t3, t1
    wrsel sr, t1
};

def macroop MOVSS_S_R {
    andi t0, regm, 0xFC, flags=(EZF,), dataSize=2
    br label("processDescriptor"), flags=(CEZF,)
    andi t2, regm, 0xF8, dataSize=8
    andi t0, regm, 0x4, flags=(EZF,), dataSize=2
    br label("globalDescriptor"), flags=(CEZF,)
    ld t3, tsl, [1, t0, t2], dataSize=8, addressSize=8
    br label("processDescriptor")
globalDescriptor:
    ld t3, tsg, [1, t0, t2], dataSize=8, addressSize=8
processDescriptor:
    chks regm, t3, SSCheck, dataSize=8
    wrdl sr, t3, regm
    wrsel sr, regm
};

def macroop MOVSS_S_M {
    ld t1, seg, sib, disp, dataSize=2
    andi t0, t1, 0xFC, flags=(EZF,), dataSize=2
    br label("processDescriptor"), flags=(CEZF,)
    andi t2, t1, 0xF8, dataSize=8
    andi t0, t1, 0x4, flags=(EZF,), dataSize=2
    br label("globalDescriptor"), flags=(CEZF,)
    ld t3, tsl, [1, t0, t2], dataSize=8, addressSize=8
    br label("processDescriptor")
globalDescriptor:
    ld t3, tsg, [1, t0, t2], dataSize=8, addressSize=8
processDescriptor:
    chks t1, t3, SSCheck, dataSize=8
    wrdl sr, t3, t1
    wrsel sr, t1
};

def macroop MOVSS_S_P {
    ldrip t1, seg, riprel, disp, dataSize=2
    andi t0, t1, 0xFC, flags=(EZF,), dataSize=2
    br label("processDescriptor"), flags=(CEZF,)
    andi t2, t1, 0xF8, dataSize=8
    andi t0, t1, 0x4, flags=(EZF,), dataSize=2
    br label("globalDescriptor"), flags=(CEZF,)
    ld t3, tsl, [1, t0, t2], dataSize=8, addressSize=8
    br label("processDescriptor")
globalDescriptor:
    ld t3, tsg, [1, t0, t2], dataSize=8, addressSize=8
processDescriptor:
    chks t1, t3, SSCheck, dataSize=8
    wrdl sr, t3, t1
    wrsel sr, t1
};

def macroop MOVNTI_M_R {
    warn_once "MOVNTI: Ignoring non-temporal hint, modeling as cacheable!"
    st reg, seg, sib, disp
};

def macroop MOVNTI_P_R {
    warn_once "MOVNTI_P: Ignoring non-temporal hint, modeling as cacheable!"
    rdip t7
    st reg, seg, riprel, disp
};

def macroop MOVD_XMM_R {
   mov2fp xmml, regm, srcSize=dsz, destSize=8
   lfpimm xmmh, 0
};

def macroop MOVD_XMM_M {
    ldfp xmml, seg, sib, disp, dataSize=dsz
    lfpimm xmmh, 0
};

def macroop MOVD_XMM_P {
    rdip t7
    ldfp xmml, seg, riprel, disp, dataSize=dsz
    lfpimm xmmh, 0
};

def macroop MOVD_R_XMM {
    mov2int reg, xmmlm, size=dsz
};

def macroop MOVD_M_XMM {
    stfp xmml, seg, sib, disp, dataSize=dsz
};

def macroop MOVD_P_XMM {
    stfp xmml, seg, riprel, disp, dataSize=dsz
};

"""
# let {{
#    class MOVD(Inst):
#       "GenFault ${new UnimpInstFault}"
# }};

microcode += """
def macroop MOVSX_B_R_M_16 {
    ld t1, seg, sib, disp, dataSize=1
    sexti reg, t1, 7
};
"""

microcode += """
def macroop MOVSX_B_R_P_16 {
    ldrip t1, seg, riprel, disp, dataSize=1
    sexti reg, t1, 7
};
"""

microcode += """
def macroop MOVSX_W_R_M_16 {
    ld t1, seg, sib, disp, dataSize=2
    sexti reg, t1, 15
};
"""

microcode += """
def macroop MOV_R_M_PARTIAL {
    ld t1, seg, sib, disp
    mov reg, reg, t1
};
def macroop MOV_R_P_PARTIAL {
    ldrip t1, seg, riprel, disp
    mov reg, reg, t1
};
"""

microcode += """
def macroop MOVSX_W_R_P_16 {
    ldrip t1, seg, riprel, disp, dataSize=2
    sexti reg, t1, 15
};
"""

microcode += """
def macroop MOVZX_B_R_M_16 {
    ld t1, seg, sib, disp, dataSize=1
    zexti reg, t1, 7
};
"""

microcode += """
def macroop MOVZX_B_R_P_16 {
    ldrip t1, seg, riprel, disp, dataSize=1
    zexti reg, t1, 7
};
"""

microcode += """
def macroop MOVZX_W_R_M_16 {
    ld t1, seg, sib, disp, dataSize=2
    zexti reg, t1, 15
};
"""

microcode += """
def macroop MOVZX_W_R_P_16 {
    ldrip t1, seg, riprel, disp, dataSize=2
    zexti reg, t1, 15
};
"""

microcode += """
def macroop MOVSXD_R_M_16 {
    ld t1, seg, sib, disp, dataSize=2
    sexti reg, t1, 15
};
"""

microcode += """
def macroop MOVSXD_R_P_16 {
    ldrip t1, seg, riprel, disp, dataSize=2
    sexti reg, t1, 15
};
"""

microcode += """
def macroop CRC32_B_R_R {
    crc32value reg, reg, byteregm, 1
};
def macroop CRC32_B_R_M {
    ld t1, seg, sib, disp, dataSize=1
    crc32value reg, reg, t1, 1
};
def macroop CRC32_B_R_P {
    ldrip t1, seg, riprel, disp, dataSize=1
    crc32value reg, reg, t1, 1
};
def macroop CRC32_V_R_R {
    crc32value reg, reg, regm, crcSourceSize
};
def macroop CRC32_V_R_M {
    ld t1, seg, sib, disp, dataSize=crcSourceSize
    crc32value reg, reg, t1, crcSourceSize
};
def macroop CRC32_V_R_P {
    ldrip t1, seg, riprel, disp, dataSize=crcSourceSize
    crc32value reg, reg, t1, crcSourceSize
};

def macroop MOVBE_R_M {
    ld t1, seg, sib, disp
    bswapvalue reg, t1, dataSize=4
};
def macroop MOVBE_R_M_16 {
    ldzxw t1, seg, sib, disp, dataSize=4
    bswapword t1, t1
    mov reg, reg, t1
};
def macroop MOVBE_R_M_64 {
    ld t1, seg, sib, disp, dataSize=8
    bswaplow t2, t1, dataSize=8
    bswapjoin reg, t1, t2, dataSize=8
};
def macroop MOVBE_M_R {
    sta seg, sib, disp
    bswapvalue t1, reg, dataSize=4
    storedata t1
};
def macroop MOVBE_M_R_16 {
    sta seg, sib, disp, dataSize=2
    bswapword t1, reg
    storedata t1
};
def macroop MOVBE_M_R_64 {
    sta seg, sib, disp, dataSize=8
    bswaplow t1, reg, dataSize=8
    bswapjoin t1, reg, t1, dataSize=8
    storedata t1
};

def macroop MOVBE_R_P {
    ldrip t1, seg, riprel, disp
    bswapvalue reg, t1, dataSize=4
};
def macroop MOVBE_R_P_16 {
    ldzxwrip t1, seg, riprel, disp, dataSize=4
    bswapword t1, t1
    mov reg, reg, t1
};
def macroop MOVBE_R_P_64 {
    ldrip t1, seg, riprel, disp, dataSize=8
    bswaplow t2, t1, dataSize=8
    bswapjoin reg, t1, t2, dataSize=8
};
def macroop MOVBE_P_R {
    starip seg, riprel, disp
    bswapvalue t1, reg, dataSize=4
    storedata t1
};
def macroop MOVBE_P_R_16 {
    starip seg, riprel, disp, dataSize=2
    bswapword t1, reg
    storedata t1
};
def macroop MOVBE_P_R_64 {
    starip seg, riprel, disp, dataSize=8
    bswaplow t1, reg, dataSize=8
    bswapjoin t1, reg, t1, dataSize=8
    storedata t1
};
"""
