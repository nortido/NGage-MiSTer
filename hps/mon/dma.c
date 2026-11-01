// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "../common/hyb_regs.h"
#include "cache.h"
#include "pamap.h"
#ifdef NGH_SECCHK
#include "../common/schk.h"
#endif

// ngage_mmc.sv DMA channels 0..6: +08 control (2 starts), +14 source, +20 destination, +2C count
#define DMA_REG_LO      0x0C120300u
#define DMA_STRIDE      0x40u
#define DMA_NCH         7u
#define DMA_OFF_CTRL    0x08u
#define DMA_OFF_SRC     0x14u
#define DMA_OFF_DST     0x20u
#define DMA_OFF_CNT     0x2Cu
#define DMA_START       2u
#define DMA_PORT_CARD   0x10120624u
#define DMA_PORT_LCD    0x10121208u
#define INTC_LO         0x0C120100u
#define INTC_HI         0x0C120280u
// ngage_mmc.sv command registers: +04 go (bit 0), +1C index, +20 argument
#define MMC_LO          0x0C120600u
#define MMC_OFF_GO      0x04u
#define MMC_OFF_IDX     0x1Cu
#define MMC_OFF_ARG     0x20u
#define MMC_SECTOR      512u
// PERIPHCLK 200 MHz global timer
#define GT_PER_US       200u
#define DMA_US_10MS     10000u
#define DMA_US_50MS     50000u
// a range never walks more than the 16 MB of guest RAM, whatever count the guest wrote
#define DMA_RANGE_MAX   0x1000000u

enum { DW_NONE = 0, DW_KICK = 1, DW_CANCEL = 2 };
enum { DK_OTHER = 0, DK_IN = 1, DK_OUT = 2, DK_LCD = 3 };
enum { DM_RANGE = 0, DM_ALL = 1 };

struct dma_snap {
    uint32_t kind;
    uint32_t pa;
    uint32_t len;
    uint32_t pos;
    uint32_t t;
};

#ifdef NGH_SECCHK
struct dma_piece {
    uint32_t pos;
    uint32_t pa;
    uint32_t n;
    uint32_t pad;
};
#endif

struct dma {
    uint32_t src_card;
    uint32_t dst_lcd;
    uint32_t out;
    uint32_t src[DMA_NCH];
    uint32_t cnt[DMA_NCH];
    uint32_t kicks;
    uint32_t card_kicks;
    uint32_t lcd_kicks;
    uint32_t card_bytes;
    uint32_t flushes;
    uint32_t lcd_src;
    uint32_t lcd_words;
    uint32_t dst_card;
    uint32_t dst[DMA_NCH];
    uint32_t wout;
    uint32_t lout;
    uint32_t last;
    struct dma_snap snap[DMA_NCH];
    uint32_t pos;
    uint32_t mmc_idx;
    uint32_t mmc_arg;
    int mmc_iw;
    uint32_t cflush;
    uint32_t cpa[DMA_NCH];
    uint32_t clen[DMA_NCH];
    uint32_t cqlc;
    uint32_t ccan;
    uint32_t clmx;
    uint32_t cl10;
    uint32_t cl50;
    uint32_t ldmx;
    uint32_t ldnc;
    uint32_t dcpr;
    uint32_t dmin;
    uint32_t dmot;
    uint32_t dmuk;
    uint32_t dmoc;
    // finished card-in channels that wait for their completion post
    uint32_t done_in;
    // host pages of the last card-in kick whose head or tail line the range covers in part
    uint32_t edge[2];
    uint32_t nedge;
    // memcfg has a write-back bit, set by guest_boot after dma_init
    uint32_t wb;
};

