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

categories = ["integer", "floating_point"]

microcode = """
# SSE instructions
"""
for category in categories:
    exec(f"from . import {category} as cat")
    microcode += cat.microcode

# Full-width lane-independent SSE operations have one execution record.
# Memory operands retain a separate load record and share allocation where
# Golden Cove supports load-operation fusion. Shuffle keeps its own two slots.
import re

_families = {
    **{f"PADD{suffix}": ("mxmmaddi", size, 0)
       for suffix, size in (("B", 1), ("W", 2), ("D", 4), ("Q", 8))},
    **{f"PCMPEQ{suffix}": ("mxmmcmpi2r", size, 0)
       for suffix, size in (("B", 1), ("W", 2), ("D", 4))},
    **{f"PCMPGT{suffix}": ("mxmmcmpi2r", size, 2)
       for suffix, size in (("B", 1), ("W", 2), ("D", 4), ("Q", 8))},
    **{f"{family}{suffix}": (op, size, '"2 |" + Signed')
       for family, op in (("PADDS", "mxmmaddi"), ("PSUBS", "mxmmsubi"))
       for suffix, size in (("B", 1), ("W", 2))},
    **{f"{family}{suffix}": (op, size, 2)
       for family, op in (("PADDUS", "mxmmaddi"), ("PSUBUS", "mxmmsubi"))
       for suffix, size in (("B", 1), ("W", 2))},
    **{f"{family}S{suffix}": (op, size, "Signed")
       for family, op in (("PMIN", "mxmmmini"), ("PMAX", "mxmmmaxi"))
       for suffix, size in (("B", 1), ("W", 2), ("D", 4))},
    **{f"{family}U{suffix}": (op, size, 0)
       for family, op in (("PMIN", "mxmmmini"), ("PMAX", "mxmmmaxi"))
       for suffix, size in (("B", 1), ("W", 2), ("D", 4))},
    "PAVGB": ("mxmmavg", 1, 0),
    "PAVGW": ("mxmmavg", 2, 0),
    "PSADBW": ("mxmmsad", 1, 0),
    "PMULLW": ("mxmmmuli", 2, 0),
    "PMULHW": ("mxmmmuli", 2, 'Signed + "|" + MultHi'),
    "PMULHUW": ("mxmmmuli", 2, "MultHi"),
    "PMULUDQ": ("mxmmmuli", 4, "Scalar"),
}


def _full_width(match):
    name, body = match.group(1), match.group(2)
    parts = name.split("_")
    family, form = parts[0], parts[-1]
    if family not in _families:
        return match.group(0)
    op, size, ext = _families[family]
    sizes = ({"PMULUDQ": "srcSize=4, destSize=8",
              "PSADBW": "srcSize=1, destSize=2"}.get(family, f"size={size}"))
    if form == "XMM":
        body = f"{op} xmml, xmmh, xmml, xmmh, xmmlm, xmmhm, {sizes}, ext={ext}"
    else:
        load = "ldfpxmmrip" if form == "P" else "ldfpxmm"
        address = "riprel" if form == "P" else "sib"
        body = (f".microfused_load\n    {load} (ufp1, ufp2), seg, {address}, disp\n"
                f"    {op} xmml, xmmh, xmml, xmmh, ufp1, ufp2, {sizes}, ext={ext}")
    return f"def macroop {name} {{\n    {body}\n}};"


microcode = re.sub(r"def macroop (\w+) \{(.*?)\};", _full_width,
                   microcode, flags=re.S)


def _multiply_dword(match):
    name = match.group(1)
    if name.split("_")[0] != "PMULLD":
        return match.group(0)
    form = name.split("_")[-1]
    body, low, high = "", "xmmlm", "xmmhm"
    if form in ("M", "P"):
        load = "ldfpxmmrip" if form == "P" else "ldfpxmm"
        address = "riprel" if form == "P" else "sib"
        body = f".microfused_load\n    {load} (ufp1, ufp2), seg, {address}, disp\n    "
        low, high = "ufp1", "ufp2"
    body += (f"mmulld xmml, xmml, {low}, size=4, ext=Signed\n"
             f"    mmulld xmmh, xmmh, {high}, size=4, ext=Signed")
    return f"def macroop {name} {{\n    {body}\n}};"


microcode = re.sub(r"def macroop (\w+) \{(.*?)\};", _multiply_dword,
                   microcode, flags=re.S)


