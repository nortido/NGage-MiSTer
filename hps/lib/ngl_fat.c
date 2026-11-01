// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngl_fat.h"

#define CL_BYTES   2048u
#define FAT_BYTES  (128u * 512u)
#define FAT_OFF    512u
#define ROOT_OFF   (257u * 512u)
#define ROOT_SLOTS 512u
#define DATA_OFF   (289u * 512u)
#define STAMP_DATE 0x2f47u
#define MAX_DEPTH  101

// boot code bytes from FreeBSD newfs_msdos, BSD-2-Clause, see third_party/LICENSE-newfs_msdos
static const uint8_t boot_head[145] = {
    0xeb, 0x3c, 0x90, 0x42, 0x53, 0x44, 0x20, 0x20, 0x34, 0x2e, 0x34, 0x00, 0x02, 0x04, 0x01, 0x00,
    0x02, 0x00, 0x02, 0x00, 0x00, 0xf0, 0x80, 0x00, 0x20, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x29, 0xf5, 0x14, 0xb4, 0xc4, 0x4e, 0x47, 0x41, 0x47, 0x45,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x46, 0x41, 0x54, 0x31, 0x36, 0x20, 0x20, 0x20, 0xfa, 0x31,
    0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0xfb, 0x8e, 0xd8, 0xe8, 0x00, 0x00, 0x5e, 0x83, 0xc6, 0x19,
    0xbb, 0x07, 0x00, 0xfc, 0xac, 0x84, 0xc0, 0x74, 0x06, 0xb4, 0x0e, 0xcd, 0x10, 0xeb, 0xf5, 0x30,
    0xe4, 0xcd, 0x16, 0xcd, 0x19, 0x0d, 0x0a, 0x4e, 0x6f, 0x6e, 0x2d, 0x73, 0x79, 0x73, 0x74, 0x65,
    0x6d, 0x20, 0x64, 0x69, 0x73, 0x6b, 0x0d, 0x0a, 0x50, 0x72, 0x65, 0x73, 0x73, 0x20, 0x61, 0x6e,
    0x79, 0x20, 0x6b, 0x65, 0x79, 0x20, 0x74, 0x6f, 0x20, 0x72, 0x65, 0x62, 0x6f, 0x6f, 0x74, 0x0d,
    0x0a,
};

struct node {
    int32_t parent, next, child, last;
    int32_t ent;
    uint32_t cluster, nclus, slots;
    const char *name;
    uint16_t nlen;
    uint8_t u16, is_dir, lfn, sset;
    uint8_t sn[11];
};

struct ctx {
    const struct ngl_src_ops *ops;
    void *src;
    const struct ngl_sink *sink;
    struct ngl_report *rep;
    int n;
    int extra;
    int nn;
    struct node *nd;
    uint8_t *fat;
};

struct dw {
    const struct ngl_sink *sink;
    uint64_t off;
    uint64_t left;
    uint32_t fill;
    int err;
    uint8_t buf[CL_BYTES];
};

struct win {
    const struct ngl_sink *inner;
    uint64_t lo, hi;
};

static int fail(struct ctx *c, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(c->rep->err, sizeof c->rep->err, fmt, ap);
    va_end(ap);
    return -1;
}

uint8_t ngl_fat_lfn_sum(const uint8_t name11[11])
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static int u8next(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint32_t c = s[0];
    int len, i;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xc2 && c < 0xe0) {
        len = 2;
        c &= 0x1f;
    } else if (c >= 0xe0 && c < 0xf0) {
        len = 3;
        c &= 0x0f;
    } else if (c >= 0xf0 && c < 0xf5) {
        len = 4;
        c &= 0x07;
    } else {
        return 0;
    }
    if ((size_t)len > n)
        return 0;
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return 0;
        c = (c << 6) | (s[i] & 0x3f);
    }
    if ((len == 3 && (c < 0x800 || (c >= 0xd800 && c < 0xe000))) || (len == 4 && (c < 0x10000 || c > 0x10ffff)))
        return 0;
    *cp = c;
    return len;
}

