// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

#define AUD_MASK (HYB_AUD_SIZE - 1u)

_Static_assert((HYB_AUD_SIZE & AUD_MASK) == 0, "ring size is a power of two");
_Static_assert(13u * AUD_MAX_CHUNK < 0x80000000u, "band arithmetic in 32 bits");

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

void aud_start(struct aud *a, const struct aud_io *io, uint32_t version)
{
    struct aud z = { 0 };

    *a = z;
    a->on = version >= HYB_VERSION_AUD;
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
    uint32_t prime = (AUD_PRIME_N * n + 7u) / 8u;

    io->wr(io->ctx, HYB_R_AUD_CTL, 0);
    // EN rising sets RD := WR: a word-aligned start keeps stereo frames on their lanes
    a->wr = (a->wr + 7u) & ~7u & AUD_MASK;
    io->wr(io->ctx, HYB_R_AUD_WR, a->wr);
    a->rate = rate;
    a->ch = ch;
    a->nom = aud_step(rate);
    a->step = a->nom;
    a->trim = 0;
    a->mdir = 0;
    a->since = 0;
    a->ctl = HYB_AUD_CTL_EN | (ch == 2u ? HYB_AUD_CTL_STEREO : 0u) | prime << HYB_AUD_CTL_PRIME_SHIFT;
    io->wr(io->ctx, HYB_R_AUD_STEP, a->step);
    io->wr(io->ctx, HYB_R_AUD_CTL, a->ctl);
    a->cfg++;
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
    io->publish(io->ctx);
    a->wr = off;
    io->wr(io->ctx, HYB_R_AUD_WR, a->wr);
    a->bytes += n;
    a->chunks++;
    if (++a->since > AUD_PRIME_N)
        aud_trim(a, io, used + n, n);
    io->wr(io->ctx, HYB_R_AUD_CTL, a->ctl);
    return AUD_OK;
}