void dma_init(struct dma *d)
{
    uint32_t k;

    d->src_card = 0;
    d->dst_lcd = 0;
    d->out = 0;
    for (k = 0; k < DMA_NCH; k++) {
        d->src[k] = 0;
        d->cnt[k] = 0;
        d->dst[k] = 0;
        d->snap[k].kind = DK_OTHER;
        d->snap[k].pa = 0;
        d->snap[k].len = 0;
        d->snap[k].pos = 0;
        d->snap[k].t = 0;
        d->cpa[k] = 0;
        d->clen[k] = 0;
    }
    d->kicks = 0;
    d->card_kicks = 0;
    d->lcd_kicks = 0;
    d->card_bytes = 0;
    d->flushes = 0;
    d->lcd_src = 0;
    d->lcd_words = 0;
    d->dst_card = 0;
    d->wout = 0;
    d->lout = 0;
    d->last = 0;
    d->pos = 0;
    d->mmc_idx = 0;
    d->mmc_arg = 0;
    d->mmc_iw = 0;
    d->cflush = 0;
    d->cqlc = 0;
    d->ccan = 0;
    d->clmx = 0;
    d->cl10 = 0;
    d->cl50 = 0;
    d->ldmx = 0;
    d->ldnc = 0;
    d->dcpr = 0;
    d->dmin = 0;
    d->dmot = 0;
    d->dmuk = 0;
    d->dmoc = 0;
    d->done_in = 0;
    d->nedge = 0;
    d->wb = 0;
}

static void dma_bit(uint32_t *m, uint32_t bit, int on)
{
    if (on)
        *m |= bit;
    else
        *m &= ~bit;
}

static uint32_t dma_nbits(uint32_t m)
{
    uint32_t n = 0;

    for (; m; m &= m - 1u)
        n++;
    return n;
}

// the guest's stream position as ngage_mmc.sv keeps it in st_pos (lba and byte offset)
static void mmc_write(struct dma *d, uint32_t off, uint32_t v)
{
    switch (off) {
    case MMC_OFF_IDX:
        d->mmc_idx = v & 0x3Fu;
        d->mmc_iw = 1;
        break;
    case MMC_OFF_ARG:
        d->mmc_arg = v;
        break;
    case MMC_OFF_GO:
        if (v & 1u) {
            if (d->mmc_iw && (d->mmc_idx == 17u || d->mmc_idx == 18u || d->mmc_idx == 24u || d->mmc_idx == 25u))
                d->pos = d->mmc_arg & ~(MMC_SECTOR - 1u);
            d->mmc_iw = 0;
        } else if (v == 0 || v == 2u) {
            d->pos &= ~(MMC_SECTOR - 1u);
        }
        break;
    default:
        break;
    }
}

static void dma_kick(struct dma *d, uint32_t k, uint32_t bit)
{
    struct dma_snap *s = &d->snap[k];

    s->kind = DK_OTHER;
    s->pa = 0;
    s->len = 0;
    s->pos = d->pos;
    d->wout &= ~bit;
    d->lout &= ~bit;
    if ((d->src_card | d->dst_card) & bit) {
        s->len = (d->cnt[k] + 1u) & ~1u;
        // ngage_mmc.sv go_len: a read moves the stream by its bytes, a write by whole sectors
        d->pos += d->src_card & bit ? s->len : ((s->len >> 1) + 255u) >> 8 << 9;
        if (d->lout)
            d->cqlc++;
    }
    if (d->src_card & bit) {
        s->kind = DK_IN;
        s->pa = d->dst[k];
        d->out |= bit;
        d->card_kicks++;
        d->card_bytes += d->cnt[k] + 1u;
    } else if (d->dst_card & bit) {
        s->kind = DK_OUT;
        s->pa = d->src[k];
        d->wout |= bit;
    } else if (d->dst_lcd & bit) {
        s->kind = DK_LCD;
        s->pa = d->src[k];
        s->len = (d->cnt[k] + 1u) * 4u;
        d->lout |= bit;
        d->lcd_kicks++;
        d->lcd_src = d->src[k];
        d->lcd_words = d->cnt[k] + 1u;
    }
}

