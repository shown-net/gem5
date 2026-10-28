# gem5 v25: FastSim and component calibration

The common mechanism starting point is
`74c1e6350bb91f581eb7fedfe9a5a154fc5f1ab0`: the FastSim producer on official
v25.1.0.1 `c8222cc67a399bfc01e8658dd14b30d5bfd634f9`. It already contains
buffer TSO / Store Prepare code; new CPU experiments explicitly select serial.
The FastSim source remains managed by its author at
`/home/data00/yinhaolang/FastSim/gem5-fs`; this restart does not modify it.

| Branch | Current responsibility | Worktree |
| --- | --- | --- |
| `fastsim-main` | Existing producer, FST contract and native observations | Temporary checkout for updates |
| `calibration-deploy` | Common base plus independent FSGSBASE functional support; future accepted calibration mechanisms | `/home/data00/xuhaoen/repos/simulators/gem5-deploy` |
| `calibration-dev` | Deployment plus KVM drain, HANDOFF, single-event-queue transition, Marker snapshots and optional PC/branch traces | `/home/data00/xuhaoen/repos/simulators/gem5-cpu` |
| `archive` | Superseded histories and their reference index | Remote only, excluded from daily fetches |

TASK isolation, native-stat return-value replacement, custom allocation / issue /
retirement counters and the previous ISA/FU, frontend-group and memory-service
calibration implementations remain in
[archive/ARCHIVE.md](https://github.com/shown-net/gem5/blob/archive/ARCHIVE.md).
The index records old development `c43cd526` and deployment `e3b82f67` tips.
New component timing remains unvalidated.

The local worktrees share Git metadata. The independent deployment consumer at
`/home/data00/zhonghaocheng/simulation-framework/gem5` tracks only
`calibration-deploy`; after the one-time history migration, update it with
`git pull --ff-only`.

Daily fetches retain the three active origin branches and two fixed official
references: `upstream/stable-v25` at c8222cc67a and `upstream/stable-v26` at
fed0e07e93. Archive and the full tag set are excluded. Use a separate history
checkout for old graphs; the fixed official references remain source controls.

FastSim updates enter deployment, then development. Promote calibration changes
individually from development; merging all development would also publish local
measurement support.

## Builds and observation

FastSim and deployment retain the v25 C++17 build recipe:
`build/X86_MESI_Three_Level/gem5.opt`, GCC 11, Python 3.11 and system protobuf.
Set `TAOGEN_SHARED` to the checked-out tree's `taogen/shared`. Build resources
come from the CPU project's `configs/host.yaml`.

Development uses the CPU project's `compile_gem5.sh`, producing
`build/X86/gem5.fast` and libm5; `--opt` preserves assertions and tracing.
Only the public development and deployment trees own simulator builds;
experiments do not keep source copies or frozen gem5 binaries. Development
uses C++20 for the canonical protobuf/Abseil installation.

The existing `TaoTrace` Marker API delegates to `CalibrationMarker` when
`marker_pc` is nonzero and retains the producer path when it is zero.
START and END save cumulative native statistics at retirement. CPU analysis
uses END minus START over the workload O3 core, with no TASK settlement or CPL
filtering. Hardware PMU keeps its measured-thread TASK scope; reports show the
difference. Optional observations do not define calibrated execution mechanisms.

The CPU public model uses native `X86O3CPU` and
`MESIThreeLevelCacheHierarchy`. Its documented SPR resource initial values and
fixed experiment geometry are distinct from validated component timing.
Development `X86O3CPU` now defaults to twelve shared execution ports, five
individual integer ALUs, and the implemented integer result paths. The common
CPU-project model applies the same SPR resource cfg to development and
deployment; the selected source owns the execution structure without task
mechanism switches.
Compilation and minimal functional runs establish interface and execution
availability, not performance accuracy or SPR equivalence.

---

# The gem5 Simulator

This is the repository for the gem5 simulator. It contains the full source code
for the simulator and all tests and regressions.

The gem5 simulator is a modular platform for computer-system architecture
research, encompassing system-level architecture as well as processor
microarchitecture. It is primarily used to evaluate new hardware designs,
system software changes, and compile-time and run-time system optimizations.

The main website can be found at <http://www.gem5.org>.

## Testing status

**Note**: These regard tests run on the develop branch of gem5:
<https://github.com/gem5/gem5/tree/develop>.

[![Daily Tests](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml/badge.svg?branch=develop)](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml)
[![Weekly Tests](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml/badge.svg?branch=develop)](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml)
[![Compiler Tests](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml/badge.svg?branch=develop)](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml)

## Getting started

A good starting point is <http://www.gem5.org/about>, and for
more information about building the simulator and getting started
please see <http://www.gem5.org/documentation> and
<http://www.gem5.org/documentation/learning_gem5/introduction>.

## Building gem5

To build gem5, you will need the following software: g++ or clang,
Python (gem5 links in the Python interpreter), SCons, zlib, m4, and lastly
protobuf if you want trace capture and playback support. Please see
<http://www.gem5.org/documentation/general_docs/building> for more details
concerning the minimum versions of these tools.

Once you have all dependencies resolved, execute
`scons build/ALL/gem5.opt` to build an optimized version of the gem5 binary
(`gem5.opt`) containing all gem5 ISAs. If you only wish to compile gem5 to
include a single ISA, you can replace `ALL` with the name of the ISA. Valid
options include `ARM`, `NULL`, `MIPS`, `POWER`, `RISCV`, `SPARC`, and `X86`
The complete list of options can be found in the build_opts directory.

See https://www.gem5.org/documentation/general_docs/building for more
information on building gem5.

## The Source Tree

The main source tree includes these subdirectories:

* build_opts: pre-made default configurations for gem5
* build_tools: tools used internally by gem5's build process.
* configs: example simulation configuration scripts
* ext: less-common external packages needed to build gem5
* include: include files for use in other programs
* site_scons: modular components of the build system
* src: source code of the gem5 simulator. The C++ source, Python wrappers, and Python standard library are found in this directory.
* system: source for some optional system software for simulated systems
* tests: regression tests
* util: useful utility programs and files

## gem5 Resources

To run full-system simulations, you may need compiled system firmware, kernel
binaries and one or more disk images, depending on gem5's configuration and
what type of workload you're trying to run. Many of these resources can be
obtained from <https://resources.gem5.org>.

More information on gem5 Resources can be found at
<https://www.gem5.org/documentation/general_docs/gem5_resources/>.

## Getting Help, Reporting bugs, and Requesting Features

We provide a variety of channels for users and developers to get help, report
bugs, requests features, or engage in community discussions. Below
are a few of the most common we recommend using.

* **GitHub Discussions**: A GitHub Discussions page. This can be used to start
discussions or ask questions. Available at
<https://github.com/orgs/gem5/discussions>.
* **GitHub Issues**: A GitHub Issues page for reporting bugs or requesting
features. Available at <https://github.com/gem5/gem5/issues>.
* **Jira Issue Tracker**: A Jira Issue Tracker for reporting bugs or requesting
features. Available at <https://gem5.atlassian.net/>.
* **Slack**: A Slack server with a variety of channels for the gem5 community
to engage in a variety of discussions. Please visit
<https://www.gem5.org/join-slack> to join.
* **gem5-users@gem5.org**: A mailing list for users of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.
* **gem5-dev@gem5.org**: A mailing list for developers of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.

## Contributing to gem5

We hope you enjoy using gem5. When appropriate we advise sharing your
contributions to the project. <https://www.gem5.org/contributing> can help you
get started. Additional information can be found in the CONTRIBUTING.md file.
