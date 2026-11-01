// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_RSC_H
#define NGL_RSC_H

#include <stddef.h>
#include <stdint.h>

#include "ngl_patch.h"
#include "ngl_types.h"

#define NGL_ROM_PA          0x39000000u
#define NGL_RSC_SLOT_BYTES  64u
#define NGL_RSC_SLOT_UNITS  32u
#define NGL_RSC_MAX_WORDS   18u

struct ngl_rsc_slot {
    uint32_t len_off;
    uint32_t str_off;
};

struct ngl_rsc_table {
    const struct ngl_rsc_slot *slot;
    int n;
    uint32_t base_pa;
};

typedef int (*ngl_rd_fn)(void *ctx, uint32_t pa, void *buf, uint32_t len);

extern const struct ngl_rsc_table ngl_rsc_k3;

int ngl_rsc_entries(ngl_rd_fn rd, void *ctx, const char *path, const struct ngl_rsc_table *tab,
                    struct ngl_pair *out, size_t cap, size_t *n, char *err, size_t errcap);

#endif
