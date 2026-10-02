/*
 * hevcdec_svc.S - the HEVC block's registers from RISC OS: an OS_Memory 13
 * mapping isn't user readable, so reads and writes happen in SVC mode
 * (OS_EnterOS ... OS_LeaveOS). A DSB first, so that what the program wrote
 * to the block's (uncached, buffered) memory has reached it before a
 * register write can start the block.
 *
 * Part of riscos-reelhwaccel (hevcdec/). GPL version 2 or later.
 */
        .syntax unified
        .arm
        .arch   armv7-a
        .text

/* void hevcdec_svc_writes(uint32_t base, const uint32_t *pairs, int n):
   n (offset, value) pairs, in order */
        .global hevcdec_svc_writes
        .type   hevcdec_svc_writes, %function
hevcdec_svc_writes:
        push    {r4-r6, lr}
        mov     r4, r0
        mov     r5, r1
        mov     r6, r2
        swi     0x16                            @ OS_EnterOS
        dsb
        cmp     r6, #0
        ble     2f
1:      ldmia   r5!, {r0, r1}
        str     r1, [r4, r0]
        subs    r6, r6, #1
        bne     1b
        dsb
2:      swi     0x7C                            @ OS_LeaveOS
        pop     {r4-r6, pc}
        .size   hevcdec_svc_writes, . - hevcdec_svc_writes

/* uint32_t hevcdec_svc_read(uint32_t addr) */
        .global hevcdec_svc_read
        .type   hevcdec_svc_read, %function
hevcdec_svc_read:
        push    {r4, lr}
        mov     r4, r0
        swi     0x16
        dsb
        ldr     r4, [r4]
        swi     0x7C
        mov     r0, r4
        pop     {r4, pc}
        .size   hevcdec_svc_read, . - hevcdec_svc_read

        .section .note.GNU-stack, "", %progbits
