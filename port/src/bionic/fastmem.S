// NEON memcpy, memmove and memset for the game and the port.
//
// newlib's generic C versions copied 16 bytes per loop when both pointers were word-aligned
// and fell back to a byte loop otherwise.  Unity and Mono copy a lot of unaligned data, and
// memcpy was the hottest single routine on the main thread (7.5% of samples on hardware).
// Bionic's versions, which the game was built against, use NEON.
//
// Only VLD1/VST1 .8 forms are used: they have no alignment requirement, unlike LDM, LDRD and
// VLDM, which fault on Horizon when unaligned (docs/PORTING.md).  d0-d7 are caller-saved.
    .syntax unified
    .arm
    .fpu neon
    .text

    .global memcpy
    .type   memcpy, %function
    .p2align 5
memcpy:
    mov     ip, r0
    cmp     r2, #64
    blo     2f
1:  vld1.8  {d0-d3}, [r1]!
    vld1.8  {d4-d7}, [r1]!
    sub     r2, r2, #64
    pld     [r1, #128]
    vst1.8  {d0-d3}, [ip]!
    vst1.8  {d4-d7}, [ip]!
    cmp     r2, #64
    bhs     1b
2:  cmp     r2, #16
    blo     4f
3:  vld1.8  {d0-d1}, [r1]!
    sub     r2, r2, #16
    vst1.8  {d0-d1}, [ip]!
    cmp     r2, #16
    bhs     3b
4:  cmp     r2, #8
    blo     5f
    vld1.8  {d0}, [r1]!
    sub     r2, r2, #8
    vst1.8  {d0}, [ip]!
5:  cmp     r2, #0
    bxeq    lr
6:  ldrb    r3, [r1], #1
    subs    r2, r2, #1
    strb    r3, [ip], #1
    bne     6b
    bx      lr
    .size   memcpy, . - memcpy

// Forward copying is safe unless the destination starts inside the source.
    .global memmove
    .type   memmove, %function
    .p2align 5
memmove:
    subs    r3, r0, r1          // dst - src
    cmphi   r2, r3              // dst above src: do the ranges overlap?
    bls     memcpy
    add     r1, r1, r2
    add     ip, r0, r2
    cmp     r2, #16
    blo     2f
1:  sub     r1, r1, #16
    sub     ip, ip, #16
    vld1.8  {d0-d1}, [r1]
    sub     r2, r2, #16
    vst1.8  {d0-d1}, [ip]
    cmp     r2, #16
    bhs     1b
2:  cmp     r2, #0
    bxeq    lr
3:  ldrb    r3, [r1, #-1]!
    subs    r2, r2, #1
    strb    r3, [ip, #-1]!
    bne     3b
    bx      lr
    .size   memmove, . - memmove

    .global memset
    .type   memset, %function
    .p2align 5
memset:
    mov     ip, r0
    vdup.8  q0, r1
    vmov    q1, q0
    cmp     r2, #32
    blo     2f
1:  vst1.8  {d0-d3}, [ip]!
    sub     r2, r2, #32
    cmp     r2, #32
    bhs     1b
2:  cmp     r2, #8
    blo     3f
    vst1.8  {d0}, [ip]!
    sub     r2, r2, #8
    b       2b
3:  cmp     r2, #0
    bxeq    lr
4:  strb    r1, [ip], #1
    subs    r2, r2, #1
    bne     4b
    bx      lr
    .size   memset, . - memset

    .section .note.GNU-stack, "", %progbits
