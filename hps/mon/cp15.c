// SPDX-License-Identifier: GPL-3.0-or-later
#include "cp15.h"

#define CTRL_STORED 0xC000730Fu
#define CTRL_M      0x0001u
#define CTRL_A      0x0002u
#define CTRL_CSR    0x0304u
#define CTRL_V      0x2000u
#define TEST_CLEAN  0x40000000u

void cp15_reset(struct cp15_state *s, uint32_t mon_domain)
{
    uint32_t i;

    s->control = CP15_CTRL_RESET;
    s->ttbr = 0;
    s->dacr = 0;
    s->fsr = 0;
    s->far = 0;
    s->fcse = 0;
    s->ctxid = 0;
    for (i = 0; i < 8; i++)
        s->c15[i] = 0;
    s->mon_domain = mon_domain;
}

static int cond_pass(uint32_t cond, uint32_t flags)
{
    uint32_t n = flags >> 31 & 1u, z = flags >> 30 & 1u, c = flags >> 29 & 1u, v = flags >> 28 & 1u;

    switch (cond) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    case 14: return 1;
    default: return 0;
    }
}

static void act(struct cp15_result *r, uint32_t kind, uint32_t arg)
{
    r->act[r->nact].kind = kind;
    r->act[r->nact].arg = arg;
    r->nact++;
}

static void read_reg(struct cp15_state *s, struct cp15_result *r, uint32_t insn,
                     uint32_t crn, uint32_t crm, uint32_t op2)
{
    uint32_t v;

    switch (crn) {
    case 0: v = op2 == 1 ? CP15_CTR : CP15_ID; break;
    case 1: v = s->control | 0x70u; break;
    case 2: v = s->ttbr; break;
    case 3: v = s->dacr; break;
    case 5: v = s->fsr; break;
    case 6: v = s->far; break;
    case 7: v = TEST_CLEAN; break;
    case 8:
    case 9:
    case 10: v = 0; break;
    case 13: v = op2 == 1 ? s->ctxid : s->fcse; break;
    case 15: v = s->c15[crm & 7u]; break;
    default:
        act(r, CP15_ACT_FATAL_UNKNOWN, insn);
        return;
    }
    if ((insn >> 12 & 0xFu) == 15) {
        r->kind = CP15_RES_FLAGS;
        r->value = v & 0xF0000000u;
    } else {
        r->kind = CP15_RES_RD;
        r->value = v;
    }
}

static void write_c1(struct cp15_state *s, struct cp15_result *r, uint32_t wval)
{
    uint32_t nw = wval & CTRL_STORED;
    uint32_t d = s->control ^ nw;

    s->control = nw;
    if (d & CTRL_M)
        act(r, CP15_ACT_MMU_MODE, nw & CTRL_M);
    if (d & CTRL_CSR)
        act(r, CP15_ACT_SHADOW_RESET, 0);
    if (d & CTRL_V)
        act(r, CP15_ACT_VECTOR_BASE, nw & CTRL_V ? 0xFFFF0000u : 0);
    if (d & CTRL_A)
        act(r, CP15_ACT_ALIGN_MODE, nw >> 1 & 1u);
}

