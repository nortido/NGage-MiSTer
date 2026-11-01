// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "ngl_cdrv.h"
#include "ngl_sav.h"

static void cdrv_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t cdrv_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void ngl_cdrv_hdr(uint8_t *out, uint32_t rom_id, uint32_t payload_crc)
{
    memset(out, 0, NGL_CDRV_HDR_BYTES);
    memcpy(out, "NGCD", 4);
    cdrv_put32(out + 4, NGL_CDRV_VERSION);
    cdrv_put32(out + 8, NGL_CDRV_PAYLOAD_BYTES);
    cdrv_put32(out + 12, rom_id);
    cdrv_put32(out + 16, payload_crc);
}

int ngl_cdrv_parse(const uint8_t *buf, size_t len, uint32_t rom_id, struct ngl_cdrv_info *info)
{
    struct ngl_cdrv_info local;

    if (!info)
        info = &local;
    info->id = 0;
    info->crc = 0;
    if (!buf || len == 0)
        return NGL_CDRV_ABSENT;
    if (len < 4 || memcmp(buf, "NGCD", 4) != 0)
        return NGL_CDRV_FOREIGN;
    if (len >= 8 && cdrv_get32(buf + 4) != NGL_CDRV_VERSION)
        return NGL_CDRV_FOREIGN;
    if (len >= 12 && cdrv_get32(buf + 8) != NGL_CDRV_PAYLOAD_BYTES)
        return NGL_CDRV_FOREIGN;
    if (len < NGL_CDRV_HDR_BYTES)
        return NGL_CDRV_CORRUPT;
    info->id = cdrv_get32(buf + 12);
    info->crc = cdrv_get32(buf + 16);
    if (len != NGL_CDRV_FILE_BYTES)
        return NGL_CDRV_CORRUPT;
    if (ngl_sav_crc32(buf + NGL_CDRV_HDR_BYTES, NGL_CDRV_PAYLOAD_BYTES) != info->crc)
        return NGL_CDRV_CORRUPT;
    if (info->id != rom_id)
        return NGL_CDRV_ROMCHANGED;
    return NGL_CDRV_OK;
}

struct ngl_cdrv_action ngl_cdrv_action(int status)
{
    struct ngl_cdrv_action a = { 0, 1, NGL_CDRV_ASIDE_NONE, 0, 0, NGL_CDRV_INFO_CANNOT_READ };

    switch (status) {
    case NGL_CDRV_OK:
        a.use_payload = 1;
        a.fill_ff = 0;
        a.persist = 1;
        a.info = NGL_CDRV_INFO_NONE;
        break;
    case NGL_CDRV_ABSENT:
        a.persist = 1;
        a.info = NGL_CDRV_INFO_NONE;
        break;
    case NGL_CDRV_ROMCHANGED:
        a.use_payload = 1;
        a.fill_ff = 0;
        a.aside = NGL_CDRV_ASIDE_COPY;
        a.persist = 1;
        a.write_now = 1;
        a.info = NGL_CDRV_INFO_ROM_CHANGED;
        break;
    case NGL_CDRV_CORRUPT:
        a.aside = NGL_CDRV_ASIDE_RENAME;
        a.persist = 1;
        a.info = NGL_CDRV_INFO_DAMAGED;
        break;
    case NGL_CDRV_FOREIGN:
        a.info = NGL_CDRV_INFO_FOREIGN;
        break;
    default:
        break;
    }
    return a;
}

// the guard mark row counts ESTART fallbacks to the Lffs format, reached only after a failed C: mount
int ngl_cdrv_guard(struct ngl_cdrv_action a, int payload_nonblank, uint32_t row)
{
    return a.use_payload && payload_nonblank && row >= 1u ? NGL_CDRV_ASIDE_RENAME : NGL_CDRV_ASIDE_NONE;
}

int ngl_cdrv_snapshot(ngl_cdrv_gen_fn rd_gen, ngl_cdrv_read_fn mem_read, void *ctx, uint8_t *dst,
                      unsigned tries)
{
    for (unsigned t = 0; t < tries; t++) {
        uint32_t g0, g1;
        uint32_t off;

        if (rd_gen(ctx, &g0) != 0)
            return NGL_CDRV_SNAP_IO;
        if (g0 & 1u)
            continue;
        for (off = 0; off < NGL_CDRV_PAYLOAD_BYTES; off += NGL_CDRV_BLOCK_BYTES)
            if (mem_read(ctx, off, dst + off, NGL_CDRV_BLOCK_BYTES) != 0)
                return NGL_CDRV_SNAP_IO;
        if (rd_gen(ctx, &g1) != 0)
            return NGL_CDRV_SNAP_IO;
        if (g1 == g0)
            return NGL_CDRV_SNAP_OK;
    }
    return NGL_CDRV_SNAP_BUSY;
}

void ngl_cdrv_observe(struct ngl_cdrv_sched *s, uint32_t gen, uint32_t now_ms)
{
    if (gen == s->gen)
        return;
    s->gen = gen;
    s->pending = 1;
    s->last_change_ms = now_ms;
}

void ngl_cdrv_rebase(struct ngl_cdrv_sched *s, uint32_t gen)
{
    s->gen = gen;
}

void ngl_cdrv_mark_pending(struct ngl_cdrv_sched *s, uint32_t now_ms)
{
    s->pending = 1;
    s->last_change_ms = now_ms;
}

int ngl_cdrv_due(const struct ngl_cdrv_sched *s, uint32_t now_ms, int force)
{
    if (!s->pending)
        return 0;
    if (force)
        return 1;
    if (s->sched_flushes >= NGL_CDRV_MAX_FLUSHES)
        return 0;
    if ((uint32_t)(now_ms - s->last_change_ms) < NGL_CDRV_DEBOUNCE_MS)
        return 0;
    if (s->flushed_once && (uint32_t)(now_ms - s->last_flush_ms) < NGL_CDRV_GAP_MS)
        return 0;
    return 1;
}

void ngl_cdrv_flushed(struct ngl_cdrv_sched *s, uint32_t gen, uint32_t now_ms, int scheduled)
{
    s->gen = gen;
    s->pending = 0;
    s->last_flush_ms = now_ms;
    s->flushed_once = 1;
    if (scheduled)
        s->sched_flushes++;
}

void ngl_cdrv_flush_failed(struct ngl_cdrv_sched *s, uint32_t now_ms, int scheduled)
{
    s->last_flush_ms = now_ms;
    s->flushed_once = 1;
    if (scheduled)
        s->sched_flushes++;
}

int ngl_cdrv_path(const char *dir, const char *name, char *out, size_t cap)
{
    int n;

    if (!name)
        name = NGL_CDRV_NAME;
    if (!dir || !dir[0] || !name[0] || !out || cap == 0)
        return NGL_CDRV_NAME_ERR;
    n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap)
        return NGL_CDRV_NAME_ERR;
    return NGL_CDRV_OK;
}

const char *ngl_cdrv_str(int status)
{
    switch (status) {
    case NGL_CDRV_OK: return "ok";
    case NGL_CDRV_ABSENT: return "absent";
    case NGL_CDRV_FOREIGN: return "foreign";
    case NGL_CDRV_ROMCHANGED: return "romchanged";
    case NGL_CDRV_CORRUPT: return "corrupt";
    case NGL_CDRV_IOERR: return "ioerr";
    case NGL_CDRV_NAME_ERR: return "name";
    default: return "unknown";
    }
}
