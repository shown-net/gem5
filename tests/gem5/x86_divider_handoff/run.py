"""Build a small payload and verify four full-system divider probe modes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

repo = Path(__file__).resolve().parents[3]
repos = repo.parent.parent
framework = repos / "simulation-tools/simulation-framework"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--gem5", type=Path, default=repo / "build/X86/gem5.fast")
parser.add_argument("--binary", type=Path, default=repo / "build/x86-divider-handoff/divider-handoff")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--mode", choices=("u32", "u64", "s32", "s64"),
                    help="run one mode; default runs all four")
parser.add_argument("--timeout-s", type=int, default=1800)
parser.add_argument("--checkpoint", type=Path,
                    default=framework / "output/calibration/spr-1ms-zstd-gem5-20261112/boot-checkpoint")
args = parser.parse_args()
out = args.output.resolve()
if out.exists() or not out.is_relative_to(repo / "build") or args.timeout_s <= 0:
    raise ValueError("use a fresh output under gem5-deploy/build and a positive timeout")
for item in (args.gem5, args.binary, args.checkpoint / "m5.cpt"):
    if not item.is_file():
        raise FileNotFoundError(item)
out.mkdir(parents=True)
staging = out / "payload-staging"
staging.mkdir()
shutil.copy2(args.binary, staging / "divider-handoff")
(staging / "divider-handoff").chmod(0o755)
payload = out / "payload.ext4"
env = dict(os.environ, TMPDIR=str(out))
subprocess.run(["mke2fs", "-q", "-t", "ext4", "-F", "-b", "4096", "-L",
                "CPU_MICROARCH", "-O", "^has_journal,^metadata_csum", "-E",
                "lazy_itable_init=0", "-N", "8192", "-d", str(staging),
                str(payload), "65536"], check=True, env=env)
model = repos / "calibration/cpu_microarchitecture/configs/microarchitecture.cfg"
kernel = Path("/home/data00/xuhaoen/data/images/gem5/host-vmlinux")
root_image = Path("/home/data00/xuhaoen/data/images/gem5/host-calibration-root.img")
initrd = framework / "build/guest-pio-20261106/initrd.img"
result = {}
for mode in ((args.mode,) if args.mode else ("u32", "u64", "s32", "s64")):
    case = out / mode
    case.mkdir()
    command = [str(args.gem5.resolve()), f"--outdir={case}",
               str(Path(__file__).with_name("fs_probe.py")), "--framework", str(framework),
               "--model", str(model), "--kernel", str(kernel), "--initrd", str(initrd),
               "--root-image", str(root_image), "--root-uuid", "f397f14a-f704-49f6-adf5-3f2de8263641",
               "--checkpoint", str(args.checkpoint.resolve()), "--payload", str(payload),
               "--mode", mode]
    with (case / "tool.log").open("w") as log:
        proc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                              env=env, timeout=args.timeout_s)
    log = (case / "tool.log").read_text(errors="replace")
    exit_path, stdout_path = case / "guest-exit.txt", case / "guest-stdout.txt"
    if (proc.returncode != 0 or "GEM5_DIVIDER_PROBE_SWITCH KVM_TO_O3" not in log or
            "GEM5_DIVIDER_PROBE_DONE" not in log or not exit_path.is_file() or
            exit_path.read_text().strip() != "0" or not stdout_path.is_file()):
        raise RuntimeError(f"{mode} failed; inspect {case}")
    rows = [json.loads(line) for line in stdout_path.read_text().splitlines()
            if line.startswith('{"probe":')]
    if rows != [{"probe": "x86-divider-handoff", "mode": mode,
                 "inherited": True, "dependencies": True, "faults": 2}]:
        raise RuntimeError(f"{mode} produced invalid guest results: {rows}")
    result[mode] = {"status": "pass", "guest_result": rows[0]}
(out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