def _full_shift(match):
    name = match.group(1)
    parts = name.split("_")
    family, form = parts[0], parts[-1]
    if family not in {"PSLLW", "PSLLD", "PSLLQ", "PSRLW", "PSRLD", "PSRLQ",
                      "PSRAW", "PSRAD"}:
        return match.group(0)
    size = {"W": 2, "D": 4, "Q": 8}[family[-1]]
    op = "mxmm" + {"PSLL": "sll", "PSRL": "srl", "PSRA": "sra"}[family[:-1]]
    body = ""
    if form == "I":
        op += "i"
        count = "imm"
    elif form == "XMM":
        body = "mxmmshiftcount ufp1, xmmlm, size=8\n    "
        count = "ufp1"
    else:
        load = "ldfpxmmrip" if form == "P" else "ldfpxmm"
        address = "riprel" if form == "P" else "sib"
        body = f".microfused_load\n    {load} (ufp1, ufp2), seg, {address}, disp\n    "
        count = "ufp1"
    body += f"{op} xmml, xmmh, xmml, xmmh, {count}, size={size}, ext=0"
    return f"def macroop {name} {{\n    {body}\n}};"


microcode = re.sub(r"def macroop (\w+) \{(.*?)\};", _full_shift,
                   microcode, flags=re.S)


def _full_unpack(match):
    name = match.group(1)
    family, form = name.split("_")[0], name.split("_")[-1]
    if not family.startswith("PUNPCK"):
        return match.group(0)
    suffix = family[7:]
    sizes = {"BW": 1, "WD": 2, "DQ": 4, "QDQ": 8}
    if suffix not in sizes:
        return match.group(0)
    high = int(family[6] == "H")
    body = ""
    source = "xmmlm, xmmhm"
    if form in ("M", "P"):
        load = "ldfpxmmrip" if form == "P" else "ldfpxmm"
        address = "riprel" if form == "P" else "sib"
        body = f".microfused_load\n    {load} (ufp1, ufp2), seg, {address}, disp\n    "
        source = "ufp1, ufp2"
    body += f"mxmmunpack xmml, xmmh, xmml, xmmh, {source}, size={sizes[suffix]}, ext={high}"
    return f"def macroop {name} {{\n    {body}\n}};"


microcode = re.sub(r"def macroop (\w+) \{(.*?)\};", _full_unpack,
                   microcode, flags=re.S)


def _pack_mask_shuffle(match):
    name = match.group(1)
    parts = name.split("_")
    family, form = parts[0], parts[2] if len(parts) > 2 else ""
    if family in {"PMOVMSKB", "MOVMSKPS", "MOVMSKPD"}:
        size = {"PMOVMSKB": 1, "MOVMSKPS": 4, "MOVMSKPD": 8}[family]
        body = f"mxmmsign reg, xmmlm, xmmhm, size={size}"
    elif family in {"PACKSSDW", "PACKSSWB", "PACKUSDW", "PACKUSWB"}:
        size = 4 if family.endswith("DW") else 2
        signed = "Signed" if family.startswith("PACKSS") else 0
        body, source = "", "xmmlm, xmmhm"
        if form in ("M", "P"):
            load = "ldfpxmmrip" if form == "P" else "ldfpxmm"
            address = "riprel" if form == "P" else "sib"
            body = f".microfused_load\n    {load} (ufp1, ufp2), seg, {address}, disp\n    "
            source = "ufp1, ufp2"
        body += (f"mxmmpack xmml, xmmh, xmml, xmmh, {source}, "
                 f"srcSize={size}, destSize={size // 2}, ext={signed}")
    elif family in {"PSHUFLW", "PSHUFHW"}:
        body, source = "", "xmmlm, xmmhm"
        if form in ("M", "P"):
            load = "ldfpxmmrip" if form == "P" else "ldfpxmm"
            address = "riprel" if form == "P" else "sib"
            body = f"{load} (ufp1, ufp2), seg, {address}, disp\n    "
            source = "ufp1, ufp2"
        high = 256 if family == "PSHUFHW" else 0
        body += f'mxmmwordshuffle xmml, xmmh, {source}, size=2, ext="IMMEDIATE | {high}"'
    else:
        return match.group(0)
    return f"def macroop {name} {{\n    {body}\n}};"


microcode = re.sub(r"def macroop (\w+) \{(.*?)\};", _pack_mask_shuffle,
                   microcode, flags=re.S)
