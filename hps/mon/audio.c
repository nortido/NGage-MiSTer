// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

#define AUD_MASK (HYB_AUD_SIZE - 1u)

_Static_assert((HYB_AUD_SIZE & AUD_MASK) == 0, "ring size is a power of two");
_Static_assert(13u * AUD_MAX_CHUNK < 0x80000000u, "band arithmetic in 32 bits");
_Static_assert(AUD_PRIME_N * AUD_MAX_CHUNK < HYB_AUD_SIZE, "prime fits the ring");

// rate * 2^32 / 48000 bit by bit: the monitor links no division helper
uint32_t aud_step(uint32_t rate)
{
    uint32_t q = 0, rem = rate, i;

    if (rate < AUD_RATE_MIN || rate > AUD_RATE_OUT)
        return 0;
    if (rate == AUD_RATE_OUT)
        return 0xFFFFFFFFu;
    for (i = 0; i < 32; i++) {
        rem <<= 1;
        q <<= 1;
        if (rem >= AUD_RATE_OUT) {
            rem -= AUD_RATE_OUT;
            q |= 1u;
        }
    }
    return q;
}

void aud_args(uint32_t (*get)(void *ctx, unsigned mode, unsigned n), void *ctx, unsigned mode, uint32_t r[4])
{
    unsigned i;

    for (i = 0; i < 4; i++)
        r[i] = get(ctx, mode, i);
}

#if NGH_AUD_CAP
static void aud_cap_init(const struct aud_io *io)
{
    uint32_t h[AUDC_HDR_N] = { AUD_CAP_MAGIC, AUD_CAP_VER, AUD_CAP_IDX, AUD_CAP_ENT, AUD_CAP_NENT, AUD_CAP_DATA, AUD_CAP_DMAX };
    uint32_t i;

    for (i = 0; i < AUDC_HDR_N; i++)
        io->cap_word(io->ctx, i * 4u, h[i]);
}

static void aud_cap_put(struct aud *a, const struct aud_io *io, const uint32_t *pa, uint32_t np, uint32_t n, uint32_t used, uint32_t va)
{
    uint32_t e[AUDE_N_WORDS] = { a->chunks, a->cap_wr, n, a->rate, a->ch, a->last, used, a->under, va, a->drops, a->skips };
    uint32_t i, k, left = n, off = AUD_CAP_DATA + a->cap_wr;

    // the first refusal ends it: a later chunk that still fits would hide the hole
    if (a->cap_full || a->cap_n >= AUD_CAP_NENT || n > AUD_CAP_DMAX - a->cap_wr) {
        a->cap_full = 1;
        io->cap_word(io->ctx, AUDC_LOST * 4u, ++a->cap_lost);
        return;
    }
    for (i = 0; i < np; i++) {
        k = AUD_PAGE - (pa[i] & (AUD_PAGE - 1u));
        k = k < left ? k : left;
        io->cap_data(io->ctx, off, pa[i], k);
        off += k;
        left -= k;
    }
    for (i = 0; i < AUDE_N_WORDS; i++)
        io->cap_word(io->ctx, AUD_CAP_IDX + a->cap_n * AUD_CAP_ENT + i * 4u, e[i]);
    a->cap_wr += n;
    a->cap_n++;
    io->cap_word(io->ctx, AUDC_WR * 4u, a->cap_wr);
    io->cap_word(io->ctx, AUDC_COUNT * 4u, a->cap_n);
}
#endif

void aud_start(struct aud *a, const struct aud_io *io, uint32_t version)
{
    struct aud z = { 0 };

    *a = z;
    a->on = version >= HYB_VERSION_AUD;
#if NGH_AUD_CAP
    if (a->on)
        aud_cap_init(io);
#endif
    if (a->on)
        io->wr(io->ctx, HYB_R_AUD_CTL, 0);
}

void aud_park(const struct aud *a, const struct aud_io *io)
{
    if (a->on)
        io->wr(io->ctx, HYB_R_AUD_CTL, 0);
}

static void aud_cfg(struct aud *a, const struct aud_io *io, uint32_t rate, uint32_t ch, uint32_t n)
{
    uint32_t lo = AUD_PRIME_N * n, hi = AUD_PRIME_MAX * n, deep = 2u * rate * ch / 5u;
    uint32_t c = HYB_AUD_CTL_EN | (ch == 2u ? HYB_AUD_CTL_STEREO : 0u);

    deep = deep > hi ? hi : deep;
    deep = deep < lo ? lo : deep;
    io->wr(io->ctx, HYB_R_AUD_CTL, 0);
    // EN rising sets RD := WR: a word-aligned start keeps stereo frames on their lanes
    a->wr = (a->wr + 7u) & ~7u & AUD_MASK;
    io->wr(io->ctx, HYB_R_AUD_WR, a->wr);
    a->rate = rate;
    a->ch = ch;
    a->bpms = 2u * rate * ch / 1000u;
    a->nom = aud_step(rate);
    a->step = a->nom;
    a->trim = 0;
    a->mdir = 0;
    a->since = 0;
    a->shallow = c | (lo + 7u) / 8u << HYB_AUD_CTL_PRIME_SHIFT;
    a->deep = c | (deep + 7u) / 8u << HYB_AUD_CTL_PRIME_SHIFT;
    a->ctl = a->shallow;
    a->under = io->rd(io->ctx, HYB_R_AUD_UNDER);
    a->last = io->now(io->ctx);
    io->wr(io->ctx, HYB_R_AUD_STEP, a->step);
    io->wr(io->ctx, HYB_R_AUD_CTL, a->ctl);
    a->cfg++;
}

