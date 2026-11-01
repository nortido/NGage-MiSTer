// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_CACHE_H
#define NGH_CACHE_H

#include <stdint.h>
#include "cp15.h"

#define CACHE_OUTER 1u

#define L2C_CACHE_SYNC   0x730u
#define L2C_INV_PA       0x770u
#define L2C_CLEAN_PA     0x7B0u
#define L2C_CLEAN_INV_PA 0x7F0u

struct cache_ops {
    void *ctx;
    void (*l1_mva)(void *ctx, uint32_t kind, uint32_t va);
    void (*l2_wr)(void *ctx, uint32_t off, uint32_t pa);
    void (*dsb)(void *ctx);
    void (*sync)(void *ctx);
    uint32_t (*abt)(void *ctx);
};

struct cache {
    struct cache_ops op;
    uint32_t flags;
    uint32_t voff;
    uint32_t seqs;
    uint32_t l1ops;
    uint32_t l2ops;
    uint32_t syncs;
};

void cache_init(struct cache *c, const struct cache_ops *op, uint32_t flags, uint32_t voff);
void cache_out(struct cache *c, uint32_t pa, uint32_t len);
void cache_in_pre(struct cache *c, uint32_t pa, uint32_t len);
void cache_in_post(struct cache *c, uint32_t pa, uint32_t len);
void cache_fresh(struct cache *c, uint32_t pa);
int cache_sweep_inv(struct cache *c, uint32_t pa, uint32_t len);
int cache_sweep_ci(struct cache *c, uint32_t pa, uint32_t len);
int cache_sweep_clean(struct cache *c, uint32_t pa, uint32_t len);
void cache_l2_mva(struct cache *c, uint32_t kind, uint32_t pa);
void cache_mva(struct cache *c, uint32_t kind, uint32_t pa);
void cache_drain(struct cache *c);
void cache_rd_va(struct cache *c, uint32_t va, uint32_t len);
void cache_rd_step(struct cache *c, uint32_t va, uint32_t step, uint32_t n);
void cache_rd_list(struct cache *c, const uint32_t *va, uint32_t n);

#endif
