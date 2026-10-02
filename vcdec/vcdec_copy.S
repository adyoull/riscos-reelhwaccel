/*
 * vcdec_copy.S - copies to and from PCI_RAMAlloc memory, which is
 * privileged (a USR mode access aborts), so in SVC mode: OS_EnterOS, the
 * copy, OS_LeaveOS. Aligned runs go 32 bytes a loop by LDM/STM (four
 * words, twice: 506-559 MB/s out of PCI memory on a Pi 4, against 131-139
 * by single words), then words, then bytes; anything unaligned by bytes.
 * Exactly n bytes are written.
 *
 * In SVC mode the stack isn't touched and r14 (r14_svc) is only scratch.
 *
 * VCDEC_HOST (the host tests, as an arm-linux program): the SWIs become
 * calls to vcdec_host_enter_os and vcdec_host_leave_os, which open and close
 * the fake's privileged memory.
 *
 * Part of riscos-reelhwaccel. GPL version 2 or later (see COPYING).
 */
        .syntax unified
        .arm
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

/* r0 dst, r1 src, r2 n: copies; corrupts r0-r3, r7, r8, r12 */
        .type   copy_inner, %function
copy_inner:
        orr     r12, r0, r1
        tst     r12, #3
        bne     5f
        subs    r2, r2, #32
        blt     2f
1:      ldmia   r1!, {r3, r7, r8, r12}
        stmia   r0!, {r3, r7, r8, r12}
        ldmia   r1!, {r3, r7, r8, r12}
        stmia   r0!, {r3, r7, r8, r12}
        subs    r2, r2, #32
        bge     1b
2:      adds    r2, r2, #28             @ (32 - 4)
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
        .size   copy_inner, . - copy_inner

/* void vcdec_svc_copy(void *dst, const void *src, size_t n) */
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
        bl      copy_inner
        LEAVE_OS
        pop     {r4-r8, pc}
        .size   vcdec_svc_copy, . - vcdec_svc_copy

/* void vcdec_svc_copy_rows(uint8_t *dst, int dst_stride, const uint8_t *src,
                            int src_stride, int width, int rows) */
        .global vcdec_svc_copy_rows
        .type   vcdec_svc_copy_rows, %function
vcdec_svc_copy_rows:
        push    {r4-r12, lr}            @ (ten: the stack stays 8-byte aligned)
        ldr     r10, [sp, #40]          @ width
        ldr     r11, [sp, #44]          @ rows
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        mov     r9, r3
        ENTER_OS
        cmp     r11, #0
        ble     2f
        cmp     r10, #0
        ble     2f
1:      mov     r0, r4
        mov     r1, r6
        mov     r2, r10
        bl      copy_inner
        add     r4, r4, r5
        add     r6, r6, r9
        subs    r11, r11, #1
        bne     1b
2:      LEAVE_OS
        pop     {r4-r12, pc}
        .size   vcdec_svc_copy_rows, . - vcdec_svc_copy_rows

        .section .note.GNU-stack, "", %progbits
