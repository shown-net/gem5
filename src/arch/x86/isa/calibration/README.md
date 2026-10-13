# Runtime-selectable x86 decomposition mechanisms

These definitions are included by the normal x86 ISA parser. They need no
candidate-specific Kconfig options. X86Decoder owns immutable per-decoder
parameters; macroop.isa selects a candidate before returning the original
macroop. The original path remains selected when all parameters are false.

* `imul.isa`: `singleResultImul` selects one result-producing IntMultOp for
  register-register IMUL, preserving 16/32/64-bit writes and CF/OF. Memory,
  immediate and implicit-operand forms are outside this candidate.
* `setz.isa`: `singleUopSetz` selects one rather than two result uops;
  `zfOnlySetz` selects Zaps rather than the generic flags dependency set.
  Byte merge retains the old destination dependency. Both false select the
  original register SETZ. The earlier single-result candidate corresponds
  to both parameters true.

Parameters do not change during simulation. ISA definitions and their includes
are tracked by the normal SCons ISA scanner. Functional and performance
experiments are owned by cpu_microarchitecture, not by this directory.
