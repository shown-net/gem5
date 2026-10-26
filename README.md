# gem5 v25: FastSim and calibration

The active branches retain the official v25.1.0.1 ancestry. The
FastSim mainline is based on c8222cc67a399bfc01e8658dd14b30d5bfd634f9. FastSim
production changes are synchronized from
`/home/data00/yinhaolang/FastSim/gem5-fs`; that source directory remains under
its author's management.

| Branch | Content | Worktree |
| --- | --- | --- |
| `fastsim-main` | Current FastSim producer and its native observations | Updated through a temporary checkout in the local worktree |
| `calibration-deploy` | FastSim plus calibrated ISA/FU, frontend and memory mechanisms | `/home/data00/zhonghaocheng/simulation-framework/gem5` |
| `calibration-dev` | Deployment plus local KVM/TASK and Marker measurement support | `/home/data00/xuhaoen/repos/simulators/gem5-cpu` |
| `archive` | Previous histories and original reference index | `/home/data00/xuhaoen/repos/simulators/gem5-archive` |

The public Git metadata is owned by the local worktree. Linked worktrees share
branches and objects, while HEAD, index and source files remain per-worktree.
`archive/ARCHIVE.md` means the file on the archive branch, where the default
source view is the previous calibration-v26 tree. Old local development
histories remain in archive; official v25 ancestors are shared with the
active source branches.

FastSim updates flow into deployment, then development. Promote selected
calibration changes from development; a whole development merge would carry
local measurement support into deployment. The upstream remote tracks the
official reference independently of the active branch history.

FastSim's original native counters, FST and request/retirement contracts are
retained. Unconsumed calibration diagnostics and their bookkeeping have been
removed; admission, dependency, ordering, cancellation and drain state remain
part of the actual models.

## Builds

FastSim and deployment use `build/X86_MESI_Three_Level/gem5.opt` with the v25
build configuration. Load the workspace environment, use GCC 11 and the
existing Python 3.11/protobuf v25 configuration, and set `TAOGEN_SHARED` to
the current tree's `taogen/shared`. Build resources come from the CPU project's
host configuration.

Development uses the CPU repository's `compile_gem5.sh`, producing
`build/X86/gem5.fast` and the common libm5. Its `--source-root PATH` input
supports checking a staged development tree before deployment. The existing
CPU Marker API selects the independent calibration module when `marker_pc`
is nonzero; the FastSim producer remains the default path when it is zero.

The reconstruction is checked by compilation, native parameter smoke checks,
reference reachability and source/interface inspection. Those checks do not
establish timing accuracy, whole-workload behavior or SPR equivalence.

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
