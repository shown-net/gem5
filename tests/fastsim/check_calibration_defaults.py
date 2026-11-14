"""Verify the CPU-owned parameter table and dynamic overrides in gem5."""
import sys
from pathlib import Path
from types import SimpleNamespace

cpu_root = Path(sys.argv[1]).resolve()
sys.path[:0] = [str(cpu_root), str(cpu_root / "configs/gem5"), str(cpu_root / "configs/gem5/sim")]
from models.ruby_board import build_model, MICROARCHITECTURE_CONFIG
from models.parameters import read_model_config, deep_merge
from models.ruby_geometry import memory_size_bytes


def check(obj, expected, path):
    for key, value in expected.items():
        actual = getattr(obj, key)
        name = f"{path}.{key}"
        if isinstance(value, dict):
            check(actual, value, name)
        elif isinstance(value, list):
            assert [int(x) for x in actual] == value, name
        elif isinstance(value, bool):
            assert str(actual).lower() == str(value).lower(), name
        elif isinstance(value, int):
            assert int(actual) == value, name
        else:
            assert str(actual) == str(type(actual)(value)), (name, str(actual), value)


def verify(overrides):
    values = deep_merge(read_model_config(MICROARCHITECTURE_CONFIG), overrides)
    board = build_model(SimpleNamespace(num_cores=2, workflow="kernel"), **overrides)
    cores = board.get_processor()._switchable_cores["switch"]
    cpu = cores[1].get_simobject()
    for name, obj in (("o3", cpu), ("frontend", cpu.frontend),
                      ("mmu", cpu.mmu), ("stackEngine", cpu.stackEngine),
                      ("branchPred", cpu.branchPred)):
        check(obj, values[name], name)
    check(cores[0].get_simobject().mmu, values["mmu"], "housekeeping.mmu")
    cache = board.get_cache_hierarchy()
    for name, expected in values["cache"]["geometry"].items():
        actual = getattr(cache, "_" + name)
        assert (memory_size_bytes(actual) == memory_size_bytes(expected)
                if isinstance(expected, str) else int(actual) == expected), name
    for name in ("l1_refill_bytes_per_cycle", "l1_fill_capacity",
                 "l1_write_fill_capacity", "l1_fill_response_width",
                 "l1d_data_access_cycles", "l2_fill_capacity",
                 "sequencer_max_outstanding_requests", "l1_tbe_capacity",
                 "l2_tbe_capacity"):
        assert getattr(cache, "_" + name) == values["cache"][name], name
    assert cache._spr_network_parameters == values["network"]
    controllers = board.get_memory().get_memory_controllers()
    assert len(controllers) == values["system"]["memory_channels"]
    for controller in controllers:
        check(controller, values["memory"]["mem_ctrl"], "memory.mem_ctrl")


verify({})
verify({"o3": {"numROBEntries": 384, "sharedIQ": {"numEntries": 192}},
        "mmu": {"dtb": {"next_level_hit_latency": 9}},
        "cache": {"l1d_data_access_cycles": 4, "l1_write_fill_capacity": 8,
                  "l2_fill_capacity": 32, "sequencer_max_outstanding_requests": 96,
                  "l1_tbe_capacity": 192, "l2_tbe_capacity": 192},
        "network": {"tile_rows": 5, "tile_columns": 3, "inter_tile_latency": 8}})
print("CPU parameter table, housekeeping MMU and dynamic overrides verified")
