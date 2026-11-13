"""Run one divider probe using the framework's existing full-system model."""
import argparse
from pathlib import Path
import shlex
import sys
from types import SimpleNamespace

parser = argparse.ArgumentParser()
parser.add_argument("--framework", type=Path, required=True)
parser.add_argument("--model", type=Path, required=True)
parser.add_argument("--kernel", type=Path, required=True)
parser.add_argument("--initrd", type=Path, required=True)
parser.add_argument("--root-image", type=Path, required=True)
parser.add_argument("--root-uuid", required=True)
parser.add_argument("--checkpoint", type=Path, required=True)
parser.add_argument("--payload", type=Path, required=True)
parser.add_argument("--mode", choices=("u32", "u64", "s32", "s64"), required=True)
args = parser.parse_args()

framework = args.framework.resolve()
sys.path.insert(0, str(framework / "configs/gem5/sim"))
sys.path.insert(0, str(framework / "configs/gem5"))

import m5
from m5.objects import NULL
from gem5.resources.resource import DiskImageResource, KernelResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.exit_handler import register_exit_handler
from gem5.simulate.simulator import Simulator
from models.ruby_board import build_model

run = SimpleNamespace(workflow="probe", num_cores=5,
                      microarchitecture=str(args.model.resolve()),
                      gem5_data_prefetch=False, gem5_early_store_prepare=True)
board = build_model(run)
board._run_inputs = run
board.set_payload_path(args.payload.resolve())
core = board.get_processor()._switchable_cores["switch"][1].get_simobject()
if core.intDivider == NULL or core.resultForwarding == NULL:
    raise RuntimeError("probe requires the deployment managed divider and forwarding")

scratch = "/var/tmp/divider-handoff-" + args.mode
cmd = shlex.join(["sudo", "-n", "/usr/bin/nice", "-n", "-20", "/usr/bin/setpriv",
                  "--reuid=gem5", "--regid=gem5", "--init-groups", "taskset",
                  "--cpu-list", "1", scratch + "/divider-handoff", args.mode])
guest = "\n".join([
    "#!/bin/sh", "set -eu",
    "sudo -n /usr/bin/mount -t ext4 -o ro,noload,noatime LABEL=CPU_MICROARCH /mnt/cpu_microarch",
    f"test ! -e {scratch}", f"mkdir -p {scratch}",
    f"cp /mnt/cpu_microarch/divider-handoff {scratch}/divider-handoff",
    f"if {cmd} >{scratch}/stdout 2>{scratch}/stderr; then rc=0; else rc=$?; fi",
    f"cat {scratch}/stdout {scratch}/stderr",
    f"gem5-bridge writefile {scratch}/stdout guest-stdout.txt",
    f"gem5-bridge writefile {scratch}/stderr guest-stderr.txt",
    f"printf '%s\\n' \"$rc\" > {scratch}/exit",
    f"gem5-bridge writefile {scratch}/exit guest-exit.txt",
    'printf "GEM5_DIVIDER_PROBE_GUEST_EXIT rc=%s\\n" "$rc"',
    'test "$rc" = 0',
])
kernel_args = [v for v in board.get_default_kernel_args()
               if not v.startswith(("lpj=", "root="))]
kernel_args += [f"root=UUID={args.root_uuid}", "systemd.cpu_affinity=0",
                "transparent_hugepage=madvise", "nopti", "libata.force=pio4",
                "libata.dma=0", "irqaffinity=0", "nohz_full=1-4"]
board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=args.kernel.resolve()),
    disk_image=DiskImageResource(local_path=args.root_image.resolve(), root_partition="2"),
    disk_device="/dev/sda", kernel_args=kernel_args, readfile_contents=guest,
    checkpoint=args.checkpoint.resolve())
board.workload.initrd = str(args.initrd.resolve())

state = {"switched": False, "finished": False}

def switch_once():
    if state["switched"]:
        raise RuntimeError("unexpected second m5_exit")
    simulator.switch_processor()
    state["switched"] = True
    print("GEM5_DIVIDER_PROBE_SWITCH KVM_TO_O3", flush=True)
    yield False
    raise RuntimeError("unexpected second m5_exit")

def guest_finished(_simulator, _payload):
    state["finished"] = True
    return True

register_exit_handler(3, guest_finished, "divider probe guest script completed")
simulator = Simulator(board=board, on_exit_event={ExitEvent.EXIT: switch_once()})
simulator.run()
if not state["switched"] or not state["finished"]:
    raise RuntimeError("divider probe did not complete its switch and guest script")
print("GEM5_DIVIDER_PROBE_DONE", flush=True)
