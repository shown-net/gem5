# Copyright 2021 Google, Inc.
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

from m5.objects.InstDecoder import InstDecoder
from m5.params import Param


class X86Decoder(InstDecoder):
    type = "X86Decoder"
    cxx_class = "gem5::X86ISA::Decoder"
    cxx_header = "arch/x86/decoder.hh"
    singleResultImul = Param.Bool(False, "Use one result-producing uop for register IMUL")
    singleResultImulForms = Param.Bool(False, "Use direct-result IMUL for memory and 32/64-bit immediate forms")
    imul16ImmediateMode = Param.Unsigned(0, "16-bit immediate IMUL: 0 original, 1 direct, 2 multiply then merge")
    compactIntDiv = Param.Bool(False, "Use fixed-footprint register DIV/IDIV behavior proxy for 32/64-bit forms")
    independentDivsd = Param.Bool(False, "Route register DIVSD to its dedicated execution resource")
    gprZeroIdiomMode = Param.Unsigned(
        0,
        "GPR XOR/SUB same-register behavior: 0 original, 1 dependency-only, "
        "2 dependency plus abstract completion-resource proxy",
    )
    singleUopSetz = Param.Bool(False, "Use one result uop for register SETZ")
    zfOnlySetz = Param.Bool(False, "Read only Zaps for register SETZ")
