@ Named stubs for imports with no implementation.  Each entry passes its index,
@ which so_missing() turns back into the symbol name.

    .arch armv8-a
    .arm
    .syntax unified

    .section .text.so_thunks, "ax", %progbits
    .global so_thunks
so_thunks:
    .set    i, 0
    .rept   2048
    movw    ip, #i
    b       so_thunk_common
    .set    i, i + 1
    .endr

so_thunk_common:
    mov     r0, ip
    b       so_missing
