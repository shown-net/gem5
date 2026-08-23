from m5.objects.Probe import ProbeListenerObject
from m5.params import *
from m5.util.pybind import *


class RoiInstructionTrace(ProbeListenerObject):
    type = "RoiInstructionTrace"
    cxx_header = "cpu/o3/probe/roi_instruction_trace.hh"
    cxx_class = "gem5::RoiInstructionTrace"
    cxx_exports = [
        PyBindMethod("startTracing"),
        PyBindMethod("stopTracing"),
        PyBindMethod("recordCount"),
    ]

    output_file = Param.String("", "ROI instruction trace output")
    chunk_records = Param.Unsigned(65536, "Records per compressed trace chunk")
    zstd_level = Param.Int(1, "Zstd compression level")
    elf_sha256 = Param.String("", "Executable SHA-256 for trace identity")
    target_exec_start = Param.Addr("Target ELF executable range start")
    target_exec_end = Param.Addr("Target ELF executable range end")
    start_pc = Param.Addr(
        0, "ROI body-start sentinel PC excluded from the trace"
    )
    end_pc = Param.Addr(0, "ROI end marker PC excluded from the trace")
