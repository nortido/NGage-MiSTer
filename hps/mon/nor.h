// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_NOR_H
#define NGH_NOR_H

#include <stdint.h>

#define NOR_PA_BASE    0x02400000u
#define NOR_SIZE       0x00400000u
#define NOR_BUSY_READS 2u

#define NOR_ST_BUSY 0x0008u
#define NOR_ST_SUSP 0x0080u

struct nor {
    uint8_t st;
    uint8_t susp;
    uint8_t busy;
    uint32_t esec;
    uint32_t elen;
    uint32_t program;
    uint32_t erase;
    uint32_t stray;
};

struct nor_fx {
    uint32_t lo;
    uint32_t hi;
    uint32_t program;
    uint32_t erase;
    uint32_t stray;
};

static inline int nor_pa_in(uint32_t pa)
{
    return pa - NOR_PA_BASE < NOR_SIZE;
}

void nor_init(struct nor *n);
void nor_reset(struct nor *n);
int nor_sector(uint32_t off, uint32_t *base, uint32_t *len);
int nor_access(struct nor *n, uint16_t *mem, uint32_t off, unsigned size, int write, uint32_t *data,
               struct nor_fx *fx);
uint32_t nor_gen_next(uint32_t gen, int after, int changed);
int nor_route_on(uint32_t cdrv, uint32_t version);

#endif
