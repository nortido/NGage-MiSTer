// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_PATCH_H
#define NGL_PATCH_H

#include <stddef.h>
#include <stdint.h>

enum { NGL_P_HEADER = -1, NGL_P_LINE = -2, NGL_P_ORDER = -3, NGL_P_COUNT = -4, NGL_P_TOOMANY = -5, NGL_P_ROM = -6 };

// a time group on ngl_pair.off, lo then hi at off + 4; the monitor re-applies it with the elapsed time
#define NGL_PAIR_TLO 0x02000000u
#define NGL_PAIR_THI 0x04000000u

struct ngl_pair {
    uint32_t off;
    uint32_t orig;
    uint32_t repl;
};

int ngl_patch_parse(const char *t, size_t n, uint32_t *img, size_t capw, size_t *nw);
int ngl_patch_parse_rom(const char *t, size_t n, const uint8_t *rom, size_t rom_len, uint32_t *img, size_t capw, size_t *nw);
long ngl_patch_expand(const char *t, size_t n, const uint8_t *rom, size_t rom_len, uint32_t *img, size_t capw, char *out, size_t cap);
int ngl_patch_append(uint32_t *img, size_t capw, size_t *nw, const struct ngl_pair *pairs, size_t n);
long ngl_patch_text(const uint32_t *img, size_t nw, const char *md5, char *out, size_t cap);
int ngl_patch_cdrv_slot(const uint32_t *img, size_t nw);

#endif
