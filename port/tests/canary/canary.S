@ Smallest possible process: no libnx, no libc, no game code.
@ Success is a black screen for 5 seconds, then a clean exit.  Failure is a crash.
@ Without RELOC_TEST it proves only that the NPDM yields a running process.
@ RELOC_TEST is kept to show what Atmosphere refuses: code made writable stays non-executable.

    .arch armv8-a
    .arm
    .syntax unified

    .set CUR_PROCESS, 0xffff8001

    .section .text.entry, "ax", %progbits
    .global _start
_start:
#ifdef RELOC_TEST
    ldr     r6, .Ltarget_off
.Lpc:
    add     r6, pc, r6
    add     r7, r6, #0x1000

    mov     r5, #3                  @ RW
    bl      protect

    movw    r0, #:lower16:0xE3A0002B
    movt    r0, #:upper16:0xE3A0002B
    str     r0, [r6]                @ becomes: mov r0, #43

    movw    r0, #:lower16:CUR_PROCESS
    movt    r0, #:upper16:CUR_PROCESS
    mov     r1, #0x1000
    mov     r2, r6
    mov     r3, #0
    mov     r4, #0
    svc     0x5f                    @ svcFlushProcessDataCache
    mov     r5, #6                  @ step marker for a crash report
    cmp     r0, #0
    bne     fail

    mov     r5, #5                  @ RX
    bl      protect

    blx     r6
    mov     r5, r0                  @ report what actually ran
    cmp     r0, #43
    movne   r0, #1
    bne     fail
#endif
    movw    r0, #0xF200             @ 5,000,000,000 ns
    movt    r0, #0x2A05
    mov     r1, #1
    svc     0x0b                    @ svcSleepThread
    svc     0x07                    @ svcExitProcess
1:  b       1b

#ifdef RELOC_TEST
protect:
    movw    r0, #:lower16:CUR_PROCESS
    movt    r0, #:upper16:CUR_PROCESS
    mov     r1, #0x1000
    mov     r2, r6
    mov     r3, #0
    mov     r4, #0
    svc     0x73                    @ svcSetProcessMemoryPermission
    cmp     r0, #0
    bxeq    lr
fail:
    mov     r2, r0
    mov     r1, r5
    mov     r0, #0
    svc     0x26                    @ svcBreak
2:  b       2b

    .balign 4
.Ltarget_off:
    .word   target - (.Lpc + 8)

    .section .text.target, "ax", %progbits
    .balign 0x1000
target:
    mov     r0, #42
    bx      lr
    .balign 0x1000, 0
#endif

    .section .rodata
    .word   0
    .data
    .word   0
