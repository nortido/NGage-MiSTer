// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_XCAP_H
#define NGL_XCAP_H

#include <stdint.h>

#include "../common/xcap.h"
#include "ngl_types.h"

#define NGL_XCAP_LINES 5
#define NGL_XCAP_LINE  160
// a self Kill, Terminate or Panic of the game process at most 60 s (XC_TIM_MS units) after its newest
// card-identity read
#define NGL_XCAP_CID_WIN 600u

struct ngl_xcap {
    uint32_t have;
    uint32_t row[XC_NROW];
    // have bit XC_NROW + r marks cid[r]
    uint32_t cid[XC_NCROW];
    // have bit XC_NROW + XC_NCROW marks larm
    uint32_t larm;
    uint32_t gproc;
};

int ngl_xcap_read(const struct ngl_io *io, struct ngl_xcap *out);
int ngl_xcap_fmt(const struct ngl_xcap *in, char (*line)[NGL_XCAP_LINE], int n);
int ngl_xcap_cid(const struct ngl_xcap *x);
int ngl_xcap_preframe(const struct ngl_xcap *x);

#endif
