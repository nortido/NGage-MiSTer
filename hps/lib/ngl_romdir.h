// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_ROMDIR_H
#define NGL_ROMDIR_H

#include <stddef.h>
#include <stdint.h>

#define NGL_ROMDIR_MAX   1024
#define NGL_ROMDIR_POOL  (32u << 10)
#define NGL_ROMDIR_DEPTH 8
#define NGL_ROMDIR_ERR   64
#define NGL_ROMDIR_LIBS  "System/Libs"

struct ngl_romdir {
    int n;
    uint32_t used;
    uint32_t at[NGL_ROMDIR_MAX];
    uint8_t dir[NGL_ROMDIR_MAX];
    char pool[NGL_ROMDIR_POOL];
    char err[NGL_ROMDIR_ERR];
};

int ngl_romdir_list(const uint8_t *rom, size_t len, const char *path, struct ngl_romdir *out);
const char *ngl_romdir_name(const struct ngl_romdir *d, int i);
int ngl_romdir_has(const struct ngl_romdir *d, const char *name);

#endif
