// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_HW_H
#define NGL_HW_H

#include <stddef.h>
#include <stdint.h>

#include "ngl_types.h"

struct ngl_dry_word {
    uint32_t pa;
    uint32_t val;
};

struct ngl_dry_file {
    char path[64];
    char val[24];
};

struct ngl_dry {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
    struct ngl_dry_word w[64];
    int nw;
    struct ngl_dry_file f[8];
    int nf;
};

void ngl_dry_init(struct ngl_dry *d, struct ngl_io *io, char *buf, size_t cap);
void ngl_dry_set(struct ngl_dry *d, uint32_t pa, uint32_t val);
uint32_t ngl_dry_get(const struct ngl_dry *d, uint32_t pa);
int ngl_dry_file(struct ngl_dry *d, const char *path, const char *val);

#ifdef __linux__
struct ngl_hw {
    int fd;
    uint32_t page;
    volatile uint32_t *map;
};

int ngl_hw_open(struct ngl_hw *h, struct ngl_io *io);
void ngl_hw_close(struct ngl_hw *h);
#endif

#endif