// v is the forwarded write data after the size mask, as the soc sees it
int dma_write(struct dma *d, uint32_t pa, uint32_t v)
{
    uint32_t k, off, bit;

    if (pa - MMC_LO < DMA_STRIDE) {
        mmc_write(d, pa - MMC_LO, v);
        return DW_NONE;
    }
    if (pa - DMA_REG_LO >= DMA_NCH * DMA_STRIDE)
        return DW_NONE;
    k = (pa - DMA_REG_LO) / DMA_STRIDE;
    off = (pa - DMA_REG_LO) % DMA_STRIDE;
    bit = 1u << k;
    switch (off) {
    case DMA_OFF_SRC:
        d->src[k] = v;
        dma_bit(&d->src_card, bit, v == DMA_PORT_CARD);
        return DW_NONE;
    case DMA_OFF_DST:
        d->dst[k] = v;
        dma_bit(&d->dst_card, bit, v == DMA_PORT_CARD);
        dma_bit(&d->dst_lcd, bit, v == DMA_PORT_LCD);
        return DW_NONE;
    case DMA_OFF_CNT:
        d->cnt[k] = v;
        return DW_NONE;
    case DMA_OFF_CTRL:
        // ngage_mmc.sv: 0 drops a queued go; the pre-read already took a finished one
        if (v == 0 && ((d->out | d->wout) & bit)) {
            // a card-out source needs nothing after the transfer, a card-in range gets its post at the next tick
            if (d->out & bit) {
                d->cpa[k] = d->snap[k].pa;
                d->clen[k] = d->snap[k].len;
                d->cflush |= bit;
            }
            d->out &= ~bit;
            d->wout &= ~bit;
            d->ccan++;
            d->last = k;
            return DW_CANCEL;
        }
        if (v != DMA_START)
            return DW_NONE;
        d->kicks++;
        d->last = k;
        dma_kick(d, k, bit);
        return DW_KICK;
    default:
        return DW_NONE;
    }
}

// the finished channels; card-in ones need the completion maintenence, LCD and card-out ones are only timed
uint32_t dma_done(struct dma *d, uint32_t status)
{
    uint32_t pend = (status & HYB_ST_DMA_PEND_MASK) >> HYB_ST_DMA_PEND_SHIFT, in = d->out & pend,
             lcd = d->lout & pend, fin = (d->out | d->wout | d->lout) & pend;

    if (!fin)
        return 0;
    d->out &= ~fin;
    d->wout &= ~fin;
    d->lout &= ~fin;
    d->done_in |= in;
    if (in)
        d->flushes++;
    d->ldnc += dma_nbits(lcd);
    return fin;
}

// any +08 write clears dma_pend in ngage_mmc.sv, so an outstanding channel's STATUS bit is read first
uint32_t dma_pre(const struct dma *d, uint32_t pa)
{
    uint32_t k;

    if (pa - DMA_REG_LO >= DMA_NCH * DMA_STRIDE || (pa - DMA_REG_LO) % DMA_STRIDE != DMA_OFF_CTRL)
        return 0;
    k = (pa - DMA_REG_LO) / DMA_STRIDE;
    return (d->out | d->wout | d->lout) & (1u << k);
}

#ifdef NGH_SECCHK
// pieces of a card snapshot: a read splits [pos, pos + len) at sector boundaries; a write covers the whole
// sectors ngage_mmc.sv sends from the sector of pos, past len zero padded (w_zero)
int dma_piece(const struct dma_snap *s, uint32_t off, struct dma_piece *p)
{
    uint32_t base;

    if (off >= s->len)
        return 0;
    p->pa = s->pa + off;
    if (s->kind == DK_OUT) {
        base = s->pos & ~(MMC_SECTOR - 1u);
        p->pos = base + off;
        p->n = s->len - off < MMC_SECTOR ? s->len - off : MMC_SECTOR;
        p->pad = MMC_SECTOR - p->n;
        return 1;
    }
    p->pos = s->pos + off;
    p->n = MMC_SECTOR - (p->pos & (MMC_SECTOR - 1u));
    if (p->n > s->len - off)
        p->n = s->len - off;
    p->pad = 0;
    return 1;
}