static int name_u16(const char *name, size_t len)
{
    size_t i = 0;
    int n = 0;

    if (len == 0 || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -1;
    while (i < len) {
        uint32_t cp;
        int k = u8next((const uint8_t *)name + i, len - i, &cp);
        if (!k || cp < 0x20 || cp == 0x7f || (cp < 0x80 && strchr("\"*:<>?\\|", (int)cp)))
            return -1;
        n += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return n > 255 ? -1 : n;
}

static int to_u16(const char *name, size_t len, uint16_t *out)
{
    size_t i = 0;
    int n = 0;

    while (i < len) {
        uint32_t cp;
        int k = u8next((const uint8_t *)name + i, len - i, &cp);
        i += (size_t)k;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (cp >> 10));
            out[n++] = (uint16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

static int short_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c > 0 && strchr("$%'-_@~!(){}^#&", c));
}

static int up(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int plain83(const char *name, size_t len, int fold)
{
    size_t dot = len, ext;

    for (size_t i = 0; i < len; i++)
        if (name[i] == '.') {
            if (dot != len)
                return 0;
            dot = i;
        }
    ext = dot == len ? 0 : len - dot - 1;
    if (dot == 0 || dot > 8 || ext > 3 || (dot != len && ext == 0))
        return 0;
    for (size_t i = 0; i < len; i++)
        if (i != dot && !short_char(fold ? up((unsigned char)name[i]) : (unsigned char)name[i]))
            return 0;
    return 1;
}

static void plain_short(const char *name, size_t len, uint8_t sn[11])
{
    size_t dot = len, i;

    memset(sn, ' ', 11);
    for (i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;
    for (i = 0; i < dot; i++)
        sn[i] = (uint8_t)up((unsigned char)name[i]);
    for (i = dot + 1; i < len; i++)
        sn[8 + i - dot - 1] = (uint8_t)up((unsigned char)name[i]);
}

static int tail_short(const char *name, size_t len, int tail, uint8_t sn[11])
{
    char base[8], ext[3], t[8];
    int nb = 0, ne = 0, tl, keep;
    size_t start = 0, dot = len, i = 0;

    while (start < len && (name[start] == '.' || name[start] == ' '))
        start++;
    for (i = start; i < len; i++)
        if (name[i] == '.')
            dot = i;
    for (i = start; i < len;) {
        uint32_t cp;
        size_t pos = i;
        int k = u8next((const uint8_t *)name + i, len - i, &cp);
        char ch;
        i += (size_t)k;
        if (pos == dot || cp == ' ' || cp == '.')
            continue;
        ch = cp < 0x80 && short_char(up((int)cp)) ? (char)up((int)cp) : '_';
        if (dot != len && pos > dot) {
            if (ne < 3)
                ext[ne++] = ch;
        } else if (nb < 8) {
            base[nb++] = ch;
        }
    }
    if (!nb) {
        base[0] = '_';
        nb = 1;
    }
    tl = snprintf(t, sizeof t, "~%d", tail);
    keep = 8 - tl;
    memset(sn, ' ', 11);
    memcpy(sn, base, (size_t)(nb < keep ? nb : keep));
    memcpy(sn + (nb < keep ? nb : keep), t, (size_t)tl);
    memcpy(sn + 8, ext, (size_t)ne);
    return 0;
}

static void link_child(struct ctx *c, int parent, int node)
{
    struct node *p = &c->nd[parent], *n = &c->nd[node];

    n->parent = parent;
    if (p->last < 0)
        p->child = node;
    else
        c->nd[p->last].next = node;
    p->last = node;
}

static void init_node(struct ctx *c, int node, int parent, const char *name, size_t len, int u16, int is_dir, int ent)
{
    struct node *n = &c->nd[node];

    memset(n, 0, sizeof *n);
    n->child = n->last = n->next = -1;
    n->ent = ent;
    n->name = name;
    n->nlen = (uint16_t)len;
    n->u16 = (uint8_t)u16;
    n->is_dir = (uint8_t)is_dir;
    n->lfn = !plain83(name, len, 1);
    link_child(c, parent, node);
}

static int find_entry(struct ctx *c, const char *path)
{
    int lo = 0, hi = c->n - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int d = strcmp(c->ops->entry(c->src, mid)->path, path);
        if (d == 0)
            return mid;
        if (d < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int link_all(struct ctx *c, int dry)
{
    int st_node[MAX_DEPTH];
    const char *st_name[MAX_DEPTH];
    size_t st_len[MAX_DEPTH];
    int sd = 0;
    int next = 1 + c->n;
    const char *prev = NULL;

    c->extra = 0;
    if (!dry) {
        memset(&c->nd[0], 0, sizeof c->nd[0]);
        c->nd[0].child = c->nd[0].last = c->nd[0].next = c->nd[0].parent = c->nd[0].ent = -1;
        c->nd[0].is_dir = 1;
    }
    for (int i = 0; i < c->n; i++) {
        const struct ngl_entry *e = c->ops->entry(c->src, i);
        const char *p = e->path;
        int parent = 0, depth = 0, matched = 1;

        if (!memchr(e->path, 0, sizeof e->path) || (prev && strcmp(prev, p) >= 0))
            return fail(c, "source not sorted: %.60s", p);
        prev = p;
        for (;;) {
            size_t len = strcspn(p, "/");
            int u16 = name_u16(p, len);
            if (u16 < 0)
                return fail(c, "bad name: %.60s", e->path);
            if (!p[len]) {
                if (!dry)
                    init_node(c, 1 + i, parent, p, len, u16, e->is_dir, i);
                break;
            }
            if (depth + 1 >= MAX_DEPTH)
                return fail(c, "path too deep: %.60s", e->path);
            if (matched && depth < sd && st_len[depth] == len && memcmp(st_name[depth], p, len) == 0) {
                parent = st_node[depth];
            } else {
                char pre[sizeof e->path];
                int ex;
                matched = 0;
                memcpy(pre, e->path, (size_t)(p - e->path) + len);
                pre[(p - e->path) + (long)len] = 0;
                ex = find_entry(c, pre);
                if (ex >= 0) {
                    if (!c->ops->entry(c->src, ex)->is_dir)
                        return fail(c, "path conflict: %.60s", pre);
                    parent = 1 + ex;
                } else if (dry) {
                    c->extra++;
                    parent = 0;
                } else {
                    init_node(c, next, parent, p, len, u16, 1, -1);
                    parent = next++;
                }
                st_node[depth] = parent;
                st_name[depth] = p;
                st_len[depth] = len;
            }
            depth++;
            p += len + 1;
        }
        sd = depth;
    }
    return 0;
}

static void emit_short(uint8_t *e, const uint8_t sn[11], int is_dir, uint32_t cluster, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = is_dir ? 0x10 : 0x20;
    put16(e + 16, STAMP_DATE);
    put16(e + 18, STAMP_DATE);
    put16(e + 24, STAMP_DATE);
    put16(e + 26, cluster);
    put32(e + 28, size);
}

static void dw_flush(struct dw *w)
{
    if (!w->err && w->sink->write(w->sink->ctx, w->off, w->buf, CL_BYTES))
        w->err = 1;
    w->off += CL_BYTES;
    w->left -= CL_BYTES;
    w->fill = 0;
    memset(w->buf, 0, CL_BYTES);
}

static void dw_init(struct dw *w, const struct ngl_sink *sink, uint64_t off, uint64_t total)
{
    memset(w, 0, sizeof *w);
    w->sink = sink;
    w->off = off;
    w->left = total;
}

static void dw_put(struct dw *w, const uint8_t *e)
{
    memcpy(w->buf + w->fill, e, 32);
    w->fill += 32;
    if (w->fill == CL_BYTES)
        dw_flush(w);
}

static int dw_end(struct dw *w)
{
    if (w->fill)
        dw_flush(w);
    while (w->left)
        dw_flush(w);
    return w->err ? -1 : 0;
}

static int sn_taken(struct ctx *c, int dir, const uint8_t sn[11])
{
    for (int k = c->nd[dir].child; k >= 0; k = c->nd[k].next)
        if (c->nd[k].sset && memcmp(c->nd[k].sn, sn, 11) == 0)
            return 1;
    return 0;
}

static int assign_shorts(struct ctx *c, int dir)
{
    for (int k = c->nd[dir].child; k >= 0; k = c->nd[k].next) {
        struct node *n = &c->nd[k];
        if (n->lfn)
            continue;
        plain_short(n->name, n->nlen, n->sn);
        if (sn_taken(c, dir, n->sn))
            n->lfn = 1;
        else
            n->sset = 1;
    }
    for (int k = c->nd[dir].child; k >= 0; k = c->nd[k].next) {
        struct node *n = &c->nd[k];
        if (n->sset)
            continue;
        for (int t = 1; t < 1000000; t++) {
            tail_short(n->name, n->nlen, t, n->sn);
            if (!sn_taken(c, dir, n->sn)) {
                n->sset = 1;
                break;
            }
        }
        if (!n->sset)
            return fail(c, "no short name: %.60s", n->name);
    }
    return 0;
}

static int settle_dirs(struct ctx *c)
{
    for (int k = 0; k < c->nn; k++) {
        struct node *d = &c->nd[k];
        if (!d->is_dir)
            continue;
        if (assign_shorts(c, k))
            return -1;
        d->slots = 0;
        for (int q = d->child; q >= 0; q = c->nd[q].next) {
            const struct node *n = &c->nd[q];
            d->slots += 1u + (n->lfn ? ((uint32_t)n->u16 + 12u) / 13u : 0u);
        }
    }
    return 0;
}

static int emit_dir(struct ctx *c, int dir)
{
    struct node *d = &c->nd[dir];
    struct dw w;
    uint8_t e[32];

    if (dir == 0) {
        dw_init(&w, c->sink, ROOT_OFF, ROOT_SLOTS * 32u);
        emit_short(e, (const uint8_t *)"NGAGE      ", 0, 0, 0);
        e[11] = 0x08;
        dw_put(&w, e);
    } else {
        uint32_t up_cl = d->parent == 0 ? 0 : c->nd[d->parent].cluster;
        dw_init(&w, c->sink, DATA_OFF + (uint64_t)(d->cluster - 2) * CL_BYTES, (uint64_t)d->nclus * CL_BYTES);
        emit_short(e, (const uint8_t *)".          ", 1, d->cluster, 0);
        dw_put(&w, e);
        emit_short(e, (const uint8_t *)"..         ", 1, up_cl, 0);
        dw_put(&w, e);
    }
    for (int k = d->child; k >= 0; k = c->nd[k].next) {
        struct node *n = &c->nd[k];
        uint32_t size = n->is_dir ? 0 : c->ops->entry(c->src, n->ent)->size;
        if (n->lfn) {
            uint16_t u[260];
            int len = to_u16(n->name, n->nlen, u);
            int cnt = (len + 12) / 13;
            uint8_t sum = ngl_fat_lfn_sum(n->sn);
            static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int q = cnt; q >= 1; q--) {
                memset(e, 0, 32);
                e[0] = (uint8_t)(q | (q == cnt ? 0x40 : 0));
                e[11] = 0x0f;
                e[13] = sum;
                for (int j = 0; j < 13; j++) {
                    int idx = (q - 1) * 13 + j;
                    put16(e + pos[j], idx < len ? u[idx] : idx == len ? 0x0000u : 0xffffu);
                }
                dw_put(&w, e);
            }
        }
        emit_short(e, n->sn, n->is_dir, n->cluster, size);
        dw_put(&w, e);
    }
    return dw_end(&w) ? fail(c, "sink write failed") : 0;
}

static int win_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
    struct win *w = ctx;

    if (off < w->lo || off > w->hi || len > w->hi - off)
        return -1;
    return w->inner->write(w->inner->ctx, off, buf, len);
}

static int emit_file(struct ctx *c, int i, const struct node *n)
{
    const struct ngl_entry *e = c->ops->entry(c->src, i);
    uint64_t off = DATA_OFF + (uint64_t)(n->cluster - 2) * CL_BYTES;
    uint64_t end = off + (uint64_t)n->nclus * CL_BYTES;
    struct win w = {c->sink, off, end};
    struct ngl_sink ws = {win_write, &w};
    uint8_t pad[CL_BYTES];

    if (c->ops->extract(c->src, i, &ws, off))
        return fail(c, "cannot read %.60s", e->path);
    memset(pad, 0, sizeof pad);
    if (off + e->size < end && c->sink->write(c->sink->ctx, off + e->size, pad, (uint32_t)(end - off - e->size)))
        return fail(c, "sink write failed");
    return 0;
}

static int layout(struct ctx *c)
{
    uint32_t next = 2;
    uint64_t total = 0;

    if (c->nd[0].slots + 1 > ROOT_SLOTS)
        return fail(c, "root directory full");
    for (int k = 1; k < c->nn; k++) {
        struct node *n = &c->nd[k];
        if (!n->is_dir)
            continue;
        n->nclus = ((n->slots + 2) * 32u + CL_BYTES - 1) / CL_BYTES;
        n->cluster = next;
        next += n->nclus;
        total += n->nclus;
        c->rep->dirs++;
    }
    for (int i = 0; i < c->n; i++) {
        struct node *n = &c->nd[1 + i];
        uint32_t size;
        if (n->is_dir)
            continue;
        size = c->ops->entry(c->src, i)->size;
        n->nclus = (uint32_t)(((uint64_t)size + CL_BYTES - 1) / CL_BYTES);
        n->cluster = n->nclus ? next : 0;
        next += n->nclus;
        total += n->nclus;
        c->rep->files++;
        c->rep->bytes += size;
    }
    if (total > NGL_FAT_CLUSTERS)
        return fail(c, "does not fit (%llu KiB over)", (unsigned long long)(total - NGL_FAT_CLUSTERS) * 2);
    c->rep->clusters = (uint32_t)total;
    return 0;
}

static void fill_fat(struct ctx *c)
{
    put16(c->fat, 0xfff0);
    put16(c->fat + 2, 0xffff);
    for (int k = 1; k < c->nn; k++) {
        const struct node *n = &c->nd[k];
        for (uint32_t q = 0; q < n->nclus; q++)
            put16(c->fat + (n->cluster + q) * 2, q + 1 == n->nclus ? 0xffffu : n->cluster + q + 1);
    }
}

int ngl_fat_build(const struct ngl_src_ops *ops, void *src, const struct ngl_sink *sink, struct ngl_report *rep)
{
    struct ctx c;
    uint8_t *block;
    uint8_t boot[512];
    int rc = -1;

    memset(rep, 0, sizeof *rep);
    memset(&c, 0, sizeof c);
    c.ops = ops;
    c.src = src;
    c.sink = sink;
    c.rep = rep;
    c.n = ops->count(src);
    if (link_all(&c, 1))
        return -1;
    c.nn = 1 + c.n + c.extra;
    block = calloc(1, (size_t)c.nn * sizeof(struct node) + FAT_BYTES);
    if (!block)
        return fail(&c, "out of memory");
    c.nd = (struct node *)block;
    c.fat = block + (size_t)c.nn * sizeof(struct node);
    if (link_all(&c, 0) || settle_dirs(&c) || layout(&c))
        goto out;
    fill_fat(&c);
    memset(boot, 0, sizeof boot);
    memcpy(boot, boot_head, sizeof boot_head);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    if (sink->write(sink->ctx, 0, boot, sizeof boot)) {
        fail(&c, "sink write failed");
        goto out;
    }
    for (int k = 0; k < c.nn; k++)
        if (c.nd[k].is_dir && emit_dir(&c, k))
            goto out;
    for (int i = 0; i < c.n; i++)
        if (!c.nd[1 + i].is_dir && c.nd[1 + i].nclus && emit_file(&c, i, &c.nd[1 + i]))
            goto out;
    if (sink->write(sink->ctx, FAT_OFF, c.fat, FAT_BYTES) || sink->write(sink->ctx, FAT_OFF + FAT_BYTES, c.fat, FAT_BYTES)) {
        fail(&c, "sink write failed");
        goto out;
    }
    rc = 0;
out:
    free(block);
    return rc;
}
