from m5.objects.Probe import ProbeListenerObject
from m5.params import *
from m5.util.pybind import *


class RoiRetireCollector(ProbeListenerObject):
    type = "RoiRetireCollector"
    cxx_class = "gem5::RoiRetireCollector"
    cxx_header = "cpu/probes/roi_retire_collector.hh"
    cxx_exports = [
        PyBindMethod("acknowledge"),
        PyBindMethod("retiredInstructions"),
        PyBindMethod("workloadUserInstructions"),
    ]

    begin_pc = Param.Addr("Excluded ROI begin sentinel PC")
    end_pc = Param.Addr("Excluded ROI end sentinel PC")
    trace_image_base = Param.Addr(
        "Target ELF image base for PC trace normalization"
    )
    trace_exec_segments = VectorParam.Addr(
        "Target ELF executable segment start/end pairs for PC trace normalization"
    )
    window_insts = Param.UInt64(
        0,
        "Fixed workload-user instruction window size; zero disables fixed windows",
    )
    window_schedule = VectorParam.UInt64(
        [],
        "Ordered workload-user window sizes; mutually exclusive with window_insts",
    )
    trace_output_file = Param.String("", "Optional ROI PC trace output")
    trace_elf_sha256 = Param.String(
        "", "Executable SHA-256 for trace identity"
    )
    trace_chunk_records = Param.Unsigned(65536, "Records per trace chunk")
    trace_zstd_level = Param.Int(1, "Zstd compression level")