uint32_t dma_fnv_pad(uint32_t h, uint32_t n)
{
    for (; n; n--)
        h *= SCHK_FNV_PRIME;
    return h;
}
#endif

// out of line so a test sees the tick take it
__attribute__((noinline)) uint32_t dma_cflush(struct dma *d)
{
    uint32_t m = d->cflush;

    if (!m)
        return 0;
    d->cflush = 0;
    d->flushes++;
    return m;
}

// guest [pa, pa + len) page by page; pages that are not RAM are skipped (the FPGA drops them) and a host run
// ends where the next page is not its continuaton
uint32_t dma_range(struct cache *c, const struct pam_ctx *pam, void (*op)(struct cache *, uint32_t, uint32_t),
                   uint32_t pa, uint32_t len)
{
    uint32_t host = 0, n = 0, runs = 0, step;
    struct pam_res r;

    if (len > DMA_RANGE_MAX)
        len = DMA_RANGE_MAX;
    for (; len; pa += step, len -= step) {
        step = 0x1000u - (pa & 0xFFFu);
        if (step > len)
            step = len;
        r = pamap_page(pa, 0, pam);
        if (n && (r.kind != PAM_RAM || r.host_pa != host + n)) {
            op(c, host, n);
            runs++;
            n = 0;
        }
        if (r.kind != PAM_RAM)
            continue;
        if (!n)
            host = r.host_pa;
        n += step;
    }
    if (n) {
        op(c, host, n);
        runs++;
    }
    return runs;
}

// a head or tail line the range covers in part also holds guest bytes; its host page joins the guard set
static void dma_edges(struct dma *d, const struct pam_ctx *pam, uint32_t pa, uint32_t len)
{
    uint32_t at[2] = { pa, pa + len - 1u }, cut[2] = { pa & 31u, (pa + len) & 31u }, i, h;
    struct pam_res r;

    for (i = 0; i < 2u && len; i++) {
        if (!cut[i])
            continue;
        r = pamap_page(at[i], 0, pam);
        h = r.host_pa & ~0xFFFu;
        if (r.kind == PAM_RAM && (!d->nedge || d->edge[0] != h))
            d->edge[d->nedge++] = h;
    }
}

static void dma_cone(struct dma *d, struct cache *c, const struct pam_ctx *pam, uint32_t k)
{
    // the guest may have stored into the buffer after the cancel, through a write-back view
    d->dmin += dma_range(c, pam, d->wb ? cache_in_pre : cache_in_post, d->cpa[k], d->clen[k]);
}

// kick maintenance; DM_ALL leaves the whole-L1 flush to the caller
__attribute__((noinline)) int dma_kick_mt(struct dma *d, struct cache *c, const struct pam_ctx *pam)
{
    uint32_t k = d->last, bit = 1u << k;
    const struct dma_snap *s = &d->snap[k];

    // a cancelled range of this channel still waits for the tick: it goes now, so the next cancel finds its slot free
    if (d->cflush & bit) {
        d->cflush &= ~bit;
        d->flushes++;
        dma_cone(d, c, pam, k);
    }
    d->nedge = 0;
    switch (s->kind) {
    case DK_IN:
        dma_edges(d, pam, s->pa, s->len);
        d->dmin += dma_range(c, pam, cache_in_pre, s->pa, s->len);
        return DM_RANGE;
    case DK_OUT:
        d->dmot += dma_range(c, pam, cache_out, s->pa, s->len);
        return DM_RANGE;
    case DK_LCD:
        d->dmot += dma_range(c, pam, cache_out, d->lcd_src, d->lcd_words * 4u);
        return DM_RANGE;
    default:
        d->dmuk++;
        return DM_ALL;
    }
}