static void write_c7(struct cp15_result *r, uint32_t crm, uint32_t op2, uint32_t wval)
{
    uint32_t kind = CP15_ACT_NOP, arg = 0;

    switch (crm) {
    case 0:
        if (op2 == 4)
            kind = CP15_ACT_WFI;
        break;
    case 5:
        if (op2 <= 1)
            kind = CP15_ACT_IMB_ALL;
        break;
    case 6:
        if (op2 == 0) {
            kind = CP15_ACT_CLEAN_INV_D_ALL;
        } else if (op2 == 2) {
            kind = CP15_ACT_CLEAN_INV_D_SW;
            arg = wval;
        } else if (op2 == 1) {
            kind = CP15_ACT_INV_D_MVA;
            arg = wval;
        }
        break;
    case 7:
        if (op2 == 0)
            kind = CP15_ACT_IMB_ALL;
        break;
    case 10:
        if (op2 == 1) {
            kind = CP15_ACT_CLEAN_D_MVA;
            arg = wval;
        } else if (op2 == 2) {
            kind = CP15_ACT_CLEAN_D_SW;
            arg = wval;
        } else if (op2 == 3) {
            kind = CP15_ACT_CLEAN_INV_D_ALL;
        } else if (op2 == 4) {
            kind = CP15_ACT_DRAIN;
        }
        break;
    case 14:
        if (op2 == 1) {
            kind = CP15_ACT_CLEAN_INV_D_MVA;
            arg = wval;
        } else if (op2 == 2) {
            kind = CP15_ACT_CLEAN_INV_D_SW;
            arg = wval;
        } else if (op2 == 3) {
            kind = CP15_ACT_CLEAN_INV_D_ALL;
        }
        break;
    default:
        break;
    }
    act(r, kind, arg);
}

static void write_reg(struct cp15_state *s, struct cp15_result *r, uint32_t insn, uint32_t wval,
                      uint32_t crn, uint32_t crm, uint32_t op2)
{
    switch (crn) {
    case 0:
    case 9:
    case 10:
        act(r, CP15_ACT_NOP, 0);
        break;
    case 1:
        write_c1(s, r, wval);
        break;
    case 2:
        s->ttbr = wval & 0xFFFFC000u;
        act(r, CP15_ACT_SHADOW_RESET, 0);
        break;
    case 3: {
        uint32_t sh = s->mon_domain * 2u;

        s->dacr = wval;
        act(r, CP15_ACT_SET_DACR, (wval & ~(3u << sh)) | (1u << sh));
        break;
    }
    case 5:
        s->fsr = wval & 0xFFu;
        break;
    case 6:
        s->far = wval;
        break;
    case 7:
        write_c7(r, crm, op2, wval);
        break;
    case 8:
        if (crm >= 5 && crm <= 7 && op2 == 0)
            act(r, CP15_ACT_SHADOW_DROP_ALL, 0);
        else if (crm >= 5 && crm <= 7 && op2 == 1)
            act(r, CP15_ACT_SHADOW_DROP_MVA, wval);
        else
            act(r, CP15_ACT_NOP, 0);
        break;
    case 13:
        if (op2 == 1)
            s->ctxid = wval;
        else if (wval >> 25 != 0)
            act(r, CP15_ACT_FATAL_FCSE, wval);
        else
            s->fcse = 0;
        break;
    case 15:
        s->c15[crm & 7u] = wval;
        break;
    default:
        act(r, CP15_ACT_FATAL_UNKNOWN, insn);
        break;
    }
}

struct cp15_result cp15_exec(struct cp15_state *s, uint32_t insn, uint32_t rd_value, uint32_t nzcv)
{
    struct cp15_result r;
    uint32_t top;

    r.kind = CP15_RES_NONE;
    r.value = 0;
    r.nact = 0;
    if (!cond_pass(insn >> 28, nzcv))
        return r;

    top = insn >> 24 & 0xFu;
    if (top != 0xEu || !(insn & 0x10u)) {
        if (top == 0xCu || top == 0xDu || top == 0xEu)
            act(&r, CP15_ACT_UNDEF, insn);
        else
            act(&r, CP15_ACT_FATAL_UNKNOWN, insn);
        return r;
    }
    if ((insn >> 8 & 0xFu) != 15u) {
        act(&r, CP15_ACT_UNDEF, insn);
        return r;
    }
    if ((insn >> 21 & 7u) != 0) {
        act(&r, CP15_ACT_NOP, 0);
        return r;
    }

    if (insn & 0x100000u)
        read_reg(s, &r, insn, insn >> 16 & 0xFu, insn & 0xFu, insn >> 5 & 7u);
    else
        write_reg(s, &r, insn, rd_value, insn >> 16 & 0xFu, insn & 0xFu, insn >> 5 & 7u);
    return r;
}
