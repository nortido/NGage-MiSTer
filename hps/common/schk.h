// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_SCHK_H
#define NGH_SCHK_H

#include <stdint.h>
#include "platform.h"

#define SCHK_MAGIC      0x4B484353u
#define SCHK_VER        1u
#define SCHK_MAX_REC    ((PL_SCHK_SIZE - 16u) / 16u)

#define SCHK_FNV_INIT   0x811C9DC5u
#define SCHK_FNV_PRIME  0x01000193u

#define SCHK_KIND_SHIFT 24
#define SCHK_LEN_MASK   0x00FFFFFFu

enum { SCHK_R = 1, SCHK_W = 2, SCHK_C = 3 };

struct schk_hdr {
    uint32_t magic;
    uint32_t ver;
    uint32_t n;
    uint32_t ovf;
};

struct schk_rec {
    uint32_t pos;
    uint32_t info;
    uint32_t dst;
    uint32_t sum;
};

typedef char schk_hdr_size_check[sizeof(struct schk_hdr) == 16 ? 1 : -1];
typedef char schk_rec_size_check[sizeof(struct schk_rec) == 16 ? 1 : -1];

static inline uint32_t schk_info(uint32_t kind, uint32_t len)
{
    return (kind << SCHK_KIND_SHIFT) | (len & SCHK_LEN_MASK);
}

static inline uint32_t schk_kind(uint32_t info)
{
    return info >> SCHK_KIND_SHIFT;
}

static inline uint32_t schk_len(uint32_t info)
{
    return info & SCHK_LEN_MASK;
}

static inline uint32_t schk_fnv_upd(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;

    while (n--) {
        h ^= *b++;
        h *= SCHK_FNV_PRIME;
    }
    return h;
}

static inline uint32_t schk_fnv(const void *p, uint32_t n)
{
    return schk_fnv_upd(SCHK_FNV_INIT, p, n);
}

#endif