// completion maintenance of the card-in channels dma_done found finished
__attribute__((noinline)) void dma_in_post(struct dma *d, struct cache *c, const struct pam_ctx *pam)
{
    uint32_t k, m = d->done_in;

    d->done_in = 0;
    for (k = 0; k < DMA_NCH; k++)
        if ((m & (1u << k)) && d->snap[k].kind == DK_IN)
            d->dmin += dma_range(c, pam, cache_in_post, d->snap[k].pa, d->snap[k].len);
}

__attribute__((noinline)) void dma_cpost(struct dma *d, struct cache *c, const struct pam_ctx *pam, uint32_t chans)
{
    uint32_t k;

    for (k = 0; k < DMA_NCH; k++)
        if (chans & (1u << k))
            dma_cone(d, c, pam, k);
}


void dma_stamp(struct dma *d, uint32_t now)
{
    d->snap[d->last].t = now;
}

void dma_fin(struct dma *d, uint32_t fin, uint32_t now)
{
    uint32_t k, us;

    for (k = 0; k < DMA_NCH; k++) {
        if (!(fin & (1u << k)))
            continue;
        us = (now - d->snap[k].t) / GT_PER_US;
        if (d->snap[k].kind == DK_LCD) {
            if (us > d->ldmx)
                d->ldmx = us;
            continue;
        }
        if (us > d->clmx)
            d->clmx = us;
        d->cl10 += us > DMA_US_10MS;
        d->cl50 += us > DMA_US_50MS;
    }
}

int dma_intc_read(uint32_t pa)
{
    return pa - INTC_LO < INTC_HI - INTC_LO;
}

#define GF_NSAMP        256u
#define GF_NHIST        64u
#ifndef NGH_GF_FULL
#define NGH_GF_FULL     0
#endif
#ifndef NGH_GF_DIAG
#define NGH_GF_DIAG     0
#endif

struct gf {
    uint32_t sig;
    int have;
    uint32_t frames;
    uint32_t sec;
    uint32_t last;
    uint32_t secs;
    uint8_t hist[GF_NHIST];
#if NGH_GF_DIAG
    uint32_t asig;
    int ahave;
    uint32_t an;
    uint32_t cc;
#endif
};

void gf_init(struct gf *g)
{
    uint32_t i;

    g->sig = 0;
    g->have = 0;
    g->frames = 0;
    g->sec = 0;
    g->last = 0;
    g->secs = 0;
    for (i = 0; i < GF_NHIST; i++)
        g->hist[i] = 0;
#if NGH_GF_DIAG
    g->asig = 0;
    g->ahave = 0;
    g->an = 0;
    g->cc = 0;
#endif
}

uint32_t gf_step(uint32_t words)
{
    return words < GF_NSAMP ? 1u : words >> 8;
}

uint32_t gf_sig(const volatile uint32_t *p, uint32_t words)
{
    uint32_t h = 2166136261u, n = words < GF_NSAMP ? words : GF_NSAMP, step = gf_step(words), i;

    for (i = 0; i < n; i++)
        h = (h ^ p[i * step]) * 16777619u;
    return h;
}

#define LIM_PIX_MASK    0x0FFF0FFFu

uint32_t lim_nonwhite(const volatile uint32_t *p, uint32_t words)
{
    // two 12-bit 4:4:4 pixels per word in [11:0] and [27:16], white is all ones in both
    uint32_t n = words < GF_NSAMP ? words : GF_NSAMP, step = gf_step(words), c = 0, i;

    for (i = 0; i < n; i++)
        if ((p[i * step] & LIM_PIX_MASK) != LIM_PIX_MASK)
            c++;
    return c;
}

