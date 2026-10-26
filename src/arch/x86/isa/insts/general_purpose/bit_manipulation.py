# SPDX-License-Identifier: BSD-3-Clause
"""BMI/BMI2 native x86 microcode; VEX operands keep their encoded roles."""

microcode = """
def macroop ANDN_R_R {
    .adjust_env bmiOsz
    andnvalue reg, vreg, regm
};
def macroop ANDN_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    andnvalue reg, vreg, t1
};
def macroop ANDN_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    andnvalue reg, vreg, t1
};
def macroop BEXTR_R_R {
    .adjust_env bmiOsz
    bextrcontrol vreg
    bextrvalue reg, regm
};
def macroop BEXTR_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    bextrcontrol vreg
    bextrvalue reg, t1
};
def macroop BEXTR_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    bextrcontrol vreg
    bextrvalue reg, t1
};
def macroop BZHI_R_R {
    .adjust_env bmiOsz
    bzhivalue reg, regm, vreg
};
def macroop BZHI_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    bzhivalue reg, t1, vreg
};
def macroop BZHI_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    bzhivalue reg, t1, vreg
};
def macroop PDEP_R_R {
    .adjust_env bmiOsz
    pdepvalue reg, vreg, regm
};
def macroop PDEP_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    pdepvalue reg, vreg, t1
};
def macroop PDEP_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    pdepvalue reg, vreg, t1
};
def macroop PEXT_R_R {
    .adjust_env bmiOsz
    pextvalue reg, vreg, regm
};
def macroop PEXT_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    pextvalue reg, vreg, t1
};
def macroop PEXT_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    pextvalue reg, vreg, t1
};
def macroop SHLX_R_R {
    .adjust_env bmiOsz
    shlxvalue reg, regm, vreg
};
def macroop SHLX_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    shlxvalue reg, t1, vreg
};
def macroop SHLX_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    shlxvalue reg, t1, vreg
};
def macroop SHRX_R_R {
    .adjust_env bmiOsz
    shrxvalue reg, regm, vreg
};
def macroop SHRX_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    shrxvalue reg, t1, vreg
};
def macroop SHRX_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    shrxvalue reg, t1, vreg
};
def macroop SARX_R_R {
    .adjust_env bmiOsz
    sarxvalue reg, regm, vreg
};
def macroop SARX_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    sarxvalue reg, t1, vreg
};
def macroop SARX_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    sarxvalue reg, t1, vreg
};
def macroop MULX_R_R {
    .adjust_env bmiOsz
    mulxproduct regm
    mulxlow vreg
    mulxhigh reg
};
def macroop MULX_R_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    mulxproduct t1
    mulxlow vreg
    mulxhigh reg
};
def macroop MULX_R_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    mulxproduct t1
    mulxlow vreg
    mulxhigh reg
};
def macroop MULX_R_R_64 {
    .adjust_env bmiOsz
    mulxwide vreg, regm
    mulxhigh64 reg
};
def macroop MULX_R_M_64 {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    mulxwide vreg, t1
    mulxhigh64 reg
};
def macroop MULX_R_P_64 {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    mulxwide vreg, t1
    mulxhigh64 reg
};
def macroop BLSI_R {
    .adjust_env bmiOsz
    blsivalue vreg, reg
};
def macroop BLSI_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    blsivalue vreg, t1
};
def macroop BLSI_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    blsivalue vreg, t1
};
def macroop BLSMSK_R {
    .adjust_env bmiOsz
    blsmskvalue vreg, reg
};
def macroop BLSMSK_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    blsmskvalue vreg, t1
};
def macroop BLSMSK_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    blsmskvalue vreg, t1
};
def macroop BLSR_R {
    .adjust_env bmiOsz
    blsrvalue vreg, reg
};
def macroop BLSR_M {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    blsrvalue vreg, t1
};
def macroop BLSR_P {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    blsrvalue vreg, t1
};
def macroop RORX_R_R_I {
    .adjust_env bmiOsz
    rorxvalue reg, regm, imm
};
def macroop RORX_R_M_I {
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    rorxvalue reg, t1, imm
};
def macroop RORX_R_P_I {
    .adjust_env bmiOsz
    ldrip t1, seg, riprel, disp
    rorxvalue reg, t1, imm
};
"""
