// SPDX-License-Identifier: GPL-3.0-or-later
        .syntax unified
        .arm

        .equ    OBJ_OLD, 0x157c
        .equ    RING, 0x58
        .equ    RING_CNT, 0x1264
        .equ    TX_BUF, 0x1268
        .equ    TX_LEN, 0x1534
        .equ    RD_DES, 0x1518
        .equ    RD_LEN, 0x151c
        .equ    RD_THR, 0x1470
        .equ    RD_PEND, 0x1524
        .equ    WR_PEND, 0x153c
        .equ    RD_WAIT, 0x1550

        .global bt_st_off
        .set    bt_st_off, OBJ_OLD

        // BTLDD write tail after the TX copy: the DFC enque becomes a tail call with this in r0
        .section .hook, "ax"
        mov     r0, r6
        pop     {r4, r5, r6, lr}
        b       bt_glue_write

        // DLogicalChannel factory 0x503f2874 news and zero-fills the object; struct bt_st lives at its old end
        .section .objsize, "a"
        .word   OBJ_OLD + ((BT_ST_SIZE + 3) & ~3)

        .section .text.glue, "ax"
        .global bt_glue_write
        .type   bt_glue_write, %function
bt_glue_write:
        push    {r4, r5, r6, r7, r8, lr}
        mov     r4, r0
        ldr     r5, =OBJ_OLD
        add     r5, r4, r5
        ldr     r1, =TX_BUF
        add     r1, r4, r1
        ldr     r2, =TX_LEN
        ldr     r2, [r4, r2]
        mov     r0, r5
        bl      bt_hci_cmd
        movs    r6, r0
        beq     done
        // the ROM put-advance aborts every request once the count reaches 4096
        ldr     r3, =RING_CNT
        ldr     r3, [r4, r3]
        rsb     r3, r3, #4096
        cmp     r6, r3
        bhs     full
        ldr     r7, =BT_ST_OUT
        add     r7, r5, r7
        add     r8, r4, #RING
1:      mov     r0, r8
        ldrb    r1, [r7], #1
        bl      btldd_ring_put
        subs    r6, r6, #1
        bne     1b
        b       done
full:   ldr     r3, [r5, #BT_ST_FULL]
        add     r3, r3, #1
        str     r3, [r5, #BT_ST_FULL]
        // write done with KErrNone as the last LPRF step 0x503f24f4 does; 0x503f262c would abort the read too
done:   ldr     r3, =WR_PEND
        mov     r2, #0
        str     r2, [r4, r3]
        mov     r0, r4
        mov     r1, #1
        bl      btldd_complete
        // a read that found too few bytes waits on +0x1550; serve it as the RX DFC 0x503f1e4c does
        ldr     r3, =RD_WAIT
        ldr     r3, [r4, r3]
        cmp     r3, #0
        popeq   {r4, r5, r6, r7, r8, pc}
        ldr     r3, =RD_LEN
        ldr     r2, [r4, r3]
        ldr     r3, =RING_CNT
        ldr     r3, [r4, r3]
        cmp     r2, r3
        pophi   {r4, r5, r6, r7, r8, pc}
        add     r0, r4, #RING
        ldr     r3, =RD_DES
        ldr     r1, [r4, r3]
        ldr     r3, =RD_THR
        ldr     r3, [r4, r3]
        bl      btldd_ring_read
        mov     r1, #0
        ldr     r3, =RD_WAIT
        str     r1, [r4, r3]
        ldr     r3, =RD_PEND
        str     r1, [r4, r3]
        mov     r0, r4
        bl      btldd_complete
        pop     {r4, r5, r6, r7, r8, pc}
        .ltorg
        .size   bt_glue_write, . - bt_glue_write
