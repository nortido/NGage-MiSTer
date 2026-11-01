// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_KTIME_H
#define NGL_KTIME_H

#include <stddef.h>
#include <stdint.h>

#include "ngl_rsc.h"

#define NGL_KTIME_PAIRS 4u
#define NGL_KTIME_MSG   96

struct ngl_civil {
    int year, mon, day, hour, min, sec;
};

uint64_t ngl_ttime(const struct ngl_civil *c);
int ngl_civil_ok(const struct ngl_civil *c);
int ngl_civil_local(int64_t t, struct ngl_civil *c);
size_t ngl_ktime_build(ngl_rd_fn rd, void *ctx, uint32_t rom_size, const uint32_t *img, size_t nw,
                       const struct ngl_civil *now, struct ngl_pair *out, char *msg, size_t cap, int tkind);

#endif
