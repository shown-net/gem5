"""Run CPUID and ordinary-loop controls against an X86 gem5 binary.

Example: python tests/gem5/x86_o3_wakeup/check.py --gem5 build/X86/gem5.fast
         --output build/o3-wakeup-check
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    source = Path(__file__).resolve().parent
    output = args.output.resolve()
    binary = args.gem5.resolve()
    if output.exists() or not output.is_relative_to(repo / "build"):
        raise ValueError("use a fresh output directory within this repository's build/")
    output.mkdir(parents=True)
    env = dict(os.environ, TMPDIR=str(output))
    result = {}
    for name, serial in (("cpuid", 1), ("ordinary", 0)):
        workload = output / name
        subprocess.run([os.environ.get("CC", "cc"), "-nostdlib", "-static", "-no-pie",
                        f"-DSERIALIZE={serial}", str(source / "loop.S"), "-o", str(workload)],
                       check=True, env=env)
        run = output / (name + "-run")
        run.mkdir()
        with (run / "tool.log").open("w") as log:
            subprocess.run([str(binary), f"--outdir={run}", str(source / "se_probe.py"),
                            str(workload)], stdout=log, stderr=subprocess.STDOUT,
                           check=True, env=env)
        wanted = {"simTicks", "system.cpu.numCycles", "system.cpu.CPU_CLK_UNHALTED.THREAD",
                  "system.cpu.commitStats0.numInsts"}
        values = {}
        for line in (run / "stats.txt").read_text().splitlines():
            fields = line.split()
            if fields and fields[0] in wanted:
                values[fields[0]] = int(fields[1])
        if values.keys() != wanted:
            raise AssertionError(f"{name}: missing required statistics: {wanted - values.keys()}")
        elapsed, remainder = divmod(values["simTicks"], 385)
        if (remainder or values["system.cpu.commitStats0.numInsts"] != 8003 or
                values["system.cpu.CPU_CLK_UNHALTED.THREAD"] != elapsed or
                not 0 <= values["system.cpu.numCycles"] - elapsed <= 1):
            raise AssertionError(f"{name}: repeated tick or incorrect completion: {values}")
        result[name] = values
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
