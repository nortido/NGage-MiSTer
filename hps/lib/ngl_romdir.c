// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "ngl_romdir.h"

#define RDIR_BASE  0x8cu
#define RDIR_ROOT  0x94u
#define RDIR_HEAD  0x98u
#define RDIR_ROOTS 64u
#define RDIR_ENT   10u
#define RDIR_ISDIR 0x10u

struct rdir_rom {
    const uint8_t *b;
    size_t len;
    uint32_t base;
};

static uint32_t rdir_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int rdir_lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int rdir_fail(struct ngl_romdir *d, const char *msg, const char *arg)
{
    d->n = 0;
    d->used = 0;
    snprintf(d->err, sizeof d->err, "%s%.40s", msg, arg ? arg : "");
    return -1;
}

static int rdir_at(const struct rdir_rom *r, uint32_t lin, uint64_t need, size_t *off)
{
    uint64_t o;

    if (lin < r->base)
        return -1;
    o = lin - r->base;
    if (o > r->len || need > r->len - o)
        return -1;
    *off = (size_t)o;
    return 0;
}

static int rdir_open(const struct rdir_rom *r, uint32_t lin, const uint8_t **p, const uint8_t **end)
{
    size_t o;
    uint32_t isz;

    if (rdir_at(r, lin, 4, &o))
        return -1;
    isz = rdir_u32(r->b + o);
    if (rdir_at(r, lin, 4 + (uint64_t)isz, &o))
        return -1;
    *p = r->b + o + 4;
    *end = *p + isz;
    return 0;
}

static int rdir_entry(const uint8_t *p, const uint8_t *end, const uint8_t **next)
{
    size_t room = (size_t)(end - p), step;
    unsigned nl;

    if (room < RDIR_ENT)
        return -1;
    nl = p[9];
    if (!nl || RDIR_ENT + 2u * nl > room)
        return -1;
    step = (RDIR_ENT + 2u * nl + 3u) & ~(size_t)3;
    // the last entry's padding may run past the directory end
    *next = step < room ? p + step : end;
    return (int)nl;
}

static unsigned rdir_unit(const uint8_t *name, unsigned i)
{
    return name[2 * i] | (unsigned)name[2 * i + 1] << 8;
}

static int rdir_match(const uint8_t *name, unsigned nl, const char *s, size_t len)
{
    unsigned i;

    if (nl != len)
        return 0;
    for (i = 0; i < nl; i++) {
        unsigned u = rdir_unit(name, i);

        if (u >= 0x80 || rdir_lc((int)u) != rdir_lc((unsigned char)s[i]))
            return 0;
    }
    return 1;
}

static int rdir_add(struct ngl_romdir *d, const uint8_t *name, unsigned nl, int is_dir)
{
    char buf[3 * 255 + 1];
    size_t o = 0;
    unsigned i;

    for (i = 0; i < nl; i++) {
        unsigned u = rdir_unit(name, i);

        // left out: no import name can match a control or surrogate unit
        if (u < 0x20 || (u >= 0xd800 && u < 0xe000))
            return 0;
        if (u < 0x80) {
            buf[o++] = (char)u;
        } else if (u < 0x800) {
            buf[o++] = (char)(0xc0 | u >> 6);
            buf[o++] = (char)(0x80 | (u & 0x3f));
        } else {
            buf[o++] = (char)(0xe0 | u >> 12);
            buf[o++] = (char)(0x80 | (u >> 6 & 0x3f));
            buf[o++] = (char)(0x80 | (u & 0x3f));
        }
    }
    buf[o++] = 0;
    if (d->n >= NGL_ROMDIR_MAX || o > NGL_ROMDIR_POOL - d->used)
        return -1;
    memcpy(d->pool + d->used, buf, o);
    d->at[d->n] = d->used;
    d->dir[d->n] = (uint8_t)is_dir;
    d->n++;
    d->used += (uint32_t)o;
    return 0;
}

int ngl_romdir_list(const uint8_t *rom, size_t len, const char *path, struct ngl_romdir *out)
{
    struct rdir_rom r = { rom, len, 0 };
    const uint8_t *p, *end, *next = NULL;
    const char *c = path;
    size_t o;
    uint32_t lin, num;
    int depth = 0, nl;

    out->n = 0;
    out->used = 0;
    out->err[0] = 0;
    if (len < RDIR_HEAD)
        return rdir_fail(out, "truncated header", NULL);
    r.base = rdir_u32(rom + RDIR_BASE);
    if (rdir_at(&r, rdir_u32(rom + RDIR_ROOT), 12, &o))
        return rdir_fail(out, "root list out of range", NULL);
    num = rdir_u32(rom + o);
    if (!num || num > RDIR_ROOTS)
        return rdir_fail(out, "bad root list", NULL);
    lin = rdir_u32(rom + o + 8);
    while (*c) {
        size_t n = strcspn(c, "/");
        int found = 0;

        if (!n)
            return rdir_fail(out, "bad path", NULL);
        if (++depth > NGL_ROMDIR_DEPTH)
            return rdir_fail(out, "path too deep", NULL);
        if (rdir_open(&r, lin, &p, &end))
            return rdir_fail(out, "directory out of range", NULL);
        for (; p < end && !found; p = next) {
            nl = rdir_entry(p, end, &next);
            if (nl < 0)
                return rdir_fail(out, "entry overruns directory", NULL);
            if ((p[8] & RDIR_ISDIR) && rdir_match(p + RDIR_ENT, (unsigned)nl, c, n)) {
                lin = rdir_u32(p + 4);
                found = 1;
            }
        }
        if (!found)
            return rdir_fail(out, "no ", path);
        c += n;
        if (*c == '/' && !*++c)
            return rdir_fail(out, "bad path", NULL);
    }
    if (rdir_open(&r, lin, &p, &end))
        return rdir_fail(out, "directory out of range", NULL);
    for (; p < end; p = next) {
        nl = rdir_entry(p, end, &next);
        if (nl < 0)
            return rdir_fail(out, "entry overruns directory", NULL);
        if (rdir_add(out, p + RDIR_ENT, (unsigned)nl, p[8] & RDIR_ISDIR ? 1 : 0))
            return rdir_fail(out, "too many entries", NULL);
    }
    return 0;
}

const char *ngl_romdir_name(const struct ngl_romdir *d, int i)
{
    return i >= 0 && i < d->n ? d->pool + d->at[i] : NULL;
}

int ngl_romdir_has(const struct ngl_romdir *d, const char *name)
{
    int i;

    for (i = 0; i < d->n; i++) {
        const char *a = d->pool + d->at[i], *b = name;

        if (d->dir[i])
            continue;
        while (*a && rdir_lc((unsigned char)*a) == rdir_lc((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return 1;
    }
    return 0;
}
