// SPDX-License-Identifier: GPL-3.0-or-later
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngl_fat.h"
#include "ngl_libs.h"

#define LB_HEAD     0x7cu
#define LB_REFS     256
#define LB_NAME     64
#define LB_IMP_MAX  0x10000u
#define LB_FOLDER   "System/Libs"

struct lb_ref {
    char name[LB_NAME + 1];
    uint32_t ord;
};

struct lb_work {
    uint8_t win[LB_IMP_MAX];
    struct lb_ref refs[LB_REFS];
};

struct lb_win {
    uint8_t *buf;
    uint32_t lo;
    uint32_t hi;
    uint32_t end;
};

static char lb_lc(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int lb_ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lb_lc(*a) != lb_lc(*b))
            return 0;
    return *a == *b;
}

static int lb_ci_pre(const char *s, const char *pre)
{
    for (; *pre; s++, pre++)
        if (lb_lc(*s) != lb_lc(*pre))
            return 0;
    return 1;
}

static const char *lb_base(const char *p)
{
    const char *s = strrchr(p, '/');

    return s ? s + 1 : p;
}

static int lb_ends(const char *s, const char *sfx)
{
    size_t l = strlen(s), n = strlen(sfx);

    return l >= n && lb_ci_eq(s + l - n, sfx);
}

static int lb_image_name(const char *p)
{
    return lb_ends(p, ".app") || lb_ends(p, ".dll") || lb_ends(p, ".exe") || lb_ends(p, ".nax");
}

static uint32_t lb_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int lb_win_write(void *ctx, uint64_t off, const void *data, uint32_t len)
{
    struct lb_win *w = ctx;
    uint64_t a = off > w->lo ? off : w->lo;
    uint64_t b = off + len < w->hi ? off + len : w->hi;

    if (a < b) {
        memcpy(w->buf + (a - w->lo), (const uint8_t *)data + (a - off), (size_t)(b - a));
        w->end = (uint32_t)b;
    }
    return off + len >= w->hi ? -1 : 0;
}

// the sink refuses once the window is full, so the source stops there
static int lb_read(const struct ngl_game_src *s, int idx, uint32_t lo, uint32_t hi, uint8_t *buf)
{
    struct lb_win w = { buf, lo, hi, lo };
    struct ngl_sink sk = { lb_win_write, &w };

    s->ops->extract(s->ctx, idx, &sk, 0);
    return w.end == hi ? 0 : -1;
}

static int lb_name_ok(const char *s, size_t n)
{
    size_t i;

    if (!n || n > LB_NAME)
        return 0;
    for (i = 0; i < n; i++)
        if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] > 0x7e || strchr("\"*/:<>?\\|", s[i]))
            return 0;
    return 1;
}

static void lb_strip(char *s)
{
    char *b = strchr(s, '[');
    int i;

    if (!b || strlen(b) < 10 || b[9] != ']')
        return;
    for (i = 1; i < 9; i++)
        if (!isxdigit((unsigned char)b[i]))
            return;
    memmove(b, b + 10, strlen(b + 10) + 1);
}

static int lb_scan(const struct ngl_game_src *s, int idx, uint32_t size, struct lb_work *w, int *nref, uint32_t *exports)
{
    uint8_t h[LB_HEAD];
    uint32_t uid, nblk, impo, avail, isz, pos, k;

    *nref = 0;
    *exports = 0;
    if (size < LB_HEAD || size > NGL_LIBS_READ_MAX || lb_read(s, idx, 0, LB_HEAD, h))
        return -1;
    uid = lb_rd32(h);
    if ((uid != 0x10000079u && uid != 0x1000007au) || memcmp(h + 0x10, "EPOC", 4) || lb_rd32(h + 0x1c) ||
        (lb_rd32(h + 0x2c) & 0xf0000000u) || (uint64_t)lb_rd32(h + 0x64) + lb_rd32(h + 0x30) > size)
        return -1;
    *exports = lb_rd32(h + 0x5c);
    nblk = lb_rd32(h + 0x54);
    impo = lb_rd32(h + 0x6c);
    if (!nblk)
        return 0;
    if (nblk > LB_REFS || (uint64_t)impo + 4 > size)
        return -1;
    avail = size - impo < LB_IMP_MAX ? size - impo : LB_IMP_MAX;
    if (lb_read(s, idx, impo, impo + avail, w->win))
        return -1;
    isz = lb_rd32(w->win);
    if (isz < 4 || isz > avail)
        return -1;
    for (pos = 4, k = 0; k < nblk; k++) {
        uint32_t noff, cnt, mx = 0, j;
        const uint8_t *z;
        struct lb_ref *r = &w->refs[k];

        if (isz - pos < 8)
            return -1;
        noff = lb_rd32(w->win + pos);
        cnt = lb_rd32(w->win + pos + 4);
        pos += 8;
        if (cnt > (isz - pos) / 4)
            return -1;
        for (j = 0; j < cnt; j++) {
            uint32_t v = lb_rd32(w->win + pos + 4 * j);
            mx = v > mx ? v : mx;
        }
        pos += cnt * 4;
        if (noff < 4 || noff >= isz)
            return -1;
        z = memchr(w->win + noff, 0, isz - noff);
        if (!z || !lb_name_ok((const char *)w->win + noff, (size_t)(z - (w->win + noff))))
            return -1;
        memcpy(r->name, w->win + noff, (size_t)(z - (w->win + noff)) + 1);
        lb_strip(r->name);
        if (!r->name[0])
            return -1;
        r->ord = mx;
        *nref = (int)k + 1;
    }
    return 0;
}

static int lb_zip_has(const struct ngl_game_src *g, int n, const char *name)
{
    int i;

    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = g->ops->entry(g->ctx, i);

        if (e && !e->is_dir && lb_ci_eq(lb_base(e->path), name))
            return 1;
    }
    return 0;
}

static int lb_src_find(const struct ngl_game_src *l, const char *name)
{
    int i, n = l->ops->count(l->ctx);

    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = l->ops->entry(l->ctx, i);

        if (e && !e->is_dir && !strchr(e->path, '/') && lb_ci_eq(e->path, name))
            return i;
    }
    return -1;
}

// the FAT builder compares directory names exactly, so the added file follows the zip's own spelling
static void lb_folder(const struct ngl_game_src *g, int n, char *dir)
{
    char sys[7] = "";
    int i;

    strcpy(dir, LB_FOLDER);
    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = g->ops->entry(g->ctx, i);

        if (!e || !lb_ci_pre(e->path, "system/"))
            continue;
        if (!sys[0])
            memcpy(sys, e->path, 6);
        if (lb_ci_pre(e->path + 7, "libs") && (e->path[11] == '/' || !e->path[11])) {
            memcpy(dir, e->path, 11);
            return;
        }
    }
    if (sys[0])
        memcpy(dir, sys, 6);
}

static int lb_take(struct ngl_libs *o, const struct lb_ref *refs, int nref, const char *dir)
{
    int i, j, m;

    for (i = 0; i < nref; i++) {
        const struct ngl_entry *e;

        for (j = 0; j < o->n && !lb_ci_eq(lb_base(o->extra[j].path), refs[i].name); j++)
            ;
        if (j < o->n) {
            o->ord[j] = refs[i].ord > o->ord[j] ? refs[i].ord : o->ord[j];
            continue;
        }
        if (lb_zip_has(&o->base, o->nbase, refs[i].name) || (o->rom && ngl_romdir_has(o->rom, refs[i].name)))
            continue;
        m = lb_src_find(&o->lib, refs[i].name);
        if (m < 0)
            continue;
        if (o->n >= NGL_LIBS_MAX)
            return 1;
        e = o->lib.ops->entry(o->lib.ctx, m);
        snprintf(o->extra[o->n].path, sizeof o->extra[o->n].path, "%s/%s", dir, e->path);
        o->extra[o->n].size = e->size;
        o->extra[o->n].is_dir = 0;
        o->from[o->n] = m;
        o->ord[o->n] = refs[i].ord;
        o->n++;
    }
    return 0;
}

static int lb_count(void *ctx)
{
    const struct ngl_libs *l = ctx;

    return l->nbase + l->n;
}

static const struct ngl_entry *lb_entry(void *ctx, int idx)
{
    const struct ngl_libs *l = ctx;

    return idx < l->nbase ? l->base.ops->entry(l->base.ctx, idx) : &l->extra[idx - l->nbase];
}

static int lb_extract(void *ctx, int idx, const struct ngl_sink *sink, uint64_t off)
{
    const struct ngl_libs *l = ctx;

    return idx < l->nbase ? l->base.ops->extract(l->base.ctx, idx, sink, off)
                          : l->lib.ops->extract(l->lib.ctx, l->from[idx - l->nbase], sink, off);
}

static const struct ngl_src_ops lb_ops = { lb_count, lb_entry, lb_extract };

static int lb_dry_write(void *ctx, uint64_t off, const void *data, uint32_t len)
{
    (void)off;
    (void)data;
    (void)len;
    *(int *)ctx = 1;
    return -1;
}

static int lb_fits(const struct ngl_libs *o, const struct ngl_sel *sel)
{
    static const struct ngl_sel no_alias;
    struct ngl_alias *al = malloc(sizeof *al);
    struct ngl_sorted sv;
    struct ngl_report rep;
    int laid = 0;
    struct ngl_sink sk = { lb_dry_write, &laid };

    if (!al)
        return -1;
    // the builder writes its first byte only once the real layout (alias copies, every folder) fits
    if (!ngl_alias_wrap(&o->src, sel ? sel : &no_alias, al) && !ngl_sorted_view(&al->src, &sv)) {
        ngl_fat_build(sv.src.ops, sv.src.ctx, &sk, &rep);
        ngl_sorted_free(&sv);
    }
    free(al);
    return laid;
}

int ngl_libs_wrap(const struct ngl_game_src *game, const struct ngl_game_src *libs, const struct ngl_romdir *rom,
                  const struct ngl_sel *sel, struct ngl_libs *out)
{
    struct lb_work *w;
    char dir[16];
    uint32_t exports;
    int i, k, nref, over = 0, fit;

    memset(out, 0, sizeof *out);
    out->base = *game;
    out->src = *game;
    out->rom = rom;
    out->nbase = game->ops->count(game->ctx);
    if (!rom)
        snprintf(out->rom_why, sizeof out->rom_why, "rom: none");
    else if (rom->err[0])
        snprintf(out->rom_why, sizeof out->rom_why, "rom: %s", rom->err);
    else
        snprintf(out->rom_why, sizeof out->rom_why, "rom: %d names", rom->n);
    if (!libs) {
        snprintf(out->why, sizeof out->why, "no source");
        return 0;
    }
    // without the ROM's list a name the ROM holds could be added and change a working game's image
    if (rom && rom->err[0]) {
        snprintf(out->why, sizeof out->why, "skipped (rom: %s)", rom->err);
        return 0;
    }
    out->lib = *libs;
    w = malloc(sizeof *w);
    if (!w) {
        snprintf(out->why, sizeof out->why, "skipped (out of memory)");
        return 0;
    }
    lb_folder(game, out->nbase, dir);
    for (i = 0; i < out->nbase && !over; i++) {
        const struct ngl_entry *e = game->ops->entry(game->ctx, i);

        if (e && !e->is_dir && lb_image_name(e->path) && !lb_scan(game, i, e->size, w, &nref, &exports))
            over = lb_take(out, w->refs, nref, dir);
    }
    for (k = 0; k < out->n && !over; k++)
        if (!lb_scan(libs, out->from[k], out->extra[k].size, w, &nref, &exports)) {
            out->exports[k] = exports;
            over = lb_take(out, w->refs, nref, dir);
        }
    free(w);
    if (over) {
        snprintf(out->why, sizeof out->why, "skipped (more than %d libs)", NGL_LIBS_MAX);
        out->n = 0;
    } else if (!out->n) {
        snprintf(out->why, sizeof out->why, "none needed");
    } else {
        out->src.ops = &lb_ops;
        out->src.ctx = out;
        fit = lb_fits(out, sel);
        if (fit <= 0) {
            snprintf(out->why, sizeof out->why, fit ? "skipped (out of memory)" : "skipped (image would not fit)");
            out->n = 0;
            out->src = *game;
        }
    }
    return out->n;
}

void ngl_libs_line(const struct ngl_libs *l, char *buf, size_t cap)
{
    size_t o;
    int k;

    if (!l->n) {
        snprintf(buf, cap, "libs: %s", l->why);
        return;
    }
    o = (size_t)snprintf(buf, cap, "libs: added %d: ", l->n);
    for (k = 0; k < l->n && o < cap; k++)
        o += (size_t)snprintf(buf + o, cap - o, "%s%s", k ? ", " : "", lb_base(l->extra[k].path));
}
