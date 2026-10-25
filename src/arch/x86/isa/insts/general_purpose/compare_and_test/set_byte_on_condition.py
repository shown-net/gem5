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
def macroop SALC_R
{
    sbb reg, reg, reg, dataSize=1
};

def macroop SETZ_R
{
    setzvalue reg, reg, dataSize=1
};

def macroop SETZ_M
{
    sta seg, sib, disp, dataSize=1
    setzvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETZ_P
{
    starip seg, riprel, disp, dataSize=1
    setzvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNZ_R
{
    setnzvalue reg, reg, dataSize=1
};

def macroop SETNZ_M
{
    sta seg, sib, disp, dataSize=1
    setnzvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNZ_P
{
    starip seg, riprel, disp, dataSize=1
    setnzvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETB_R
{
    setbvalue reg, reg, dataSize=1
};

def macroop SETB_M
{
    sta seg, sib, disp, dataSize=1
    setbvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETB_P
{
    starip seg, riprel, disp, dataSize=1
    setbvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNB_R
{
    setnbvalue reg, reg, dataSize=1
};

def macroop SETNB_M
{
    sta seg, sib, disp, dataSize=1
    setnbvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNB_P
{
    starip seg, riprel, disp, dataSize=1
    setnbvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETBE_R {
    conditionbe
    setcondition reg, reg, dataSize=1
};

def macroop SETBE_M
{
    sta seg, sib, disp, dataSize=1
    conditionbe
    setcondition t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETBE_P
{
    starip seg, riprel, disp, dataSize=1
    conditionbe
    setcondition t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNBE_R {
    conditionnbe
    setcondition reg, reg, dataSize=1
};

def macroop SETNBE_M
{
    sta seg, sib, disp, dataSize=1
    conditionnbe
    setcondition t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNBE_P
{
    starip seg, riprel, disp, dataSize=1
    conditionnbe
    setcondition t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETS_R
{
    setsvalue reg, reg, dataSize=1
};

def macroop SETS_M
{
    sta seg, sib, disp, dataSize=1
    setsvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETS_P
{
    starip seg, riprel, disp, dataSize=1
    setsvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNS_R
{
    setnsvalue reg, reg, dataSize=1
};

def macroop SETNS_M
{
    sta seg, sib, disp, dataSize=1
    setnsvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNS_P
{
    starip seg, riprel, disp, dataSize=1
    setnsvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETP_R
{
    setpvalue reg, reg, dataSize=1
};

def macroop SETP_M
{
    sta seg, sib, disp, dataSize=1
    setpvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETP_P
{
    starip seg, riprel, disp, dataSize=1
    setpvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNP_R
{
    setnpvalue reg, reg, dataSize=1
};

def macroop SETNP_M
{
    sta seg, sib, disp, dataSize=1
    setnpvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNP_P
{
    starip seg, riprel, disp, dataSize=1
    setnpvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETL_R
{
    setlvalue reg, reg, dataSize=1
};

def macroop SETL_M
{
    sta seg, sib, disp, dataSize=1
    setlvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETL_P
{
    starip seg, riprel, disp, dataSize=1
    setlvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNL_R
{
    setnlvalue reg, reg, dataSize=1
};

def macroop SETNL_M
{
    sta seg, sib, disp, dataSize=1
    setnlvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNL_P
{
    starip seg, riprel, disp, dataSize=1
    setnlvalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETLE_R
{
    setlevalue reg, reg, dataSize=1
};

def macroop SETLE_M
{
    sta seg, sib, disp, dataSize=1
    setlevalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETLE_P
{
    starip seg, riprel, disp, dataSize=1
    setlevalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNLE_R
{
    setnlevalue reg, reg, dataSize=1
};

def macroop SETNLE_M
{
    sta seg, sib, disp, dataSize=1
    setnlevalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNLE_P
{
    starip seg, riprel, disp, dataSize=1
    setnlevalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETO_R
{
    setovalue reg, reg, dataSize=1
};

def macroop SETO_M
{
    sta seg, sib, disp, dataSize=1
    setovalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETO_P
{
    starip seg, riprel, disp, dataSize=1
    setovalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNO_R
{
    setnovalue reg, reg, dataSize=1
};

def macroop SETNO_M
{
    sta seg, sib, disp, dataSize=1
    setnovalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};

def macroop SETNO_P
{
    starip seg, riprel, disp, dataSize=1
    setnovalue t1, t0, dataSize=8
    storedata t1, dataSize=1
};
"""
