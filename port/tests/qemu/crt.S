@ Entry point and raw Linux access for the emulated ARM harness.

    .arch armv8-a
    .arm
    .syntax unified

    .section .text._start, "ax", %progbits
    .global _start
_start:
    ldr     sp, =qemu_main_stack_top
    bl      qemu_main
1:  b       1b

@ long sc(long n, long a, long b, long c, long d, long e, long f)
    .section .text.sc, "ax", %progbits
    .global sc
sc:
    mov     ip, sp
    push    {r4, r5, r7}
    mov     r7, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldm     ip, {r3, r4, r5}
    svc     0
    pop     {r4, r5, r7}
    bx      lr

@ int qemu_clone(flags, stack, ptid, tls, ctid, fn, arg): the child never returns.
    .section .text.qemu_clone, "ax", %progbits
    .global qemu_clone
qemu_clone:
    mov     ip, sp
    push    {r4, r5, r6, r7}
    ldm     ip, {r4, r5, r6}        @ ctid, fn, arg
    mov     r7, #120                @ clone
    svc     0
    cmp     r0, #0
    beq     1f
    pop     {r4, r5, r6, r7}
    bx      lr
1:  mov     r0, r6
    blx     r5
2:  mov     r0, #0
    mov     r7, #1                  @ exit (thread only)
    svc     0
    b       2b

@ newlib's exit path calls these; the startup files that define them are not linked.
    .section .text._fini, "ax", %progbits
    .global _fini
    .global _init
_fini:
_init:
    bx      lr

@ -mtp=soft: TPIDRURO is set by clone/set_tls.
    .section .text.__aeabi_read_tp, "ax", %progbits
    .global __aeabi_read_tp
__aeabi_read_tp:
    mrc     p15, 0, r0, c13, c0, 3
    bx      lr

    .bss
    .balign 16
    .global qemu_main_stack
qemu_main_stack:
    .space  8 * 1024 * 1024
qemu_main_stack_top:
