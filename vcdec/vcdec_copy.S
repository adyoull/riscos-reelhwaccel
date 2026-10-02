/*
 * vcdec_copy.S - copies out of the decoder's picture buffers, and a call
 * into the kernel's cache maintenance, for vcdec.c.
 *
 * PCI_RAMAlloc memory is uncachable and privileged (a USR mode access
 * aborts), so it is read in SVC mode: OS_EnterOS, the copy, OS_LeaveOS.
 * Picture buffers in a Physical Memory Pool (VCDEC_OUT_PMP) are user
 * readable, and copied in USR mode.
 *
 * Three ways to copy an aligned run (VCDEC_COPY_*), then words, then
 * bytes; anything unaligned by bytes. Exactly n bytes are written.
 *   0 LDM/STM four words, twice a loop (506-559 MB/s out of PCI memory on
 *     a Pi 4, against 131-139 by single words);
 *   1 LDM/STM eight words a loop;
 *   2 NEON, 64 bytes a loop (VLD1/VST1 of byte elements: no alignment
 *     needed), in USR mode only. In SVC mode an IRQ could leave the
 *     program's VFP context lazily switched off, and the trap would be
 *     taken in SVC mode; there way 2 copies by LDM 8 instead.
 *
 * VCDEC_HOST (the host tests, as an arm-linux program): the SWIs become
 * calls to vcdec_host_enter_os and vcdec_host_leave_os, which open and close
 * the fake's privileged memory.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
        .syntax unified
        .arm
        .fpu    neon
        .text

#ifdef VCDEC_HOST
        .macro  ENTER_OS
        push    {r0-r3, r12, lr}
        bl      vcdec_host_enter_os
        pop     {r0-r3, r12, lr}
        .endm
        .macro  LEAVE_OS
        push    {r0-r3, r12, lr}
        bl      vcdec_host_leave_os
        pop     {r0-r3, r12, lr}
        .endm
#else
        .macro  ENTER_OS
        swi     0x16                    @ OS_EnterOS
        .endm
        .macro  LEAVE_OS
        swi     0x7C                    @ OS_LeaveOS
        .endm
#endif

/* The inner copies: r0 dst, r1 src, r2 n. Each corrupts r0-r3, r7, r8,
   r12 and nothing else (copy_ldm8 saves what more it uses). */

/* the tail, shared: r2 = bytes left; words if aligned, then bytes */
        .type   copy_tail, %function
copy_tail:
        orr     r12, r0, r1
        tst     r12, #3
        bne     5f
        subs    r2, r2, #4
        blt     4f
3:      ldr     r3, [r1], #4
        str     r3, [r0], #4
        subs    r2, r2, #4
        bge     3b
4:      adds    r2, r2, #4              @ 0 to 3 bytes left
5:      cmp     r2, #0
        bxeq    lr
6:      ldrb    r3, [r1], #1
        strb    r3, [r0], #1
        subs    r2, r2, #1
        bne     6b
        bx      lr
        .size   copy_tail, . - copy_tail

        .type   copy_ldm4, %function
copy_ldm4:
        orr     r12, r0, r1
        tst     r12, #3
        bne     copy_tail
        subs    r2, r2, #32
        blt     2f
1:      ldmia   r1!, {r3, r7, r8, r12}
        stmia   r0!, {r3, r7, r8, r12}
        ldmia   r1!, {r3, r7, r8, r12}
        stmia   r0!, {r3, r7, r8, r12}
        subs    r2, r2, #32
        bge     1b
2:      add     r2, r2, #32
        b       copy_tail
        .size   copy_ldm4, . - copy_ldm4

        .type   copy_ldm8, %function
copy_ldm8:
        orr     r12, r0, r1
        tst     r12, #3
        bne     copy_tail
        push    {r4-r6, r9}
        subs    r2, r2, #32
        blt     2f
1:      ldmia   r1!, {r3-r9, r12}
        stmia   r0!, {r3-r9, r12}
        subs    r2, r2, #32
        bge     1b
2:      pop     {r4-r6, r9}
        add     r2, r2, #32
        b       copy_tail
        .size   copy_ldm8, . - copy_ldm8

        .type   copy_neon, %function
copy_neon:
        orr     r12, r0, r1
        tst     r12, #3
        bne     copy_tail
        subs    r2, r2, #64
        blt     2f
1:      vld1.8  {d0-d3}, [r1]!
        vld1.8  {d4-d7}, [r1]!
        vst1.8  {d0-d3}, [r0]!
        vst1.8  {d4-d7}, [r0]!
        subs    r2, r2, #64
        bge     1b
2:      add     r2, r2, #64
        b       copy_tail
        .size   copy_neon, . - copy_neon

/* void vcdec_svc_copy(void *dst, const void *src, size_t n): in SVC mode, LDM 4 */
        .global vcdec_svc_copy
        .type   vcdec_svc_copy, %function
vcdec_svc_copy:
        push    {r4-r8, lr}
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        ENTER_OS
        mov     r0, r4
        mov     r1, r5
        mov     r2, r6
        bl      copy_ldm4
        LEAVE_OS
        pop     {r4-r8, pc}
        .size   vcdec_svc_copy, . - vcdec_svc_copy

/* void vcdec_copy_rows(uint8_t *dst, int dst_stride, const uint8_t *src,
                        int src_stride, int width, int rows, int mode)
   mode: bits 0-1 the way (0 LDM 4, 1 LDM 8, 2 NEON); bit 2 in SVC mode */
        .macro  ROWS inner
        cmp     r11, #0
        ble     9f
        cmp     r10, #0
        ble     9f
1:      mov     r0, r4
        mov     r1, r6
        mov     r2, r10
        bl      \inner
        add     r4, r4, r5
        add     r6, r6, r9
        subs    r11, r11, #1
        bne     1b
9:
        .endm

        .global vcdec_copy_rows
        .type   vcdec_copy_rows, %function
vcdec_copy_rows:
        push    {r4-r12, lr}            @ (ten: the stack stays 8-byte aligned)
        ldr     r10, [sp, #40]          @ width
        ldr     r11, [sp, #44]          @ rows
        ldr     r12, [sp, #48]          @ mode
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        mov     r9, r3
        tst     r12, #4
        beq     20f
        ENTER_OS                        @ (SVC mode: no NEON; way 2 copies by LDM 8)
        and     r0, r12, #3
        cmp     r0, #0
        bne     12f
        ROWS    copy_ldm4
        b       19f
12:     ROWS    copy_ldm8
19:     LEAVE_OS
        pop     {r4-r12, pc}
20:     and     r0, r12, #3             @ (user mode: no SWIs)
        cmp     r0, #1
        beq     22f
        cmp     r0, #2
        beq     23f
        ROWS    copy_ldm4
        pop     {r4-r12, pc}
22:     ROWS    copy_ldm8
        pop     {r4-r12, pc}
23:     ROWS    copy_neon
        pop     {r4-r12, pc}
        .size   vcdec_copy_rows, . - vcdec_copy_rows

/* void vcdec_svc_call(void (*fn)(), uint32_t r0, uint32_t r1): fn called in
   SVC mode with r0 and r1 (a kernel ARMop: it may corrupt r0, lr, flags) */
        .global vcdec_svc_call
        .type   vcdec_svc_call, %function
vcdec_svc_call:
        push    {r4-r6, lr}
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        ENTER_OS
        mov     r0, r5
        mov     r1, r6
        blx     r4
        LEAVE_OS
        pop     {r4-r6, pc}
        .size   vcdec_svc_call, . - vcdec_svc_call

        .section .note.GNU-stack, "", %progbits
