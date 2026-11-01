@ SPDX-License-Identifier: GPL-3.0-or-later
.syntax unified
.arm
#include "../common/platform.h"
.section .text.start,"ax"

@ runs at PA 0x38000000 with the MMU off until the jump to va_entry: PC-relative code only
.globl _start
_start:
    b       1f
    @ image word 1: ngl reset-starts core 1 here on a park-less return only when it reads this marker
    .word   PL_C1RST_MARK
    .word   PL_TT_MARK
1:  cpsid   aif, #0x13
    @ the flag means core 1 restores its banked GIC words and stops; no monitor init runs
    ldr     r0, =PL_C1RST_FLAG
    ldr     r1, [r0]
    ldr     r2, =PL_C1RST_ARM
    cmp     r1, r2
    beq     c1rst
    mrc     p15, 0, r0, c1, c0, 0
    ldr     r1, =0x72003C07
    bic     r0, r0, r1
    mcr     p15, 0, r0, c1, c0, 0
    isb
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 6
    mcr     p15, 0, r0, c8, c7, 0
    dsb
    isb
    bl      dcache_inval_all
    mrc     p15, 0, r0, c1, c0, 1
    bic     r0, r0, #0x47
    mcr     p15, 0, r0, c1, c0, 1
    mov     r0, #0
    mcr     p15, 0, r0, c1, c0, 2
    isb

    ldr     r4, =0x38104000
    add     r1, r4, #0x4000
    mov     r0, r4
    mov     r2, #0
1:  str     r2, [r0], #4
    cmp     r0, r1
    blo     1b
    ldr     r2, =0x38004406
    str     r2, [r4, #(0x380 * 4)]
    ldr     r3, =_start
    lsr     r3, r3, #20
    str     r2, [r4, r3, lsl #2]
    ldr     r2, =0x38104416
    add     r3, r3, #1
    str     r2, [r4, r3, lsl #2]
    dsb

    mov     r0, #1
    mcr     p15, 0, r0, c3, c0, 0
    mov     r0, #0
    mcr     p15, 0, r0, c2, c0, 2
    mcr     p15, 0, r0, c13, c0, 1
    orr     r0, r4, #0x40
    mcr     p15, 0, r0, c2, c0, 0
    isb
    mrc     p15, 0, r0, c1, c0, 0
    ldr     r1, =0x1C07
    orr     r0, r0, r1
    dsb
    mcr     p15, 0, r0, c1, c0, 0
    isb
    ldr     pc, =va_entry
.ltorg

va_entry:
    ldr     r0, =vectors
    mcr     p15, 0, r0, c12, c0, 0
    isb
    cps     #0x1B
    ldr     sp, =__und_top
    cps     #0x17
    ldr     sp, =__abt_top
    cps     #0x12
    ldr     sp, =__irq_top
    cps     #0x11
    ldr     sp, =__fiq_top
    cps     #0x13
    ldr     sp, =__svc_top
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
2:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     2b
    bl      mon_main
3:  wfe
    b       3b
.ltorg

@ MMU and caches off: gic_c1rst is read at its PA, found from the PC-relative and linked _start
c1rst:
    adr     r5, _start
    ldr     r6, =_start
    sub     r5, r5, r6
    ldr     r4, =gic_c1rst
    add     r4, r4, r5
    ldr     r7, =gic_c1rst_n
    add     r7, r7, r5
    ldr     r7, [r7]
1:  subs    r7, r7, #1
    bmi     2f
    ldmia   r4!, {r0, r1, r2}
    str     r1, [r0]
    dsb
    b       1b
2:  ldr     r0, =PL_C1RST_DONE
    ldr     r1, =PL_C1RST_DONE_V
    str     r1, [r0]
    dsb
3:  wfe
    b       3b
.ltorg

.macro DCSW name, insn
.globl \name
\name:
    mov     r1, #0
1:  mov     r2, #0
2:  mov     r3, r1, lsl #30
    orr     r3, r3, r2, lsl #5
    \insn
    add     r2, r2, #1
    cmp     r2, #256
    blt     2b
    add     r1, r1, #1
    cmp     r1, #4
    blt     1b
    dsb
    bx      lr
.endm

DCSW dcache_inval_all, "mcr p15, 0, r3, c7, c6, 2"
DCSW dcache_clean_all, "mcr p15, 0, r3, c7, c10, 2"
DCSW dcache_clean_inv_all, "mcr p15, 0, r3, c7, c14, 2"