static void aud_watch(struct aud *a, const struct aud_io *io)
{
    uint32_t u = io->rd(io->ctx, HYB_R_AUD_UNDER), t = io->now(io->ctx), ctl = a->ctl;

    // a chunk gap over AUD_GAP ends the stream; an underrun with a shorter one is a stall
    if (t - a->last > AUD_GAP)
        ctl = a->shallow;
    else if (u != a->under)
        ctl = a->deep;
    a->last = t;
    a->under = u;
    // before this chunk's WR, so the re-prime the FPGA waits in takes the new value
    if (ctl != a->ctl) {
        a->ctl = ctl;
        io->wr(io->ctx, HYB_R_AUD_CTL, ctl);
    }
}

// one notch at a time within 1% of nominal, while the level is outside 2..6 chunks or the trim pushes it
// toward the far edge of the band (never past 0); a further notch only if the level got worse or after AUD_TRIM_WAIT
// chunks, so the trim settles at the production error
static void aud_trim(struct aud *a, const struct aud_io *io, uint32_t level, uint32_t n)
{
    int32_t cap = (int32_t)(a->nom / AUD_TRIM_CAP), d = (int32_t)(a->nom / AUD_TRIM_DIV), t = a->trim, dir = 0, lim = cap;
    int64_t s;

    if (2u * level < (2u * AUD_BAND_LO - 1u) * n)
        dir = -1;
    else if (2u * level >= (2u * AUD_BAND_HI + 1u) * n)
        dir = 1;
    else if (t > 0 && 2u * level < (AUD_BAND_LO + AUD_BAND_HI) * n)
        dir = -1, lim = 0;
    else if (t < 0 && 2u * level > (AUD_BAND_LO + AUD_BAND_HI) * n)
        dir = 1, lim = 0;
    if (!dir) {
        a->mdir = 0;
    } else if (a->mdir != dir || (dir < 0 ? level < a->mark : level > a->mark) ||
               (++a->wait >= AUD_TRIM_WAIT && (dir < 0 ? level <= a->mark : level >= a->mark))) {
        t = dir < 0 ? (t - d < -lim ? -lim : t - d) : (t + d > lim ? lim : t + d);
        a->mdir = dir;
        a->mark = level;
        a->wait = 0;
    }
    a->trim = t;
    s = (int64_t)a->nom + t;
    s = s > 0xFFFFFFFF ? 0xFFFFFFFF : s < 1 ? 1 : s;
    if ((uint32_t)s != a->step) {
        a->step = (uint32_t)s;
        io->wr(io->ctx, HYB_R_AUD_STEP, a->step);
    }
}

int aud_chunk(struct aud *a, const struct aud_io *io, const uint32_t r[4])
{
    uint32_t pa[AUD_NPAGE], va = r[0], rate = r[2], ch = r[3], n, np, i, used, off, left, k, m, p;

    if (!a->on)
        return AUD_OFF;
    if (!r[1])
        return AUD_EMPTY;
    n = r[1] & ~(ch == 2u ? 3u : 1u);
    if (r[1] > AUD_MAX_CHUNK || (ch != 1u && ch != 2u) || !aud_step(rate) || !n) {
        a->skips++;
        return AUD_SKIP;
    }
    np = ((va & (AUD_PAGE - 1u)) + n + AUD_PAGE - 1u) / AUD_PAGE;
    for (i = 0; i < np; i++)
        if (io->va2pa(io->ctx, i ? (va & ~(AUD_PAGE - 1u)) + i * AUD_PAGE : va, &pa[i])) {
            a->skips++;
            return AUD_SKIP;
        }
    if (rate != a->rate || ch != a->ch)
        aud_cfg(a, io, rate, ch, n);
    aud_watch(a, io);
    used = (a->wr - (io->rd(io->ctx, HYB_R_AUD_RD) & ~7u)) & AUD_MASK;
    if (HYB_AUD_SIZE - used < n + AUD_SLACK) {
        a->drops++;
        io->wr(io->ctx, HYB_R_AUD_CTL, a->ctl);
        return AUD_DROP;
    }
    off = a->wr;
    left = n;
    for (i = 0; i < np; i++) {
        k = AUD_PAGE - (pa[i] & (AUD_PAGE - 1u));
        k = k < left ? k : left;
        p = pa[i];
        left -= k;
        while (k) {
            m = HYB_AUD_SIZE - off;
            m = m < k ? m : k;
            io->copy(io->ctx, off, p, m);
            off = (off + m) & AUD_MASK;
            p += m;
            k -= m;
        }
    }
#if NGH_AUD_CAP
    aud_cap_put(a, io, pa, np, n, used, va);
#endif
    io->publish(io->ctx);
    a->wr = off;
    io->wr(io->ctx, HYB_R_AUD_WR, a->wr);
    a->bytes += n;
    a->chunks++;
    a->lvl = (int32_t)(used + n);
    m = (a->ctl >> HYB_AUD_CTL_PRIME_SHIFT) * 8u / 3u;
    k = AUD_LOW_MS * a->bpms;
    a->low = (int32_t)(m < k ? m : k);
    a->end = a->low - (int32_t)(AUD_REL_MS * a->bpms);
    if (++a->since > AUD_PRIME_N)
        aud_trim(a, io, used + n, n);
    io->wr(io->ctx, HYB_R_AUD_CTL, a->ctl);
    return AUD_OK;
}

uint32_t aud_short(struct aud *a)
{
    // once per 1 ms tick: the ring drains at the stream's rate from the level the last chunk left
    if (a->lvl <= a->end)
        return 0;
    a->lvl -= (int32_t)a->bpms;
    return a->lvl < a->low;
}
