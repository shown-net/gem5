from m5.objects.Probe import ProbeListenerObject
from m5.params import *
from m5.util.pybind import *


class RoiRetireWindow(ProbeListenerObject):
    type = "RoiRetireWindow"
    cxx_class = "gem5::RoiRetireWindow"
    cxx_header = "cpu/o3/probe/roi_retire_window.hh"
    cxx_exports = [
        PyBindMethod("eventKind"),
        PyBindMethod("acknowledge"),
        PyBindMethod("totalInstructions"),
        PyBindMethod("completeWindows"),
    ]

    begin_pc = Param.Addr("ROI begin marker PC")
    end_pc = Param.Addr("ROI end marker PC")
    interval_insts = Param.UInt64("Target retired-instruction interval")
