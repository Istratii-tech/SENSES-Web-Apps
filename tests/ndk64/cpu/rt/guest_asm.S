// Assembly parts of the AArch64 guest test runtime: setjmp/longjmp,
// running a function on another stack, and run_case, which the
// differential tests (tests/ndk64/cpu/diff) use to run one generated case
// with every register loaded from and stored to a buffer. Assembled both
// for the guest (ELF) and natively on macOS (Mach-O) for the reference run.
#ifdef __APPLE__
#define SYM(x) _##x
#define FUNC(x) .globl SYM(x) %% .p2align 2 %% SYM(x):
#else
#define SYM(x) x
#define FUNC(x) .globl x; .type x, %function; .p2align 2; x:
#endif

        .text

#ifndef DIFF_NATIVE
// int guest_setjmp(guest_jmp_buf env): x19-x30, sp, d8-d15.
FUNC(guest_setjmp)
        stp     x19, x20, [x0, #0]
        stp     x21, x22, [x0, #16]
        stp     x23, x24, [x0, #32]
        stp     x25, x26, [x0, #48]
        stp     x27, x28, [x0, #64]
        stp     x29, x30, [x0, #80]
        mov     x1, sp
        str     x1, [x0, #96]
        stp     d8, d9, [x0, #104]
        stp     d10, d11, [x0, #120]
        stp     d12, d13, [x0, #136]
        stp     d14, d15, [x0, #152]
        mov     w0, #0
        ret

// void guest_longjmp(guest_jmp_buf env, int value)
FUNC(guest_longjmp)
        ldp     x19, x20, [x0, #0]
        ldp     x21, x22, [x0, #16]
        ldp     x23, x24, [x0, #32]
        ldp     x25, x26, [x0, #48]
        ldp     x27, x28, [x0, #64]
        ldp     x29, x30, [x0, #80]
        ldr     x2, [x0, #96]
        mov     sp, x2
        ldp     d8, d9, [x0, #104]
        ldp     d10, d11, [x0, #120]
        ldp     d12, d13, [x0, #136]
        ldp     d14, d15, [x0, #152]
        cmp     w1, #0
        csinc   w0, w1, wzr, ne
        ret

// uint32_t guest_on_stack(uint32_t (*fn)(uint32_t), uint32_t arg, void* stack_top)
FUNC(guest_on_stack)
        stp     x29, x30, [sp, #-32]!
        str     x19, [sp, #16]
        mov     x19, sp
        mov     sp, x2
        mov     x3, x0
        mov     w0, w1
        blr     x3
        mov     sp, x19
        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
#endif

// void run_case(uint64_t* io, void (*fn)(void)): io layout (diff/diff.h):
// [0..27] x0-x27 (x18 not loaded: the platform register on macOS),
// [32] nzcv, [33] fpcr in, [34] fpsr out, [36..99] v0-v31.
// The case code ends with ret; x28 holds io meanwhile.
FUNC(run_case)
        stp     x29, x30, [sp, #-176]!
        mov     x29, sp
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]
        stp     x23, x24, [sp, #48]
        stp     x25, x26, [sp, #64]
        stp     x27, x28, [sp, #80]
        stp     d8, d9, [sp, #96]
        stp     d10, d11, [sp, #112]
        stp     d12, d13, [sp, #128]
        stp     d14, d15, [sp, #144]
        str     x1, [sp, #160]
        mov     x28, x0
        msr     fpsr, xzr
        ldr     x2, [x28, #264]
        msr     fpcr, x2
        ldr     x2, [x28, #256]
        msr     nzcv, x2
        ldp     q0, q1, [x28, #288]
        ldp     q2, q3, [x28, #320]
        ldp     q4, q5, [x28, #352]
        ldp     q6, q7, [x28, #384]
        ldp     q8, q9, [x28, #416]
        ldp     q10, q11, [x28, #448]
        ldp     q12, q13, [x28, #480]
        ldp     q14, q15, [x28, #512]
        ldp     q16, q17, [x28, #544]
        ldp     q18, q19, [x28, #576]
        ldp     q20, q21, [x28, #608]
        ldp     q22, q23, [x28, #640]
        ldp     q24, q25, [x28, #672]
        ldp     q26, q27, [x28, #704]
        ldp     q28, q29, [x28, #736]
        ldp     q30, q31, [x28, #768]
        ldr     x30, [sp, #160]
        ldp     x0, x1, [x28, #0]
        ldp     x2, x3, [x28, #16]
        ldp     x4, x5, [x28, #32]
        ldp     x6, x7, [x28, #48]
        ldp     x8, x9, [x28, #64]
        ldp     x10, x11, [x28, #80]
        ldp     x12, x13, [x28, #96]
        ldp     x14, x15, [x28, #112]
        ldp     x16, x17, [x28, #128]
        ldp     x19, x20, [x28, #152]
        ldp     x21, x22, [x28, #168]
        ldp     x23, x24, [x28, #184]
        ldp     x25, x26, [x28, #200]
        ldr     x27, [x28, #216]
        blr     x30
        stp     x0, x1, [x28, #0]
        stp     x2, x3, [x28, #16]
        stp     x4, x5, [x28, #32]
        stp     x6, x7, [x28, #48]
        stp     x8, x9, [x28, #64]
        stp     x10, x11, [x28, #80]
        stp     x12, x13, [x28, #96]
        stp     x14, x15, [x28, #112]
        stp     x16, x17, [x28, #128]
        stp     x19, x20, [x28, #152]
        stp     x21, x22, [x28, #168]
        stp     x23, x24, [x28, #184]
        stp     x25, x26, [x28, #200]
        str     x27, [x28, #216]
        mrs     x1, nzcv
        str     x1, [x28, #256]
        mrs     x1, fpsr
        str     x1, [x28, #272]
        stp     q0, q1, [x28, #288]
        stp     q2, q3, [x28, #320]
        stp     q4, q5, [x28, #352]
        stp     q6, q7, [x28, #384]
        stp     q8, q9, [x28, #416]
        stp     q10, q11, [x28, #448]
        stp     q12, q13, [x28, #480]
        stp     q14, q15, [x28, #512]
        stp     q16, q17, [x28, #544]
        stp     q18, q19, [x28, #576]
        stp     q20, q21, [x28, #608]
        stp     q22, q23, [x28, #640]
        stp     q24, q25, [x28, #672]
        stp     q26, q27, [x28, #704]
        stp     q28, q29, [x28, #736]
        stp     q30, q31, [x28, #768]
        msr     fpcr, xzr
        msr     fpsr, xzr
        ldp     d14, d15, [sp, #144]
        ldp     d12, d13, [sp, #128]
        ldp     d10, d11, [sp, #112]
        ldp     d8, d9, [sp, #96]
        ldp     x27, x28, [sp, #80]
        ldp     x25, x26, [sp, #64]
        ldp     x23, x24, [sp, #48]
        ldp     x21, x22, [sp, #32]
        ldp     x19, x20, [sp, #16]
        ldp     x29, x30, [sp], #176
        ret
