// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "ngl_rsc.h"

static const struct ngl_rsc_slot k3_slots[] = {
    { 0x96f366, 0x96f368 },
    { 0x96fa1f, 0x96fa20 },
    { 0x96fbec, 0x96fbee },
    { 0x96feb6, 0x96feb8 },
    { 0x97099c, 0x97099e },
};

const struct ngl_rsc_table ngl_rsc_k3 = { k3_slots, 5, NGL_ROM_PA };

static int fail(char *err, size_t cap, const char *msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
    return -1;
}

static int fail_n(char *err, size_t cap, const char *fmt, int a)
{
    if (err && cap)
        snprintf(err, cap, fmt, a);
    return -1;
}

static uint32_t rd_le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int slot_ok(const uint8_t *len_p, const uint8_t *str_p)
{
    static const char pre[] = "?:\\system\\apps\\";
    unsigned units = 0, i;

    while (units < NGL_RSC_SLOT_UNITS && (str_p[2 * units] || str_p[2 * units + 1]))
        units++;
    if (units == NGL_RSC_SLOT_UNITS || *len_p != units || units < sizeof pre - 1)
        return 0;
    for (i = 0; i < units; i++)
        if (str_p[2 * i + 1] || str_p[2 * i] >= 0x80)
            return 0;
    for (i = 0; i < sizeof pre - 1; i++)
        if (pre[i] != '?' && lower((char)str_p[2 * i]) != pre[i])
            return 0;
    return 1;
}

int ngl_rsc_entries(ngl_rd_fn rd, void *ctx, const char *path, const struct ngl_rsc_table *tab,
                    struct ngl_pair *out, size_t cap, size_t *n, char *err, size_t errcap)
{
    size_t plen = strlen(path), i, cnt = 0;
    int s;

    *n = 0;
    if (plen == 0 || plen > NGL_SLOT_CHARS)
        return fail_n(err, errcap, "path longer than 31 chars or empty (%d)", (int)plen);
    for (i = 0; i < plen; i++)
        if ((unsigned char)path[i] >= 0x80)
            return fail(err, errcap, "path is not ASCII");
    for (s = 0; s < tab->n; s++) {
        uint32_t start = (tab->slot[s].len_off - 2u) & ~3u;
        uint32_t end = (tab->slot[s].str_off + NGL_RSC_SLOT_BYTES + 3u) & ~3u;
        uint32_t nwords = (end - start) / 4;
        uint8_t old[NGL_RSC_MAX_WORDS * 4], nw[NGL_RSC_MAX_WORDS * 4];
        uint32_t lo = tab->slot[s].len_off - start, so = tab->slot[s].str_off - start, w;
        unsigned type;

        if (tab->slot[s].str_off < tab->slot[s].len_off || nwords > NGL_RSC_MAX_WORDS)
            return fail_n(err, errcap, "bad slot table entry %d", s + 1);
        if (rd(ctx, tab->base_pa + start, old, nwords * 4))
            return fail_n(err, errcap, "ROM read failed at slot %d", s + 1);
        if (!slot_ok(old + lo, old + so))
            return fail_n(err, errcap, "ROM slot %d unknown", s + 1);
        type = old[lo - 2] | (unsigned)old[lo - 1] << 8;
        if (type != 2 && type != 7)
            return fail_n(err, errcap, "ROM slot %d type unknown", s + 1);
        memcpy(nw, old, nwords * 4);
        // Starter 0x5096cbfc starts type 7 with EApaCommandBackground; type 2 gets the Run command
        nw[lo - 2] = 2;
        nw[lo] = (uint8_t)plen;
        memset(nw + so, 0, NGL_RSC_SLOT_BYTES);
        for (i = 0; i < plen; i++)
            nw[so + 2 * i] = (uint8_t)path[i];
        for (w = 0; w < nwords; w++) {
            uint32_t a = rd_le(old + 4 * w), b = rd_le(nw + 4 * w);
            if (a == b)
                continue;
            if (cnt >= cap)
                return fail(err, errcap, "too many rewrite entries");
            out[cnt].off = start + 4 * w;
            out[cnt].orig = a;
            out[cnt].repl = b;
            cnt++;
        }
    }
    *n = cnt;
    return 0;
}
