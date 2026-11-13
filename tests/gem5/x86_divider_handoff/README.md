# x86 KVM-to-O3 divider readiness probe

Build from this directory with the workspace environment loaded:

```
make OUT=../../../build/x86-divider-handoff
```

Run against the already built deployment binary and the framework's guest and
system boot checkpoint (the output directory must be new):

```
"${PYTHON}" tests/gem5/x86_divider_handoff/run.py \
  --output build/x86-divider-handoff-run
```

The runner creates a read-only payload, restores the boot checkpoint and runs
four separate guest executions: `u32`, `u64`, `s32`, and `s64`. Pass `--mode`
to run just one. `fs_probe.py` reuses the framework's full-system model with
the deployment's managed `IntegerDivider` and `ResultForwarding` enabled. It
switches KVM to O3 on `m5_exit(0)`, immediately before the first divide; the
runner requires normal guest exit and checks the success JSON for each mode.

The first divide consumes RAX, RDX, and RBX prepared before the switch.
Later checks cover 64-bit load/ALU and divide-to-divide dependencies, signed
32-bit dependency, and precise divide-by-zero/overflow fault register state.
This is a functional probe; it does not alone establish divider latency or
quotient/remainder wakeup timing. It is not a benchmark or a PMU measurement.