int gf_lcd(struct gf *g, uint32_t sig)
{
    int nf = g->have && sig != g->sig;

    g->have = 1;
    g->sig = sig;
    if (nf) {
        g->frames++;
        g->sec++;
    }
    return nf;
}

void gf_second(struct gf *g)
{
    g->last = g->sec;
    g->hist[g->secs % GF_NHIST] = (uint8_t)(g->sec > 255u ? 255u : g->sec);
    g->secs++;
    g->sec = 0;
}

uint32_t gf_row(const struct gf *g, uint32_t r)
{
    const uint8_t *b = &g->hist[(r * 4u) % GF_NHIST];

    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

#if NGH_GF_FULL || NGH_GF_DIAG
// 8 lines ahead, so the DDR reads of the next lines overlap the hashing of this one
#define GF_PF_WORDS     64u

static inline uint32_t gf_fnv(uint32_t h, uint32_t w)
{
    return (h ^ w) * 16777619u;
}

__attribute__((noinline)) uint32_t gf_sigf(const volatile uint32_t *p, uint32_t words)
{
    uint32_t h0 = 2166136261u ^ words, h1 = h0 ^ 1u, h2 = h0 ^ 2u, h3 = h0 ^ 3u, i;

    for (i = 0; i + 8u <= words; i += 8u) {
        // the clamp keeps the prefetch inside the frame: gcc drops a plain if around a prefetch
        __builtin_prefetch((const void *)&p[i + GF_PF_WORDS < words ? i + GF_PF_WORDS : words - 1u]);
        h0 = gf_fnv(h0, p[i]);
        h1 = gf_fnv(h1, p[i + 1u]);
        h2 = gf_fnv(h2, p[i + 2u]);
        h3 = gf_fnv(h3, p[i + 3u]);
        h0 = gf_fnv(h0, p[i + 4u]);
        h1 = gf_fnv(h1, p[i + 5u]);
        h2 = gf_fnv(h2, p[i + 6u]);
        h3 = gf_fnv(h3, p[i + 7u]);
    }
    if (i + 4u <= words) {
        h0 = gf_fnv(h0, p[i]);
        h1 = gf_fnv(h1, p[i + 1u]);
        h2 = gf_fnv(h2, p[i + 2u]);
        h3 = gf_fnv(h3, p[i + 3u]);
        i += 4u;
    }
    if (i < words)
        h0 = gf_fnv(h0, p[i]);
    if (i + 1u < words)
        h1 = gf_fnv(h1, p[i + 1u]);
    if (i + 2u < words)
        h2 = gf_fnv(h2, p[i + 2u]);
    return gf_fnv(gf_fnv(gf_fnv(gf_fnv(2166136261u, h0), h1), h2), h3);
}

// guest stores may bypass the cacheable alias, so every line of the frame is dropped before the read
__attribute__((noinline)) void gf_fresh(struct cache *c, uint32_t va, uint32_t bytes)
{
    uint32_t a = va & ~31u, n = ((va & 31u) + bytes + 31u) >> 5;

    if (!bytes)
        return;
    for (; n; n--, a += 32u)
        c->op.l1_mva(c->op.ctx, CP15_ACT_CLEAN_INV_D_MVA, a);
    c->op.dsb(c->op.ctx);
    if (c->flags & CACHE_OUTER)
        cache_rd_va(c, va, bytes);
}
#endif

#if NGH_GF_DIAG
uint32_t gf_alt(struct gf *g, uint32_t sig, int pnew, uint32_t cyc)
{
    uint32_t an = g->ahave && sig != g->asig, pn = pnew != 0, k = cyc >> 6;

    g->ahave = 1;
    g->asig = sig;
    g->an += an;
    g->cc += k;
    return 0x80000000u | (uint32_t)NGH_GF_FULL << 18 | (NGH_GF_FULL ? an : pn) << 17 | (NGH_GF_FULL ? pn : an) << 16 |
           (k > 0xFFFFu ? 0xFFFFu : k);
}
#endif
