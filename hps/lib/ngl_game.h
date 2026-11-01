// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_GAME_H
#define NGL_GAME_H

#include <stdint.h>

#include "ngl_types.h"

#define NGL_GAME_NAME   96
#define NGL_GAME_ERR    256
#define NGL_ALIAS_MAX   5
#define NGL_ALIAS_EXTRA 256

struct ngl_game_src {
    const struct ngl_src_ops *ops;
    void *ctx;
};

struct ngl_sel {
    char dir[NGL_GAME_NAME];
    char app[NGL_GAME_NAME];
    char path[NGL_SLOT_CHARS + 1];
    char alias[NGL_ALIAS_MAX + 1];
    char err[NGL_GAME_ERR];
};

struct ngl_alias {
    struct ngl_game_src src;
    struct ngl_game_src base;
    int nbase;
    int n;
    int from[NGL_ALIAS_EXTRA];
    struct ngl_entry extra[NGL_ALIAS_EXTRA];
};

struct ngl_sorted {
    struct ngl_game_src src;
    struct ngl_game_src in;
    int n;
    int *idx;
};

int ngl_game_select(const struct ngl_game_src *src, struct ngl_sel *sel);
int ngl_alias_wrap(const struct ngl_game_src *base, const struct ngl_sel *sel, struct ngl_alias *out);
int ngl_sorted_view(const struct ngl_game_src *in, struct ngl_sorted *out);
void ngl_sorted_free(struct ngl_sorted *s);

#endif
