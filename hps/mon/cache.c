// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "cache.h"

void cache_init(struct cache *c, const struct cache_ops *op, uint32_t flags, uint32_t voff)
{
    c->op = *op;
    c->flags = flags;
    c->voff = voff;
    c->seqs = 0;
    c->l1ops = 0;
    c->l2ops = 0;
    c->syncs = 0;
}

// lines from pa & ~31 to (pa + len - 1) | 31 without computing pa + len, so a range ending at 0xFFFFFFFF works
static uint32_t lines(uint32_t pa, uint32_t len)
{
    return ((pa & 31u) + len + 31u) >> 5;
}

static void l1_range(struct cache *c, uint32_t kind, uint32_t pa, uint32_t n)
{
    uint32_t a = pa & ~31u;

    c->l1ops += n;
    for (; n; n--, a += 32u)
        c->op.l1_mva(c->op.ctx, kind, a + c->voff);
}

static void l2_range(struct cache *c, uint32_t off, uint32_t pa, uint32_t n)
{
    uint32_t a = pa & ~31u;

    c->l2ops += n;
    for (; n; n--, a += 32u)
        c->op.l2_wr(c->op.ctx, off, a);
}

static void sync(struct cache *c)
{
    c->op.sync(c->op.ctx);
    c->syncs++;
}

static int outer(const struct cache *c)
{
    return (c->flags & CACHE_OUTER) != 0;
}

static void down(struct cache *c, uint32_t l1kind, uint32_t l2off, uint32_t pa, uint32_t len)
{
    uint32_t n = lines(pa, len);

    if (!len)
        return;
    c->seqs++;
    l1_range(c, l1kind, pa, n);
    c->op.dsb(c->op.ctx);
    if (!outer(c))
        return;
    l2_range(c, l2off, pa, n);
    sync(c);
}

static int l2_only(struct cache *c, uint32_t off, uint32_t pa, uint32_t len)
{
    if (!len || !outer(c))
        return 0;
    c->seqs++;
    c->op.dsb(c->op.ctx);
    l2_range(c, off, pa, lines(pa, len));
    sync(c);
    // a PL310 SLVERR only pends while the monitor masks asynchronous aborts
    return c->op.abt ? c->op.abt(c->op.ctx) != 0 : 0;
}

void cache_out(struct cache *c, uint32_t pa, uint32_t len)
{
    down(c, CP15_ACT_CLEAN_D_MVA, L2C_CLEAN_PA, pa, len);
}

void cache_in_pre(struct cache *c, uint32_t pa, uint32_t len)
{
    down(c, CP15_ACT_CLEAN_INV_D_MVA, L2C_CLEAN_INV_PA, pa, len);
}

void cache_fresh(struct cache *c, uint32_t pa)
{
    down(c, CP15_ACT_CLEAN_INV_D_MVA, L2C_CLEAN_INV_PA, pa, 1u);
}

// TRM order for invalidation: outer first, else the L2 can refill L1 with stale data in between
void cache_in_post(struct cache *c, uint32_t pa, uint32_t len)
{
    uint32_t n = lines(pa, len);

    if (!len)
        return;
    c->seqs++;
    if (outer(c)) {
        c->op.dsb(c->op.ctx);
        l2_range(c, L2C_INV_PA, pa, n);
        sync(c);
    }
    l1_range(c, CP15_ACT_INV_D_MVA, pa, n);
    c->op.dsb(c->op.ctx);
}

int cache_sweep_inv(struct cache *c, uint32_t pa, uint32_t len)
{
    return l2_only(c, L2C_INV_PA, pa, len);
}

int cache_sweep_ci(struct cache *c, uint32_t pa, uint32_t len)
{
    return l2_only(c, L2C_CLEAN_INV_PA, pa, len);
}

int cache_sweep_clean(struct cache *c, uint32_t pa, uint32_t len)
{
    return l2_only(c, L2C_CLEAN_PA, pa, len);
}

void cache_l2_mva(struct cache *c, uint32_t kind, uint32_t pa)
{
    if (kind == CP15_ACT_CLEAN_D_MVA)
        l2_only(c, L2C_CLEAN_PA, pa, 1u);
    else if (kind == CP15_ACT_INV_D_MVA)
        l2_only(c, L2C_INV_PA, pa, 1u);
    else if (kind == CP15_ACT_CLEAN_INV_D_MVA)
        l2_only(c, L2C_CLEAN_INV_PA, pa, 1u);
}

void cache_mva(struct cache *c, uint32_t kind, uint32_t pa)
{
    if (kind == CP15_ACT_CLEAN_D_MVA)
        cache_out(c, pa, 1u);
    else if (kind == CP15_ACT_INV_D_MVA)
        cache_in_post(c, pa, 1u);
    else
        cache_fresh(c, pa);
}

void cache_drain(struct cache *c)
{
    c->op.dsb(c->op.ctx);
    if (outer(c))
        sync(c);
}

static void rd_lines(struct cache *c, const uint32_t *list, uint32_t va, uint32_t step, uint32_t n)
{
    uint32_t i, a;

    if (!n || !outer(c))
        return;
    c->seqs++;
    c->op.dsb(c->op.ctx);
    for (i = 0; i < n; i++) {
        a = (list ? list[i] : va + i * step) & ~31u;
        c->l2ops++;
        c->op.l2_wr(c->op.ctx, L2C_CLEAN_INV_PA, a - c->voff);
    }
    sync(c);
    // read side: the L1 lines go again, a speculative refill between the two levels may have read a stale L2 line
    for (i = 0; i < n; i++) {
        a = (list ? list[i] : va + i * step) & ~31u;
        c->l1ops++;
        c->op.l1_mva(c->op.ctx, CP15_ACT_CLEAN_INV_D_MVA, a);
    }
    c->op.dsb(c->op.ctx);
}

void cache_rd_va(struct cache *c, uint32_t va, uint32_t len)
{
    if (len)
        rd_lines(c, 0, va & ~31u, 32u, lines(va, len));
}

void cache_rd_step(struct cache *c, uint32_t va, uint32_t step, uint32_t n)
{
    rd_lines(c, 0, va, step, n);
}

void cache_rd_list(struct cache *c, const uint32_t *va, uint32_t n)
{
    rd_lines(c, va, 0, 0, n);
}
