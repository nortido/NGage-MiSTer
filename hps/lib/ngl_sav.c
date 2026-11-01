// SPDX-License-Identifier: GPL-3.0-or-later
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "ngl_sav.h"

struct sav_crc {
    uint32_t t[4][256];
};

static void sav_crc_init(struct sav_crc *c)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i;
        for (int k = 0; k < 8; k++)
            v = (v & 1u) ? (v >> 1) ^ 0xEDB88320u : v >> 1;
        c->t[0][i] = v;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int k = 1; k < 4; k++)
            c->t[k][i] = (c->t[k - 1][i] >> 8) ^ c->t[0][c->t[k - 1][i] & 0xFFu];
}

static uint32_t sav_crc_upd(const struct sav_crc *c, uint32_t crc, const uint8_t *p, size_t n)
{
    while (n >= 4) {
        crc ^= (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        crc = c->t[3][crc & 0xFFu] ^ c->t[2][(crc >> 8) & 0xFFu] ^ c->t[1][(crc >> 16) & 0xFFu] ^
              c->t[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n--)
        crc = c->t[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static void sav_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t sav_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int sav_bit(const uint8_t *bm, uint32_t lba)
{
    return (bm[lba >> 3] >> (lba & 7u)) & 1;
}

static uint32_t sav_next(const uint8_t *bm, uint32_t from)
{
    uint32_t lba = from;

    while (lba < NGL_IMG_SECTORS) {
        if ((lba & 7u) == 0 && bm[lba >> 3] == 0) {
            lba += 8;
            continue;
        }
        if (sav_bit(bm, lba))
            return lba;
        lba++;
    }
    return NGL_IMG_SECTORS;
}

void ngl_sav_init(struct ngl_sav *s, uint8_t *bitmap)
{
    memset(s, 0, sizeof(*s));
    s->bitmap = bitmap;
    memset(bitmap, 0, NGL_SAV_BITMAP_BYTES);
}

uint32_t ngl_sav_crc32(const void *buf, size_t len)
{
    struct sav_crc c;

    sav_crc_init(&c);
    return ~sav_crc_upd(&c, 0xFFFFFFFFu, buf, len);
}

uint32_t ngl_sav_image_id(const uint8_t *img)
{
    return ngl_sav_crc32(img, (size_t)NGL_IMG_SECTORS * 512u);
}

int ngl_sav_store(struct ngl_sav *s, uint8_t *img, uint64_t off, const void *data, uint32_t len,
                  uint32_t now_ms)
{
    const uint64_t total = (uint64_t)NGL_IMG_SECTORS * 512u;
    int st = NGL_SAV_OK;
    int kept = 0;

    if (off > total || len > total - off)
        return NGL_SAV_RANGE;
    if (len == 0)
        return NGL_SAV_OK;
    memcpy(img + off, data, len);
    for (uint32_t lba = (uint32_t)(off >> 9); lba <= (uint32_t)((off + len - 1) >> 9); lba++) {
        if (sav_bit(s->bitmap, lba)) {
            kept = 1;
        } else if (s->dirty >= NGL_SAV_MAX_RECORDS) {
            if (!s->full) {
                s->full = 1;
                st = NGL_SAV_FULL;
            }
        } else {
            s->bitmap[lba >> 3] |= (uint8_t)(1u << (lba & 7u));
            s->dirty++;
            kept = 1;
        }
    }
    if (kept) {
        s->pending = 1;
        s->last_write_ms = now_ms;
    }
    return st;
}

uint32_t ngl_sav_size(const struct ngl_sav *s)
{
    return NGL_SAV_HDR_BYTES + s->dirty * NGL_SAV_REC_BYTES;
}

int ngl_sav_serialize(const struct ngl_sav *s, const uint8_t *img, uint32_t image_id,
                      const struct ngl_sink *sink)
{
    struct sav_crc c;
    uint8_t hdr[NGL_SAV_HDR_BYTES];
    uint8_t lbuf[4];
    uint8_t batch[8 * NGL_SAV_REC_BYTES];
    uint32_t count = 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t off = NGL_SAV_HDR_BYTES;
    size_t fill = 0;
    uint32_t lba;

    if (s->dirty > NGL_SAV_MAX_RECORDS)
        return NGL_SAV_RANGE;
    for (lba = sav_next(s->bitmap, 0); lba < NGL_IMG_SECTORS; lba = sav_next(s->bitmap, lba + 1)) {
        if (++count > NGL_SAV_MAX_RECORDS)
            return NGL_SAV_RANGE;
    }
    sav_crc_init(&c);
    for (lba = sav_next(s->bitmap, 0); lba < NGL_IMG_SECTORS; lba = sav_next(s->bitmap, lba + 1)) {
        sav_put32(lbuf, lba);
        crc = sav_crc_upd(&c, crc, lbuf, 4);
        crc = sav_crc_upd(&c, crc, img + (size_t)lba * 512u, 512);
    }
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "NGSV", 4);
    sav_put32(hdr + 4, NGL_SAV_VERSION);
    sav_put32(hdr + 8, image_id);
    sav_put32(hdr + 12, count);
    sav_put32(hdr + 16, ~crc);
    if (sink->write(sink->ctx, 0, hdr, NGL_SAV_HDR_BYTES) != 0)
        return NGL_SAV_SINK;
    for (lba = sav_next(s->bitmap, 0); lba < NGL_IMG_SECTORS; lba = sav_next(s->bitmap, lba + 1)) {
        sav_put32(batch + fill, lba);
        memcpy(batch + fill + 4, img + (size_t)lba * 512u, 512);
        fill += NGL_SAV_REC_BYTES;
        if (fill == sizeof(batch)) {
            if (sink->write(sink->ctx, off, batch, (uint32_t)fill) != 0)
                return NGL_SAV_SINK;
            off += fill;
            fill = 0;
        }
    }
    if (fill && sink->write(sink->ctx, off, batch, (uint32_t)fill) != 0)
        return NGL_SAV_SINK;
    return NGL_SAV_OK;
}

int ngl_sav_parse(const uint8_t *buf, size_t len, uint32_t image_id, struct ngl_sav_info *info)
{
    struct ngl_sav_info local;
    struct sav_crc c;
    uint32_t count;
    uint32_t prev = 0;

    if (!info)
        info = &local;
    info->id = 0;
    info->count = 0;
    if (!buf || len == 0)
        return NGL_SAV_ABSENT;
    if (len < 4 || memcmp(buf, "NGSV", 4) != 0)
        return NGL_SAV_FOREIGN;
    if (len >= 8 && sav_get32(buf + 4) != NGL_SAV_VERSION)
        return NGL_SAV_FOREIGN;
    if (len < NGL_SAV_HDR_BYTES) {
        info->id = ngl_sav_crc32(buf, len);
        return NGL_SAV_CORRUPT;
    }
    info->id = sav_get32(buf + 8);
    count = sav_get32(buf + 12);
    if (count > NGL_SAV_MAX_RECORDS || len != NGL_SAV_HDR_BYTES + (size_t)count * NGL_SAV_REC_BYTES)
        return NGL_SAV_CORRUPT;
    info->count = count;
    sav_crc_init(&c);
    if (~sav_crc_upd(&c, 0xFFFFFFFFu, buf + NGL_SAV_HDR_BYTES, len - NGL_SAV_HDR_BYTES) !=
        sav_get32(buf + 16))
        return NGL_SAV_CORRUPT;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t lba = sav_get32(buf + NGL_SAV_HDR_BYTES + (size_t)i * NGL_SAV_REC_BYTES);

        if (lba >= NGL_IMG_SECTORS || (i > 0 && lba <= prev))
            return NGL_SAV_CORRUPT;
        prev = lba;
    }
    if (sav_get32(buf + 8) != image_id)
        return NGL_SAV_MISMATCH;
    return NGL_SAV_OK;
}

int ngl_sav_apply(struct ngl_sav *s, uint8_t *img, const uint8_t *buf, size_t len,
                  uint32_t image_id, uint32_t *applied)
{
    struct ngl_sav_info info;
    int st;

    if (applied)
        *applied = 0;
    st = ngl_sav_parse(buf, len, image_id, &info);
    if (st != NGL_SAV_OK)
        return st;
    if (s->dirty + info.count > NGL_SAV_MAX_RECORDS)
        return NGL_SAV_FULL;
    for (uint32_t i = 0; i < info.count; i++) {
        const uint8_t *r = buf + NGL_SAV_HDR_BYTES + (size_t)i * NGL_SAV_REC_BYTES;
        uint32_t lba = sav_get32(r);

        memcpy(img + (size_t)lba * 512u, r + 4, 512);
        if (!sav_bit(s->bitmap, lba)) {
            s->bitmap[lba >> 3] |= (uint8_t)(1u << (lba & 7u));
            s->dirty++;
        }
    }
    if (applied)
        *applied = info.count;
    return NGL_SAV_OK;
}

int ngl_sav_due(const struct ngl_sav *s, uint32_t now_ms, int unload)
{
    if (!s->pending)
        return 0;
    if (unload)
        return 1;
    if (s->sched_flushes >= NGL_SAV_MAX_FLUSHES)
        return 0;
    if ((uint32_t)(now_ms - s->last_write_ms) < NGL_SAV_DEBOUNCE_MS)
        return 0;
    if (s->flushed_once && (uint32_t)(now_ms - s->last_flush_ms) < NGL_SAV_GAP_MS)
        return 0;
    return 1;
}

void ngl_sav_flushed(struct ngl_sav *s, uint32_t now_ms, int scheduled)
{
    s->pending = 0;
    s->last_flush_ms = now_ms;
    s->flushed_once = 1;
    if (scheduled)
        s->sched_flushes++;
}

void ngl_sav_flush_failed(struct ngl_sav *s, uint32_t now_ms, int scheduled)
{
    s->last_flush_ms = now_ms;
    s->flushed_once = 1;
    if (scheduled)
        s->sched_flushes++;
}

int ngl_sav_aside_name(const char *sav_path, uint32_t id, ngl_sav_exists_fn exists, void *ctx,
                       char *out, size_t cap)
{
    if (!sav_path || !sav_path[0] || !exists || !out || cap == 0)
        return NGL_SAV_NAME;
    for (uint32_t k = 0; k < NGL_SAV_ASIDE_TRIES; k++) {
        int n;

        if (k == 0)
            n = snprintf(out, cap, "%s.%08x", sav_path, (unsigned)id);
        else
            n = snprintf(out, cap, "%s.%08x.%u", sav_path, (unsigned)id, (unsigned)k);
        if (n < 0 || (size_t)n >= cap)
            return NGL_SAV_NAME;
        if (!exists(ctx, out))
            return NGL_SAV_OK;
    }
    return NGL_SAV_NAME;
}

int ngl_sav_path(const char *dir, const char *zip_path, char *out, size_t cap)
{
    const char *base;
    size_t len;
    int n;

    if (!dir || !dir[0] || !zip_path || !out || cap == 0)
        return NGL_SAV_NAME;
    base = strrchr(zip_path, '/');
    base = base ? base + 1 : zip_path;
    len = strlen(base);
    if (len >= 4 && base[len - 4] == '.' && tolower((unsigned char)base[len - 3]) == 'z' &&
        tolower((unsigned char)base[len - 2]) == 'i' && tolower((unsigned char)base[len - 1]) == 'p')
        len -= 4;
    if (len == 0 || len > (size_t)INT_MAX)
        return NGL_SAV_NAME;
    n = snprintf(out, cap, "%s/%.*s.sav", dir, (int)len, base);
    if (n < 0 || (size_t)n >= cap)
        return NGL_SAV_NAME;
    return NGL_SAV_OK;
}

const char *ngl_sav_str(int status)
{
    switch (status) {
    case NGL_SAV_OK: return "ok";
    case NGL_SAV_ABSENT: return "absent";
    case NGL_SAV_FOREIGN: return "foreign";
    case NGL_SAV_MISMATCH: return "mismatch";
    case NGL_SAV_CORRUPT: return "corrupt";
    case NGL_SAV_FULL: return "full";
    case NGL_SAV_RANGE: return "range";
    case NGL_SAV_SINK: return "sink";
    case NGL_SAV_NAME: return "name";
    default: return "unknown";
    }
}
