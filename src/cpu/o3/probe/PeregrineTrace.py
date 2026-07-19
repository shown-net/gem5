from m5.objects.Probe import *


class PeregrineTrace(ProbeListenerObject):
    type = "PeregrineTrace"
    cxx_class = "gem5::PeregrineTrace"
    cxx_header = "cpu/o3/probe/peregrine_trace.hh"

    output_file = Param.String(
        "peregrine.trace.pb.zst", "Peregrine zstd protobuf trace output file"
    )
    chunk_records = Param.Unsigned(65536, "Maximum records per trace chunk")
    zstd_level = Param.Int(1, "Zstd compression level")
    mem_dep_lookback = Param.UInt64(
        16777216,
        "Memory dependency tracking horizon in committed instructions; "
        "older writers are evicted to bound the address map",
    )
