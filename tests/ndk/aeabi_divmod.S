@ The ARM EABI 64-bit division helpers libgcc provides to NDK code
@ (compiler-rt has them only as part of its arm/ directory): quotient in
@ r0:r1, remainder in r2:r3. Built into libndkrt.a for the tests/ndk
@ libraries, which link it statically as NDK libraries link libgcc.
	.syntax unified
	.arm
	.text

	.globl __aeabi_ldivmod
	.type __aeabi_ldivmod, %function
__aeabi_ldivmod:
	push {r11, lr}
	sub sp, sp, #16
	add r12, sp, #8
	str r12, [sp]           @ long long* rem, the fifth argument
	bl __divmoddi4
	ldr r2, [sp, #8]
	ldr r3, [sp, #12]
	add sp, sp, #16
	pop {r11, pc}

	.globl __aeabi_uldivmod
	.type __aeabi_uldivmod, %function
__aeabi_uldivmod:
	push {r11, lr}
	sub sp, sp, #16
	add r12, sp, #8
	str r12, [sp]
	bl __udivmoddi4
	ldr r2, [sp, #8]
	ldr r3, [sp, #12]
	add sp, sp, #16
	pop {r11, pc}

	.section .note.GNU-stack,"",%progbits
