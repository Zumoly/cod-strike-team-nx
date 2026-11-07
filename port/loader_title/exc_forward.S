/* exc_forward.S -- kernel exceptions go to the NRO.
 *
 * The kernel enters a process at its own entry point on a user exception (r0 = type, r1 =
 * &ExceptionInfo, a tiny stack), which is this loader's crt0, not the NRO's.  Once the NRO is
 * mapped (g_nro_entry set by main.c), branch to its offset 0 with r0/r1/sp untouched: its crt0
 * sees r0 != 0, r1 != -1 and runs the port's handler (src/rt/exc32.S), which ends the exception
 * with svcReturnFromException itself.  Mono's null checks depend on this.  Before that, there
 * is nothing to handle a fault: break, so Atmosphere writes a crash report.
 */
    .syntax unified
    .arm

    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type __libnx_exception_entry, %function
    .align 2
__libnx_exception_entry:
    ldr     r2, 1f
0:  add     r2, pc, r2              @ r2 = &g_nro_entry, pc-relative: no relocation needed
    ldr     r2, [r2]
    cmp     r2, #0
    bxne    r2
    mov     r0, #0                  @ svcBreak(0, 0xC0D5EE00, type): no NRO yet
    ldr     r1, 2f
    svc     0x26
    b       .
1:  .word   g_nro_entry - (0b + 8)
2:  .word   0xC0D5EE00
    .size __libnx_exception_entry, . - __libnx_exception_entry
