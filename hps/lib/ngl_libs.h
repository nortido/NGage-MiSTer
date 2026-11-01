// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_LIBS_H
#define NGL_LIBS_H

#include <stddef.h>
#include <stdint.h>

#include "ngl_game.h"
#include "ngl_romdir.h"

#define NGL_LIBS_MAX      32
#define NGL_LIBS_READ_MAX (16u << 20)
#define NGL_LIBS_WHY      96
#define NGL_LIBS_LINE     (NGL_LIBS_MAX * 66 + 64)

struct ngl_libs {
    struct ngl_game_src src;
    struct ngl_game_src base;
    struct ngl_game_src lib;
    const struct ngl_romdir *rom;
    int nbase;
    int n;
    int from[NGL_LIBS_MAX];
    uint32_t ord[NGL_LIBS_MAX];
    uint32_t exports[NGL_LIBS_MAX];
    struct ngl_entry extra[NGL_LIBS_MAX];
    char why[NGL_LIBS_WHY];
    char rom_why[NGL_LIBS_WHY];
};

int ngl_libs_wrap(const struct ngl_game_src *game, const struct ngl_game_src *libs, const struct ngl_romdir *rom,
                  const struct ngl_sel *sel, struct ngl_libs *out);
void ngl_libs_line(const struct ngl_libs *l, char *buf, size_t cap);

#endif
