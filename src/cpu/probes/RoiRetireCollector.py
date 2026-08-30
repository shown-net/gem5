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
    ]

    body_start_pc = Param.Addr("Excluded ROI body-start sentinel PC")
    end_pc = Param.Addr("Excluded ROI end-entry sentinel PC")
    target_exec_start = Param.Addr("Target ELF executable range start")
    target_exec_end = Param.Addr("Target ELF executable range end")
    window_insts = VectorParam.UInt64("Ordered target-user window sizes")
    trace_output_file = Param.String("", "Optional ROI PC trace output")
    trace_elf_sha256 = Param.String(
        "", "Executable SHA-256 for trace identity"
    )
    trace_chunk_records = Param.Unsigned(65536, "Records per trace chunk")
    trace_zstd_level = Param.Int(1, "Zstd compression level")
