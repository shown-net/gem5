.text
.global _start
.type _start,@function
_start:
    mov $2000, %r12d
.Lloop:
    xor %eax, %eax
#if SERIALIZE
    cpuid
#else
    nop
#endif
    dec %r12d
    jne .Lloop
    mov $60, %eax
    xor %edi, %edi
    syscall
.size _start,.-_start
.section .note.GNU-stack,"",@progbits
