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
#define AUD_PRIME_MAX   5u
#define AUD_GAP         200000000u
#define AUD_REL_MS      150u
#define AUD_LOW_MS      70u
#define AUD_BAND_LO     2u
#define AUD_BAND_HI     6u
#define AUD_TRIM_DIV    2000u
#define AUD_TRIM_CAP    100u
#define AUD_TRIM_WAIT   64u

#ifndef NGH_AUD_CAP
#define NGH_AUD_CAP 0
#endif

#if NGH_AUD_CAP
#define AUD_CAP_BASE    0x3C200000u
#define AUD_CAP_SIZE    0x00800000u
#define AUD_CAP_MAGIC   0x50414341u
#define AUD_CAP_VER     1u
#define AUD_CAP_IDX     0x00001000u
#define AUD_CAP_ENT     44u
#define AUD_CAP_NENT    ((AUD_CAP_DATA - AUD_CAP_IDX) / AUD_CAP_ENT)
#define AUD_CAP_DATA    0x00100000u
#define AUD_CAP_DMAX    (AUD_CAP_SIZE - AUD_CAP_DATA)

enum { AUDC_MAGIC, AUDC_VER, AUDC_IDX, AUDC_ENT, AUDC_NENT, AUDC_DATA, AUDC_DMAX, AUDC_COUNT, AUDC_WR, AUDC_LOST, AUDC_HDR_N };
enum { AUDE_SEQ, AUDE_OFF, AUDE_N, AUDE_RATE, AUDE_CH, AUDE_T, AUDE_USED, AUDE_UNDER, AUDE_VA, AUDE_DROPS, AUDE_SKIPS, AUDE_N_WORDS };
#endif

enum { AUD_OK = 0, AUD_OFF = 1, AUD_EMPTY = 2, AUD_SKIP = 3, AUD_DROP = 4 };

struct aud_io {
    void *ctx;
    int (*va2pa)(void *ctx, uint32_t va, uint32_t *pa);
    void (*copy)(void *ctx, uint32_t off, uint32_t pa, uint32_t n);
    void (*publish)(void *ctx);
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t v);
    uint32_t (*now)(void *ctx);
#if NGH_AUD_CAP
    void (*cap_data)(void *ctx, uint32_t off, uint32_t pa, uint32_t n);
    void (*cap_word)(void *ctx, uint32_t off, uint32_t v);
#endif
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
    uint32_t under;
    uint32_t last;
    uint32_t shallow;
    uint32_t deep;
    uint32_t bpms;
    int32_t lvl;
    int32_t low;
    int32_t end;
#if NGH_AUD_CAP
    uint32_t cap_n;
    uint32_t cap_wr;
    uint32_t cap_lost;
    uint32_t cap_full;
#endif
};

uint32_t aud_step(uint32_t rate);
void aud_args(uint32_t (*get)(void *ctx, unsigned mode, unsigned n), void *ctx, unsigned mode, uint32_t r[4]);
void aud_start(struct aud *a, const struct aud_io *io, uint32_t version);
void aud_park(const struct aud *a, const struct aud_io *io);
int aud_chunk(struct aud *a, const struct aud_io *io, const uint32_t r[4]);
uint32_t aud_short(struct aud *a);

#endif
