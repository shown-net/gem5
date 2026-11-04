# Native SPR retired-stream collection

`fs_capture.py` brings the existing FastSim full-system collection protocol to
native SPR gem5. The guest workload drives the checkpoint and WORKBEGIN markers.
KVM boots and initializes the workload, the checkpoint switches to native O3,
and TaoTrace records the functional warmup before the measured window. The first
participant reaching the measured user-UOP target closes every attached stream
at one common end. A premature guest exit or tick limit fails the capture.

The legacy begin helper prints `operation=workbegin-serial` and issues an
`m5_work_begin` work-item notification. UART delivery can lag behind the m5op.
Work-item IDs may be zero, so exit codes cannot identify the genuine boundary.
The handler reads each producer's read-only `functionalMeasurementStarted()`
state. If no participant has begun, the event is logged as
`raw-work-item-notification` without opening measurement or resetting statistics.
The serial terminal invokes `traceMeasurementBegin` globally; all participants
must then report that they have started before the handler opens the host
measurement state and resets statistics once. Mixed participant states fail.
Later work-item notifications, including ID zero, are logged as
`work-item-notification-after-measurement` and do not reset statistics again.

Example with local legacy assets and an independently audited scheduler profile:

```sh
build/X86/gem5.opt --outdir=/path/to/new-run \
  configs/fastsim/fs_capture.py \
  --fastsim-root /path/to/FastSim \
  --workload-binary /path/to/static-marked-workload \
  --workload-args '1000 2000' --participant-cores 4 \
  --functional-user-target 100000 \
  --kernel /path/to/audited-vmlinux --disk /path/to/guest.img \
  --disk-root-partition 1 \
  --functional-sync --thread-state --scheduler-profile /path/to/profile.json
```

Omit `--kernel`/`--disk` to use gem5 resources
`x86-linux-kernel-5.4.49`/`x86-ubuntu-18.04-img`; other resource IDs can be selected
with `--kernel-resource`/`--disk-resource`. Local paths are supported without
resource downloads. The static workload and its markers must already be present;
the entry does not synthesize marker instructions or scheduler evidence.

The detailed CPUs retain `X86O3CPU` defaults, including native MITE/DSB/MS/IDQ,
512 ROB entries, the 128/72/48 partitioned IQs, shared execution ports and native
SPR committed-store behavior. `SPRCacheHierarchy` supplies the native private
caches and eight LLC banks of 12 MiB each, 96 MiB total, 12 ways. Memory is
2 GiB, 16-channel DDR5_4400_4x8 with 64-byte interleaving; the system runs at
2.6 GHz. These are the current `spr-framework` native settings. Generic O3,
DDR4, store-mode and generic cache tuning flags are intentionally unavailable.
After the KVM-to-O3 switch the entry enters single-event-queue mode and sets
`Root.sim_quantum=0`, matching the framework's native detailed handoff.

The legacy topology uses O3 participants **0 through N-1**, plus optional
untraced O3 spare cores. It has no reserved Timing CPU0. This differs from the
fixed-work framework topology, whose worker cores start after a housekeeping
CPU0. `sync-run-request.json` declares the actual participant IDs. The existing
TaoTrace name parser recognizes the native `board.processor.switch0.core`
names; `fixed_work_source_core` is not used for this collection mode.

Every participant emits direct kernel-inclusive FST with real syscall UOPs,
retired frontend metadata (`.ffe.gz`) and functional retirement evidence
(`.fsem`). The new `functional_retirement` producer option must be built into
gem5. Functional retirement is independent of task-state attribution.
`--functional-sync` validates the actual static synchronization-marker symbol.
`--thread-state` requires a scheduler profile whose kernel identity and audited
machine-code witnesses are rechecked against the selected kernel; the known
legacy 5.4.49 build is supported. A host 5.15 kernel has no audited profile here
and cannot establish task ownership. Omitting thread-state options leaves task
attribution unavailable while retaining functional retirement evidence.

The entry reuses maintained workload staging, static marker validation and
scheduler audit utilities from `--fastsim-root/scripts/run_gem5_sync_probe.py`.
It never calls that script's `main`, `configure_o3`, memory builder or profile
writer. It also reuses the bounded-warmup/rate-launcher protocol. Rate captures
require their existing real launcher and a positive `--warmup-insts` budget;
plain captures normally use the workload's WORKBEGIN. This entry does not
restore old hardware-specific checkpoints or save new ones.

`uarch_profile.json` is written from the incorporated native Ruby cache,
controller, CPU and TLB objects before probe registration. Its cache/TLB shadow
fields serve the existing diagnostic observer. They are not native cache truth;
its limitations and capacity definitions are explicit. Native PMU/cache truth
still requires conserved Ruby/SLICC summaries and the maintained validator.
Neither successful collection nor functional companion validity establishes
hardware identity, CPI/PMU accuracy, task ownership or throughput acceptance.

Run the dependency-free entry policy checks from FastSim with:

```sh
python3 tests/test_spr_collection_entry.py
```
