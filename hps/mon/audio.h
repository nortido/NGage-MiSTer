// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_AUDIO_H
#define NGH_AUDIO_H

#include <stdint.h>
#include "../common/hyb_regs.h"

#define AUD_MAX_CHUNK   0x10000u
#define AUD_PAGE        0x1000u
#define AUD_NPAGE       (AUD_MAX_CHUNK / AUD_PAGE + 1u)
#define AUD_SLACK       8u
#define AUD_RATE_MIN    8000u
#define AUD_RATE_OUT    48000u
#define AUD_PRIME_N     2u
#define AUD_BAND_LO     2u
#define AUD_BAND_HI     6u
#define AUD_TRIM_DIV    2000u
#define AUD_TRIM_CAP    100u
#define AUD_TRIM_WAIT   64u

enum { AUD_OK = 0, AUD_OFF = 1, AUD_EMPTY = 2, AUD_SKIP = 3, AUD_DROP = 4 };

struct aud_io {
    void *ctx;
    int (*va2pa)(void *ctx, uint32_t va, uint32_t *pa);
    void (*copy)(void *ctx, uint32_t off, uint32_t pa, uint32_t n);
    void (*publish)(void *ctx);
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t v);
};

struct aud {
    uint32_t on;
    uint32_t wr;
    uint32_t rate;
    uint32_t ch;
    uint32_t ctl;
    uint32_t nom;
    uint32_t step;
    uint32_t since;
    int32_t trim;
    int32_t mdir;
    uint32_t mark;
    uint32_t wait;
    uint32_t bytes;
    uint32_t chunks;
    uint32_t drops;
    uint32_t skips;
    uint32_t cfg;
};

uint32_t aud_step(uint32_t rate);
void aud_args(uint32_t (*get)(void *ctx, unsigned mode, unsigned n), void *ctx, unsigned mode, uint32_t r[4]);
void aud_start(struct aud *a, const struct aud_io *io, uint32_t version);
void aud_park(const struct aud *a, const struct aud_io *io);
int aud_chunk(struct aud *a, const struct aud_io *io, const uint32_t r[4]);

#endif
