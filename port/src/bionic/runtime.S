@ Pieces that must be exact at the instruction level.

    .arch armv8-a
    .fpu neon-fp-armv8
    .arm
    .syntax unified

    .macro  FUNC name
    .section .text.\name, "ax", %progbits
    .global \name
    .type \name, %function
\name:
    .endm

@ Same contract as the Linux helper at 0xffff0fc0:
@ r0 = expected, r1 = new, r2 = ptr; returns r0 = 0 and C set on success.
@ May clobber r3, ip and flags; preserves everything else, so no stack use.
FUNC port_kuser_cmpxchg
    dmb     ish
1:  ldrex   r3, [r2]
    subs    r3, r3, r0
    strexeq r3, r1, [r2]
    teqeq   r3, #1
    beq     1b
    rsbs    r0, r3, #0
    dmb     ish
    bx      lr

FUNC port_kuser_barrier
    dmb     ish
    bx      lr

@ bionic's jmp_buf has room for 64 words.  newlib's ARMv4T setjmp would skip d8-d15,
@ which AAPCS requires a longjmp to restore.
FUNC b_setjmp
    mov     ip, sp
    stmia   r0!, {r4-r11, ip, lr}
    vstmia  r0, {d8-d15}
    mov     r0, #0
    bx      lr

FUNC b_longjmp
    ldmia   r0!, {r4-r11, ip, lr}
    vldmia  r0, {d8-d15}
    mov     sp, ip
    movs    r0, r1
    moveq   r0, #1
    bx      lr
