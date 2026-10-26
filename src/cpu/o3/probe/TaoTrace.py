# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""Commit-point Marker control with optional PC and branch observation."""
from m5.objects.Probe import ProbeListenerObject
from m5.params import Param
from m5.util.pybind import PyBindMethod

class TaoTrace(ProbeListenerObject):
    type = "TaoTrace"
    cxx_class = "gem5::o3::TaoTrace"
    cxx_header = "cpu/o3/probe/tao_trace.hh"
    cxx_exports = [
        PyBindMethod("startMarkerCapture"),
        PyBindMethod("selectMarkerWindow"),
        PyBindMethod("resumeMarkerHandoff"),
    ]
    marker_pc = Param.Addr(0, "Common Marker PC observed at macro retirement")
    marker_pc_trace_file = Param.String("", "Optional committed PC protobuf stream")
    branch_observation = Param.Bool(False, "Observe retired user conditional branches")
    marker_branch_trace_file = Param.String("", "Optional committed branch protobuf stream")
