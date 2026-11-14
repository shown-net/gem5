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
def macroop MOVAPS_XMM_M {
    ldfpxmm (xmml, xmmh), seg, sib, disp
};

def macroop MOVAPS_XMM_P {
    ldfpxmmrip (xmml, xmmh), seg, riprel, disp
};

def macroop MOVAPS_M_XMM {
    .microfused_store
    sta seg, sib, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVAPS_P_XMM {
    .microfused_store
    starip seg, riprel, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVAPS_XMM_XMM {
    mxmmmov xmml, xmmh, xmmlm, xmmhm, size=8
};

def macroop MOVAPD_XMM_XMM {
    mxmmmov xmml, xmmh, xmmlm, xmmhm, size=8
};

def macroop MOVAPD_XMM_M {
    ldfpxmm (xmml, xmmh), seg, sib, disp
};

def macroop MOVAPD_XMM_P {
    ldfpxmmrip (xmml, xmmh), seg, riprel, disp
};

def macroop MOVAPD_M_XMM {
    .microfused_store
    sta seg, sib, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVAPD_P_XMM {
    .microfused_store
    starip seg, riprel, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVUPS_XMM_XMM {
    mxmmmov xmml, xmmh, xmmlm, xmmhm, size=8
};

def macroop MOVUPS_XMM_M {
    ldfpxmm (xmml, xmmh), seg, sib, disp
};

def macroop MOVUPS_XMM_P {
    ldfpxmmrip (xmml, xmmh), seg, riprel, disp
};

def macroop MOVUPS_M_XMM {
    .microfused_store
    sta seg, sib, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVUPS_P_XMM {
    .microfused_store
    starip seg, riprel, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVUPD_XMM_XMM {
    mxmmmov xmml, xmmh, xmmlm, xmmhm, size=8
};

def macroop MOVUPD_XMM_M {
    ldfpxmm (xmml, xmmh), seg, sib, disp
};

def macroop MOVUPD_XMM_P {
    ldfpxmmrip (xmml, xmmh), seg, riprel, disp
};

def macroop MOVUPD_M_XMM {
    .microfused_store
    sta seg, sib, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVUPD_P_XMM {
    .microfused_store
    starip seg, riprel, disp, dataSize=16
    storedataxmm xmml, xmmh
};

def macroop MOVHPS_XMM_M {
    .microfused_load
    ldfp ufp1, seg, sib, disp, dataSize=8
    mxmmmergehi xmml, xmmh, xmml, ufp1, size=8
};

def macroop MOVHPS_XMM_P {
    .microfused_load
    ldfprip ufp1, seg, riprel, disp, dataSize=8
    mxmmmergehi xmml, xmmh, xmml, ufp1, size=8
};

def macroop MOVHPS_M_XMM {
    stfp xmmh, seg, sib, disp, dataSize=8
};

def macroop MOVHPS_P_XMM {
    stfprip xmmh, seg, riprel, disp, dataSize=8
};

def macroop MOVHPD_XMM_M {
    .microfused_load
    ldfp ufp1, seg, sib, disp, dataSize=8
    mxmmmergehi xmml, xmmh, xmml, ufp1, size=8
};

def macroop MOVHPD_XMM_P {
    .microfused_load
    ldfprip ufp1, seg, riprel, disp, dataSize=8
    mxmmmergehi xmml, xmmh, xmml, ufp1, size=8
};

def macroop MOVHPD_M_XMM {
    stfp xmmh, seg, sib, disp, dataSize=8
};

def macroop MOVHPD_P_XMM {
    stfprip xmmh, seg, riprel, disp, dataSize=8
};

def macroop MOVLPS_XMM_M {
    .microfused_load
    ldfp ufp1, seg, sib, disp, dataSize=8
    mxmmmergelo xmml, xmmh, xmmh, ufp1, size=8
};

def macroop MOVLPS_XMM_P {
    .microfused_load
    ldfprip ufp1, seg, riprel, disp, dataSize=8
    mxmmmergelo xmml, xmmh, xmmh, ufp1, size=8
};

def macroop MOVLPS_M_XMM {
    stfp xmml, seg, sib, disp, dataSize=8
};

def macroop MOVLPS_P_XMM {
    stfprip xmml, seg, riprel, disp, dataSize=8
};

def macroop MOVLPD_XMM_M {
    .microfused_load
    ldfp ufp1, seg, sib, disp, dataSize=8
    mxmmmergelo xmml, xmmh, xmmh, ufp1, size=8
};

def macroop MOVLPD_XMM_P {
    .microfused_load
    ldfprip ufp1, seg, riprel, disp, dataSize=8
    mxmmmergelo xmml, xmmh, xmmh, ufp1, size=8
};

def macroop MOVLPD_M_XMM {
    stfp xmml, seg, sib, disp, dataSize=8
};

def macroop MOVLPD_P_XMM {
    stfprip xmml, seg, riprel, disp, dataSize=8
};

def macroop MOVHLPS_XMM_XMM {
    movfp xmml, xmmhm, dataSize=8
};

def macroop MOVLHPS_XMM_XMM {
    movfp xmmh, xmmlm, dataSize=8
};

def macroop MOVSS_XMM_XMM {
    movfp xmml, xmmlm, dataSize=4
};

def macroop MOVSS_XMM_M {
    ldfpxmm4 (xmml, xmmh), seg, sib, disp
};

def macroop MOVSS_XMM_P {
    ldfpxmm4rip (xmml, xmmh), seg, riprel, disp
};

def macroop MOVSS_M_XMM {
    .microfused_store
    sta seg, sib, disp, dataSize=4
    storedatafp xmml, dataSize=4
};

def macroop MOVSS_P_XMM {
    .microfused_store
    starip seg, riprel, disp, dataSize=4
    storedatafp xmml, dataSize=4
};

def macroop MOVSD_XMM_M {
    ldfpxmm8 (xmml, xmmh), seg, sib, disp
};

def macroop MOVSD_XMM_P {
    ldfpxmm8rip (xmml, xmmh), seg, riprel, disp
};

def macroop MOVSD_M_XMM {
    .microfused_store
    sta seg, sib, disp, dataSize=8
    storedatafp xmml, dataSize=8
};

def macroop MOVSD_P_XMM {
    .microfused_store
    starip seg, riprel, disp, dataSize=8
    storedatafp xmml, dataSize=8
};

def macroop MOVSD_XMM_XMM {
    movfp xmml, xmmlm, dataSize=8
};

def macroop EXTRACTPS_R_XMM_I {
    extractps reg, xmmlm, "IMMEDIATE & mask(2)", size=8
    extractps reg, xmmhm, "IMMEDIATE & mask(2)", size=8, ext=PartHi
};

def macroop EXTRACTPS_M_XMM_I {
    extractps t1, xmmlm, "IMMEDIATE & mask(2)", size=8
    extractps t1, xmmhm, "IMMEDIATE & mask(2)", size=8, ext=PartHi
    st t1, seg, sib, disp
};

def macroop EXTRACTPS_P_XMM_I {
    rdip t7
    extractps t1, xmmlm, "IMMEDIATE & mask(2)", size=8
    extractps t1, xmmhm, "IMMEDIATE & mask(2)", size=8, ext=PartHi
    st t1, seg, riprel, disp
};

def macroop INSERTPS_XMM_XMM_I {
    movfp ufp1, xmml, dataSize=8
    insertps xmml, xmmh, xmmlm, xmmhm, "IMMEDIATE", size=8
    insertps xmmh, ufp1, xmmlm, xmmhm, "IMMEDIATE", size=8, ext=PartHi
};

def macroop INSERTPS_XMM_M_I {
    movfp ufp1, xmml, dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT", dataSize=4
    insertps xmml, xmmh, ufp2, ufp3, "IMMEDIATE & mask(6)", size=8
    insertps xmmh, ufp1, ufp2, ufp3, "IMMEDIATE & mask(6)", size=8, ext=PartHi
};

def macroop INSERTPS_XMM_P_I {
    rdip t7
    movfp ufp1, xmml, dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT", dataSize=4
    insertps xmml, xmmh, ufp2, ufp3, "IMMEDIATE & mask(6)", size=8
    insertps xmmh, ufp1, ufp2, ufp3, "IMMEDIATE & mask(6)", size=8, ext=PartHi
};
"""
