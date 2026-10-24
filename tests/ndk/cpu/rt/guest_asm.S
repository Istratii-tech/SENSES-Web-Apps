@ Assembly parts of the guest test runtime (ARM state, callable from ARM
@ and Thumb code through interworking): 64-bit divmod helpers, setjmp /
@ longjmp and running a function on another stack.
        .syntax unified
        .arm
        .text

@ uint64 quotient in r0:r1, remainder in r2:r3.
        .globl __aeabi_uldivmod
        .type __aeabi_uldivmod, %function
__aeabi_uldivmod:
        push    {r4, lr}
        sub     sp, sp, #16
        add     r4, sp, #8
        str     r4, [sp]
        bl      __udivmoddi4
        ldr     r2, [sp, #8]
        ldr     r3, [sp, #12]
        add     sp, sp, #16
        pop     {r4, pc}

        .globl __aeabi_ldivmod
        .type __aeabi_ldivmod, %function
__aeabi_ldivmod:
        push    {r4, lr}
        sub     sp, sp, #16
        add     r4, sp, #8
        str     r4, [sp]
        bl      __divmoddi4
        ldr     r2, [sp, #8]
        ldr     r3, [sp, #12]
        add     sp, sp, #16
        pop     {r4, pc}

@ int guest_setjmp(guest_jmp_buf env): saves r4-r11, sp, lr.
        .globl guest_setjmp
        .type guest_setjmp, %function
guest_setjmp:
        stmia   r0, {r4-r11}
        str     sp, [r0, #32]
        str     lr, [r0, #36]
        mov     r0, #0
        bx      lr

@ void guest_longjmp(guest_jmp_buf env, int value)
        .globl guest_longjmp
        .type guest_longjmp, %function
guest_longjmp:
        ldmia   r0, {r4-r11}
        ldr     sp, [r0, #32]
        ldr     lr, [r0, #36]
        movs    r0, r1
        moveq   r0, #1
        bx      lr

@ uint32_t guest_on_stack(uint32_t (*fn)(uint32_t), uint32_t arg, void* stack_top)
        .globl guest_on_stack
        .type guest_on_stack, %function
guest_on_stack:
        push    {r4, lr}
        mov     r4, sp
        mov     sp, r2
        mov     r3, r0
        mov     r0, r1
        blx     r3
        mov     sp, r4
        pop     {r4, pc}
