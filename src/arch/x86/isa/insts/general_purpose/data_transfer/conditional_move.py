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
def macroop CMOVZ_R_R
{
    cmovzvalue reg, reg, regm
};

def macroop CMOVZ_R_M
{
    ld t1, seg, sib, disp
    cmovzvalue reg, reg, t1
};

def macroop CMOVZ_R_P
{
    ldrip t1, seg, riprel, disp
    cmovzvalue reg, reg, t1
};

def macroop CMOVNZ_R_R
{
    cmovnzvalue reg, reg, regm
};

def macroop CMOVNZ_R_M
{
    ld t1, seg, sib, disp
    cmovnzvalue reg, reg, t1
};

def macroop CMOVNZ_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnzvalue reg, reg, t1
};

def macroop CMOVB_R_R
{
    cmovbvalue reg, reg, regm
};

def macroop CMOVB_R_M
{
    ld t1, seg, sib, disp
    cmovbvalue reg, reg, t1
};

def macroop CMOVB_R_P
{
    ldrip t1, seg, riprel, disp
    cmovbvalue reg, reg, t1
};

def macroop CMOVNB_R_R
{
    cmovnbvalue reg, reg, regm
};

def macroop CMOVNB_R_M
{
    ld t1, seg, sib, disp
    cmovnbvalue reg, reg, t1
};

def macroop CMOVNB_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnbvalue reg, reg, t1
};

def macroop CMOVBE_R_R {
    conditionbe
    cmovcondition reg, reg, regm
};

def macroop CMOVBE_R_M {
    ld t1, seg, sib, disp
    conditionbe
    cmovcondition reg, reg, t1
};

def macroop CMOVBE_R_P {
    ldrip t1, seg, riprel, disp
    conditionbe
    cmovcondition reg, reg, t1
};

def macroop CMOVNBE_R_R {
    conditionnbe
    cmovcondition reg, reg, regm
};

def macroop CMOVNBE_R_M {
    ld t1, seg, sib, disp
    conditionnbe
    cmovcondition reg, reg, t1
};

def macroop CMOVNBE_R_P {
    ldrip t1, seg, riprel, disp
    conditionnbe
    cmovcondition reg, reg, t1
};

def macroop CMOVS_R_R
{
    cmovsvalue reg, reg, regm
};

def macroop CMOVS_R_M
{
    ld t1, seg, sib, disp
    cmovsvalue reg, reg, t1
};

def macroop CMOVS_R_P
{
    ldrip t1, seg, riprel, disp
    cmovsvalue reg, reg, t1
};

def macroop CMOVNS_R_R
{
    cmovnsvalue reg, reg, regm
};

def macroop CMOVNS_R_M
{
    ld t1, seg, sib, disp
    cmovnsvalue reg, reg, t1
};

def macroop CMOVNS_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnsvalue reg, reg, t1
};

def macroop CMOVP_R_R
{
    cmovpvalue reg, reg, regm
};

def macroop CMOVP_R_M
{
    ld t1, seg, sib, disp
    cmovpvalue reg, reg, t1
};

def macroop CMOVP_R_P
{
    ldrip t1, seg, riprel, disp
    cmovpvalue reg, reg, t1
};

def macroop CMOVNP_R_R
{
    cmovnpvalue reg, reg, regm
};

def macroop CMOVNP_R_M
{
    ld t1, seg, sib, disp
    cmovnpvalue reg, reg, t1
};

def macroop CMOVNP_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnpvalue reg, reg, t1
};

def macroop CMOVL_R_R
{
    cmovlvalue reg, reg, regm
};

def macroop CMOVL_R_M
{
    ld t1, seg, sib, disp
    cmovlvalue reg, reg, t1
};

def macroop CMOVL_R_P
{
    ldrip t1, seg, riprel, disp
    cmovlvalue reg, reg, t1
};

def macroop CMOVNL_R_R
{
    cmovnlvalue reg, reg, regm
};

def macroop CMOVNL_R_M
{
    ld t1, seg, sib, disp
    cmovnlvalue reg, reg, t1
};

def macroop CMOVNL_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnlvalue reg, reg, t1
};

def macroop CMOVLE_R_R
{
    cmovlevalue reg, reg, regm
};

def macroop CMOVLE_R_M
{
    ld t1, seg, sib, disp
    cmovlevalue reg, reg, t1
};

def macroop CMOVLE_R_P
{
    ldrip t1, seg, riprel, disp
    cmovlevalue reg, reg, t1
};

def macroop CMOVNLE_R_R
{
    cmovnlevalue reg, reg, regm
};

def macroop CMOVNLE_R_M
{
    ld t1, seg, sib, disp
    cmovnlevalue reg, reg, t1
};

def macroop CMOVNLE_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnlevalue reg, reg, t1
};

def macroop CMOVO_R_R
{
    cmovovalue reg, reg, regm
};

def macroop CMOVO_R_M
{
    ld t1, seg, sib, disp
    cmovovalue reg, reg, t1
};

def macroop CMOVO_R_P
{
    ldrip t1, seg, riprel, disp
    cmovovalue reg, reg, t1
};

def macroop CMOVNO_R_R
{
    cmovnovalue reg, reg, regm
};

def macroop CMOVNO_R_M
{
    ld t1, seg, sib, disp
    cmovnovalue reg, reg, t1
};

def macroop CMOVNO_R_P
{
    ldrip t1, seg, riprel, disp
    cmovnovalue reg, reg, t1
};
"""
