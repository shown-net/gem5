# SPDX-License-Identifier: BSD-3-Clause
"""Selected BMI1 operations; VEX source and destination roles stay explicit."""

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
    rdip t7
    ld t1, seg, riprel, disp
    andnvalue reg, vreg, t1
};
"""

for name, operation in (("BLSI", "blsivalue"), ("BLSMSK", "blsmskvalue"),
                        ("BLSR", "blsrvalue")):
    microcode += f"""
def macroop {name}_R {{
    .adjust_env bmiOsz
    {operation} vreg, reg
}};
def macroop {name}_M {{
    .adjust_env bmiOsz
    ld t1, seg, sib, disp
    {operation} vreg, t1
}};
def macroop {name}_P {{
    .adjust_env bmiOsz
    rdip t7
    ld t1, seg, riprel, disp
    {operation} vreg, t1
}};
"""
