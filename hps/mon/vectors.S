@ SPDX-License-Identifier: GPL-3.0-or-later
.syntax unified
.arm
.section .text.vectors,"ax"

.balign 32
.globl vectors
vectors:
    b       .
    b       und_stub
    ldr     pc, fwd_svc
    b       pabt_stub
    b       dabt_stub
    b       .
    b       irq_stub
    b       fiq_stub

.globl fwd_und, fwd_svc, fwd_pabt_t, fwd_dabt, fwd_irq, fwd_fiq
fwd_und:    .word 0x04
fwd_svc:    .word 0x08
fwd_pabt_t: .word 0x0C
fwd_dabt:   .word 0x10
fwd_irq:    .word 0x18
fwd_fiq:    .word 0x1C

@ guest SP_mode parks in a TPIDR register; the frame is {spsr, pad, r0-r12, lr}
.macro STUB name, scr, top, fn, fwd, keep_a
\name:
    mcr     p15, 0, sp, c13, c0, \scr
    ldr     sp, =\top
    cpsid   a
    push    {r0-r12, lr}
    mrs     r0, spsr
    push    {r0, r1}
    mov     r0, sp
    bl      \fn
    pop     {r1, r2}
    msr     spsr_cxsf, r1
    cmp     r0, #0
    bne     1f
    pop     {r0-r12, lr}
    mrc     p15, 0, sp, c13, c0, \scr
    movs    pc, lr
1:  msr     APSR_nzcvq, r1
.if \keep_a
    msr     cpsr_x, r1
.endif
    pop     {r0-r12, lr}
    mrc     p15, 0, sp, c13, c0, \scr
    ldr     pc, \fwd
.ltorg
.endm

@ an Undefined entry leaves A as the guest had it, so a forward hands that A back
STUB und_stub, 4, __und_top, trap_und_c, fwd_und, 1
STUB pabt_stub, 3, __abt_top, trap_pabt_c, fwd_pabt_t, 0
STUB dabt_stub, 3, __abt_top, trap_dabt_c, fwd_dabt, 0
@ trap_irq_c steps lr back by 4 when it resumes the guest itself
STUB irq_stub, 2, __irq_top, trap_irq_c, fwd_irq, 0

@ the guest sp_fiq parks in fiq_sp (all TPIDRs are taken); trap_fiq_c keeps r4-r11
fiq_stub:
    str     sp, fiq_sp
    ldr     sp, =__fiq_top
    push    {r0-r3, r12, lr}
    mov     r0, lr
    mrs     r1, spsr
    bl      trap_fiq_c
    mrs     r1, spsr
    cmp     r0, #0
    bne     1f
    msr     APSR_nzcvq, r1
    pop     {r0-r3, r12, lr}
    ldr     sp, fiq_sp
    ldr     pc, fwd_fiq
@ deferred: F stays set until the interrupted context restores a CPSR with F clear; the level line retakes then
1:  orr     r1, r1, #0x40
    msr     spsr_c, r1
    pop     {r0-r3, r12, lr}
    ldr     sp, fiq_sp
    subs    pc, lr, #4
fiq_sp:
    .word   0
.ltorg

.text

.globl bank_get
bank_get:
    mrs     r2, cpsr
    bic     r3, r2, #0x1F
    orr     r3, r3, r0
    msr     cpsr_c, r3
    stm     r1, {r8-r12}
    str     sp, [r1, #20]
    str     lr, [r1, #24]
    msr     cpsr_c, r2
    bx      lr

@ r8-r12 are written only for FIQ, whose bank is not the caller's
.globl bank_put
bank_put:
    mrs     r2, cpsr
    bic     r3, r2, #0x1F
    orr     r3, r3, r0
    msr     cpsr_c, r3
    cmp     r0, #0x11
    ldmeq   r1, {r8-r12}
    ldr     sp, [r1, #20]
    ldr     lr, [r1, #24]
    msr     cpsr_c, r2
    bx      lr

.globl bank_spsr
bank_spsr:
    mrs     r2, cpsr
    bic     r3, r2, #0x1F
    orr     r3, r3, r0
    msr     cpsr_c, r3
    mrs     r0, spsr
    msr     cpsr_c, r2
    bx      lr

.globl bank_spsr_put
bank_spsr_put:
    mrs     r2, cpsr
    bic     r3, r2, #0x1F
    orr     r3, r3, r0
    msr     cpsr_c, r3
    msr     spsr_cxsf, r1
    msr     cpsr_c, r2
    bx      lr

.globl guest_enter
guest_enter:
    mov     r2, #0
    cps     #0x1B
    mov     sp, r2
    mov     lr, r2
    cps     #0x17
    mov     sp, r2
    mov     lr, r2
    cps     #0x12
    mov     sp, r2
    mov     lr, r2
    cps     #0x11
    mov     r8, r2
    mov     r9, r2
    mov     r10, r2
    mov     r11, r2
    mov     r12, r2
    mov     sp, r2
    mov     lr, r2
    cps     #0x1F
    mov     sp, r2
    mov     lr, r2
    cps     #0x13
    msr     spsr_cxsf, r1
    mov     lr, r0
    mov     r0, r2
    mov     r1, r2
    mov     r3, r2
    mov     r4, r2
    mov     r5, r2
    mov     r6, r2
    mov     r7, r2
    mov     r8, r2
    mov     r9, r2
    mov     r10, r2
    mov     r11, r2
    mov     r12, r2
    mov     sp, r2
    movs    pc, lr
