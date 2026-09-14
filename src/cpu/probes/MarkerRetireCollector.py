from m5.objects.Probe import ProbeListenerObject
from m5.params import *
from m5.util.pybind import *


class MarkerRetireCollector(ProbeListenerObject):
    type = "MarkerRetireCollector"
    cxx_class = "gem5::MarkerRetireCollector"
    cxx_header = "cpu/probes/marker_retire_collector.hh"
    cxx_exports = [
        PyBindMethod("acknowledge"),
    ]

    marker_pc = Param.Addr("Marker boundary PC")
    trace_image_base = Param.Addr(
        "Target ELF image base for PC trace normalization"
    )
    trace_exec_segments = VectorParam.Addr(
        "Target ELF executable segment start/end pairs for PC trace "
        "normalization"
    )
    trace_output_file = Param.String("", "Optional PC trace output")
    trace_elf_sha256 = Param.String(
        "", "Executable SHA-256 for trace identity"
    )
    trace_chunk_records = Param.Unsigned(65536, "Records per trace chunk")
    trace_zstd_level = Param.Int(1, "Zstd compression level")
