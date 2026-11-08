// SPDX-License-Identifier: GPL-3.0-or-later
#include "xdiag.h"

#if NGH_XDIAG

#define XD_M_USR 0x10u
#define XD_M_FIQ 0x11u
#define XD_M_SYS 0x1Fu

#define XD_F_FILLED 0x1u
#define XD_F_LRLOST 0x2u
#define XD_F_SPSRNA 0x4u
#define XD_F_FROMHK 0x38u

static uint32_t xd_bank_mode(const struct xd_in *in)
{
    uint32_t m = in->gm & 0x1Fu;

    return m == XD_M_USR ? XD_M_SYS : m;
}

// the trap overwrote the banked r14 and SPSR of the mode it ran in, and r13 with it
static int xd_lost(const struct xd_in *in)
{
    return xd_bank_mode(in) == (in->emode & 0x1Fu);
}

int xd_note(struct xd *x)
{
    x->nfwd++;
    if (x->taken)
        return 0;
    x->taken = 1;
    return 1;
}

uint32_t xd_reg(const struct xd_in *in, unsigned n)
{
    if (n < 8u)
        return in->fr[n];
    if (n < 13u)
        return (in->gm & 0x1Fu) == XD_M_FIQ ? in->fiq[n - 8u] : in->fr[n];
    if (n == 13u)
        return xd_lost(in) ? in->ssp : in->bsp;
    if (n == 14u)
        return xd_lost(in) ? 0 : in->blr;
    return n == 15u ? in->pc : 0;
}

uint32_t xd_probe_va(const struct xd_in *in, unsigned i)
{
    switch (i) {
    case 0:
        return 0x938u;
    case 1:
        return 0x93Cu;
    case 2:
        return 0x830u;
    case 3:
        return xd_reg(in, 10) & ~3u;
    case 4:
        return (in->fr[1] - 4u) & ~3u;
    case 5:
        return 0x0A000000u;
    default:
        return 0;
    }
}

void xd_desc(const struct sh *s, uint32_t va, uint32_t *l1e, uint32_t *l2e)
{
    uint32_t e = s->l1[va >> 20];

    *l1e = e;
    *l2e = (e & 3u) == 1u ? s->l2[((e & 0xFFFFFC00u) - s->l2_pa) / 4u + ((va >> 12) & 0xFFu)] : 0;
}

void xd_fill(uint32_t *w, const struct xd *x, const struct xd_in *in)
{
    uint32_t f = XD_F_FILLED | (in->xflags & XD_F_FROMHK);
    unsigned i;

    w[0] = XD_MAGIC;
    w[1] = x->nfwd;
    w[3] = in->kind;
    w[4] = in->pc;
    w[5] = in->vfar;
    w[6] = in->vfsr;
    w[7] = in->hb;
    w[8] = in->dfsr;
    w[9] = in->dfar;
    w[10] = in->ifsr;
    w[11] = in->ifar;
    for (i = 0; i < 16u; i++)
        w[12 + i] = xd_reg(in, i);
    w[28] = in->cpsr;
    if (xd_lost(in))
        f |= XD_F_LRLOST;
    if (xd_lost(in) || xd_bank_mode(in) == XD_M_SYS) {
        w[29] = 0;
        f |= XD_F_SPSRNA;
    } else {
        w[29] = in->bspsr;
    }
    w[30] = in->gctl;
    w[31] = in->gttb;
    w[32] = in->gdacr;
    w[33] = in->sctlr;
    w[34] = in->actlr;
    w[35] = in->dacr;
    for (i = 0; i < 4u; i++)
        w[36 + i] = in->desc[i];
    for (i = 0; i < XD_NPROBE; i++) {
        int g = (in->pok >> i) & 1u, n = (in->pok >> (8u + i)) & 1u;

        w[40 + 5 * i] = in->prb[i][0];
        w[41 + 5 * i] = in->prb[i][1];
        w[42 + 5 * i] = in->prb[i][2];
        w[43 + 5 * i] = g ? in->prb[i][3] : 0;
        w[44 + 5 * i] = n ? in->prb[i][4] : 0;
        f |= (uint32_t)g << (8u + i) | (uint32_t)n << (16u + i);
    }
    w[70] = in->l2ms;
    w[71] = in->memcfg;
    w[2] = f;
}

#endif
