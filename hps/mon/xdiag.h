// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_XDIAG_H
#define NGH_XDIAG_H

#ifndef NGH_XDIAG
#define NGH_XDIAG 0
#endif

#if NGH_XDIAG

#include <stdint.h>
#include "shadow.h"
#include "../common/platform.h"
#include "../common/status.h"

#define XD_PA       0x38200C00u
#define XD_NW       72u
#define XD_NPROBE   6u
#define XD_MAGIC    0x31474458u

_Static_assert(sizeof(struct ngst) <= XD_PA - PL_STATUS_BASE, "record above the status struct");
_Static_assert(XD_PA + XD_NW * 4u <= PL_C1RST_FLAG, "record below the reset flag");

struct xd {
    uint32_t nfwd;
    uint32_t taken;
};

struct xd_in {
    uint32_t kind, pc, vfar, vfsr, hb;
    uint32_t dfsr, dfar, ifsr, ifar;
    uint32_t fr[13];
    uint32_t fiq[5];
    uint32_t bsp, blr, bspsr, ssp;
    uint32_t gm, emode, cpsr;
    uint32_t gctl, gttb, gdacr;
    uint32_t sctlr, actlr, dacr;
    uint32_t desc[4];
    uint32_t prb[XD_NPROBE][5];
    uint32_t pok, xflags, l2ms, memcfg;
};

int xd_note(struct xd *x);
uint32_t xd_reg(const struct xd_in *in, unsigned n);
uint32_t xd_probe_va(const struct xd_in *in, unsigned i);
void xd_desc(const struct sh *s, uint32_t va, uint32_t *l1e, uint32_t *l2e);
void xd_fill(uint32_t *w, const struct xd *x, const struct xd_in *in);

#endif
#endif
