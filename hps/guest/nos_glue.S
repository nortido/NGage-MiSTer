// SPDX-License-Identifier: GPL-3.0-or-later
        .syntax unified
        .arm

        .equ    RET0, 0x50324598

        // NOS DLL export table slots nosdriver calls through its thunks 0x511b4464 (export 4) and 0x511b4458 (export 6)
        .section .exp4, "a"
        .word   nos_attach
        .section .exp6, "a"
        .word   nos_ctor

        // DoCreate's new(8): room for struct nos_ch after the 8-byte object
        .section .objsize, "ax"
        mov     r0, #(8 + NOS_CH_SIZE)

        // vt+0xc, +0x10, +0x14 are what the channel calls; slots 0 and 4 stay RET0 as in the earlier stub's VTBL
        .section .rodata.nosv, "a"
        .balign 4
        .global nos_vtbl
        .type   nos_vtbl, %object
nos_vtbl:
        .word   RET0, RET0, nos_dtor, nos_cancel, nos_request, nos_control
        .size   nos_vtbl, . - nos_vtbl
