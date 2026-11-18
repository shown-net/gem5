/* Fill six ways, revisit pair B, checkpoint, then distinguish lost state.
 * All data pages use the same 4 KiB DTLB set; no stack loads intervene.
 */
.text
.globl _start
_start:
  lea pages(%rip),%r12
  mov 0(%r12),%rax
  mov 65536(%r12),%rax
  mov 131072(%r12),%rax
  mov 196608(%r12),%rax
  mov 262144(%r12),%rax
  mov 327680(%r12),%rax
  mov 262144(%r12),%rax
  /* Drain may finish the next instruction after the instruction-count exit.
   * Keep that instruction independent of the translation replacement state.
   */
  .rept 8
  nop
  .endr
  mov 393216(%r12),%rax
  mov 131072(%r12),%rax
  mov 0(%r12),%rax
  xor %edi,%edi
  mov $60,%eax
  syscall
.data
.balign 65536
pages:
  .skip 458752
.section .note.GNU-stack,"",@progbits
