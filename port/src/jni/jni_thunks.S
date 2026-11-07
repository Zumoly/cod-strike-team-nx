@ One entry per JNIEnv/JavaVM slot, so an unimplemented call reports its slot number.

    .arch armv8-a
    .arm
    .syntax unified

    .section .text.jni_thunks, "ax", %progbits
    .global jni_thunks
jni_thunks:
    .set    i, 0
    .rept   240
    movw    ip, #i
    b       jni_thunk_common
    .set    i, i + 1
    .endr

jni_thunk_common:
    mov     r0, ip
    b       jni_missing
