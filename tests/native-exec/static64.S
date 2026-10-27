// A static arm64 program (topic native-exec): no libc, no PT_INTERP; Linux
// syscalls of its own. Prints "static: ARGV1" and exits with 7.
        .text
        .globl _start
_start:
        ldr     x19, [sp]               // argc
        mov     x0, #1
        adr     x1, msg
        mov     x2, #(msgend - msg)
        mov     x8, #64                 // write
        svc     #0
        cmp     x19, #2
        b.lt    1f
        ldr     x20, [sp, #16]          // argv[1]
        mov     x21, x20
2:      ldrb    w22, [x21], #1
        cbnz    w22, 2b
        sub     x2, x21, x20
        sub     x2, x2, #1
        mov     x0, #1
        mov     x1, x20
        mov     x8, #64
        svc     #0
1:      mov     x0, #1
        adr     x1, nl
        mov     x2, #1
        mov     x8, #64
        svc     #0
        mov     x0, #7
        mov     x8, #94                 // exit_group
        svc     #0
msg:    .ascii  "static: "
msgend:
nl:     .ascii  "\n"
