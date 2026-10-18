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
* `idiv.isa`: `compactIntDiv` selects a fixed-footprint behavior proxy for
  32/64-bit register DIV/IDIV. It computes quotient, remainder and #DE
  semantics atomically, then exposes separate quotient/remainder result stages;
  O3 owns the shared width-sensitive admission timing. False selects the
  original macro-op. 8/16-bit and memory forms remain unchanged. The proxy is
  constrained by published SPR behavior and is not a claim about Intel's
  internal microcode algorithm.
* `zero_idiom.isa`: `gprZeroIdiomMode=1` routes 32/64-bit same-register
  XOR/SUB to a zero-result operation without the old GPR dependency while
  retaining IntAlu use. Mode 2 uses the same semantics with an abstract
  six-wide completion resource rather than an ordinary execution port. Both
  preserve result, flags and retirement; neither claims to implement Intel's
  undisclosed rename-map representation. Other widths and forms remain on the
  original path.

Parameters do not change during simulation. ISA definitions and their includes
are tracked by the normal SCons ISA scanner. Functional and performance
experiments are owned by cpu_microarchitecture, not by this directory.
