# SPDX-License-Identifier: BSD-3-Clause
from m5.SimObject import SimObject


class ZeroIdiom(SimObject):
    type = "ZeroIdiom"
    cxx_class = "gem5::o3::ZeroIdiom"
    cxx_header = "cpu/o3/zero_idiom.hh"
