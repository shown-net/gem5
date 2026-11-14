/* KVM-to-O3 architectural-register readiness and managed-divider probe. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern char div_zero_fault[], div_zero_after[];
extern char div_overflow_fault[], div_overflow_after[];
extern void div_zero(void), div_overflow(void);
static volatile sig_atomic_t faults;

/* Identifiable fault and resume PCs allow testing precise #DE register state. */
__asm__(
    ".text\n"
    ".globl div_zero,div_zero_fault,div_zero_after\n"
    "div_zero:\n"
    "mov $123,%eax\n xor %edx,%edx\n xor %ecx,%ecx\n"
    "div_zero_fault:\n divq %rcx\n div_zero_after:\n ret\n"
    ".globl div_overflow,div_overflow_fault,div_overflow_after\n"
    "div_overflow:\n"
    "xor %eax,%eax\n mov $1,%edx\n mov $1,%ecx\n"
    "div_overflow_fault:\n divq %rcx\n div_overflow_after:\n ret\n");

static void
divide_fault(int signal_number, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    greg_t *regs = uc->uc_mcontext.gregs;
    (void)info;
    if (signal_number != SIGFPE) _exit(20);
    if ((uintptr_t)regs[REG_RIP] == (uintptr_t)div_zero_fault &&
        regs[REG_RAX] == 123 && regs[REG_RDX] == 0) {
        regs[REG_RIP] = (greg_t)(uintptr_t)div_zero_after;
    } else if ((uintptr_t)regs[REG_RIP] == (uintptr_t)div_overflow_fault &&
               regs[REG_RAX] == 0 && regs[REG_RDX] == 1) {
        regs[REG_RIP] = (greg_t)(uintptr_t)div_overflow_after;
    } else {
        _exit(21);
    }
    ++faults;
}

static void
fail(const char *stage)
{
    fprintf(stderr, "divider probe failed: %s\n", stage);
    exit(1);
}

int
main(int argc, char **argv)
{
    struct sigaction action = {0};
    int is32, is_signed;
    uint64_t low, high, divisor = 7;
    register volatile void *trigger __asm__("r8");

    if (argc != 2 || (strcmp(argv[1], "u32") && strcmp(argv[1], "u64") &&
                      strcmp(argv[1], "s32") && strcmp(argv[1], "s64"))) {
        fprintf(stderr, "usage: %s {u32|u64|s32|s64}\n", argv[0]);
        return 2;
    }
    is32 = strstr(argv[1], "32") != NULL;
    is_signed = argv[1][0] == 's';
    action.sa_sigaction = divide_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGFPE, &action, NULL)) { perror("sigaction"); return 2; }

    int fd = open("/dev/gem5_bridge", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open gem5 bridge"); return 2; }
    void *base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { perror("mmap gem5 bridge"); return 2; }
    trigger = (volatile char *)base + (0x21 << 8); /* m5_exit(0) */

    low = is_signed ? (uint64_t)-100 : 100;
    high = is_signed ? UINT64_MAX : 0;
    /* RAX, RDX and RBX are prepared before the KVM exit. The immediately
     * following divide must consume the values inherited by O3. */
    if (is32 && is_signed) {
        __asm__ volatile("movq $0,(%%r8)\n\tidivl %%ebx"
                         : "+a"(low), "+d"(high)
                         : "b"(divisor), "r"(trigger), "D"(0UL) : "cc", "memory");
    } else if (is32) {
        __asm__ volatile("movq $0,(%%r8)\n\tdivl %%ebx"
                         : "+a"(low), "+d"(high)
                         : "b"(divisor), "r"(trigger), "D"(0UL) : "cc", "memory");
    } else if (is_signed) {
        __asm__ volatile("movq $0,(%%r8)\n\tidivq %%rbx"
                         : "+a"(low), "+d"(high)
                         : "b"(divisor), "r"(trigger), "D"(0UL) : "cc", "memory");
    } else {
        __asm__ volatile("movq $0,(%%r8)\n\tdivq %%rbx"
                         : "+a"(low), "+d"(high)
                         : "b"(divisor), "r"(trigger), "D"(0UL) : "cc", "memory");
    }
    if (is_signed) {
        if (low != (is32 ? UINT64_C(0xfffffff2) : (uint64_t)-14) ||
            high != (is32 ? UINT64_C(0xfffffffe) : (uint64_t)-2))
            fail("inherited signed inputs");
    } else if (low != 14 || high != 2) {
        fail("inherited unsigned inputs");
    }

    /* Load/ALU-to-divide and divide-to-divide dependencies, both results. */
    volatile uint64_t six = 6;
    uint64_t quotient = 100, remainder = 0;
    __asm__ volatile("movq %[six],%%rcx\n\taddq $1,%%rcx\n\t"
                     "divq %%rcx\n\txor %%edx,%%edx\n\t"
                     "mov $3,%%ecx\n\tdivq %%rcx"
                     : "+a"(quotient), "+d"(remainder)
                     : [six] "m"(six) : "rcx", "cc", "memory");
    if (quotient != 4 || remainder != 2) fail("dependent 64-bit divides");
    quotient = (uint32_t)-100; remainder = UINT32_MAX;
    __asm__ volatile("movl $6,%%ecx\n\taddl $1,%%ecx\n\tidivl %%ecx"
                     : "+a"(quotient), "+d"(remainder) : : "rcx", "cc");
    if (quotient != UINT64_C(0xfffffff2) || remainder != UINT64_C(0xfffffffe))
        fail("dependent signed 32-bit divide");

    div_zero();
    div_overflow();
    if (faults != 2) fail("precise divide exceptions");
    munmap(base, 0x10000);
    printf("{\"probe\":\"x86-divider-handoff\",\"mode\":\"%s\","
           "\"inherited\":true,\"dependencies\":true,\"faults\":%d}\n",
           argv[1], (int)faults);
    return 0;
}
