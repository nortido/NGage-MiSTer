// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngl_fat.h"
#include "ngl_game.h"

#define TXT_NAME   "ngagegamestarter.txt"
#define APPS       "system/apps/"
#define TXT_MAX    4096u
#define E32_HEAD   0x68u
#define PATH_PRE   "E:\\System\\Apps\\"
#define PATH_PRE_N 15
#define CL_ROUND   2048u

static const char *const alias_sfx[] = { ".app", ".rsc", ".aif", "_caption.rsc" };

struct memsink {
    uint8_t *buf;
    uint32_t cap;
    uint32_t got;
    int stop_at_cap;
};

static int mem_write(void *ctx, uint64_t off, const void *data, uint32_t len)
{
    struct memsink *m = ctx;
    uint64_t end = off + len;

    if (end > m->cap) {
        if (m->stop_at_cap && off < m->cap) {
            memcpy(m->buf + off, data, m->cap - (uint32_t)off);
            m->got = m->cap;
        }
        return -1;
    }
    memcpy(m->buf + off, data, len);
    if (end > m->got)
        m->got = (uint32_t)end;
    return 0;
}

static int read_head(const struct ngl_game_src *s, int idx, uint8_t *buf, uint32_t cap, int stop_at_cap, uint32_t *got)
{
    struct memsink m = { buf, cap, 0, stop_at_cap };
    struct ngl_sink sk = { mem_write, &m };
    int rc = s->ops->extract(s->ctx, idx, &sk, 0);

    *got = m.got;
    return rc && !(stop_at_cap && m.got == cap) ? -1 : 0;
}

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static int ci_prefix(const char *s, const char *pre)
{
    size_t n = strlen(pre);
    char tmp[NGL_GAME_ERR];

    if (strlen(s) < n || n >= sizeof tmp)
        return 0;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    return ci_eq(tmp, pre);
}

static const struct ngl_entry *ent(const struct ngl_game_src *s, int i)
{
    return s->ops->entry(s->ctx, i);
}

static int find_file(const struct ngl_game_src *s, const char *path)
{
    int i, n = s->ops->count(s->ctx);

    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = ent(s, i);
        if (e && !e->is_dir && ci_eq(e->path, path))
            return i;
    }
    return -1;
}

static int app_candidate(const char *p, char *dir)
{
    const char *d, *slash, *f;
    size_t dl;

    if (!ci_prefix(p, APPS))
        return 0;
    d = p + sizeof APPS - 1;
    slash = strchr(d, '/');
    if (!slash || slash == d)
        return 0;
    dl = (size_t)(slash - d);
    f = slash + 1;
    if (dl >= NGL_GAME_NAME || strchr(f, '/') || strlen(f) != dl + 4)
        return 0;
    memcpy(dir, d, dl);
    dir[dl] = 0;
    {
        char want[NGL_GAME_NAME + 8];
        snprintf(want, sizeof want, "%s.app", dir);
        return ci_eq(f, want);
    }
}

static int e32_complete(const struct ngl_game_src *s, int idx, uint32_t size)
{
    uint8_t h[E32_HEAD];
    uint32_t got, code_size, code_off;

    if (size < E32_HEAD || read_head(s, idx, h, sizeof h, 1, &got) || got < E32_HEAD)
        return 0;
    if (memcmp(h + 0x10, "EPOC", 4))
        return 0;
    code_size = (uint32_t)h[0x30] | (uint32_t)h[0x31] << 8 | (uint32_t)h[0x32] << 16 | (uint32_t)h[0x33] << 24;
    code_off = (uint32_t)h[0x64] | (uint32_t)h[0x65] << 8 | (uint32_t)h[0x66] << 16 | (uint32_t)h[0x67] << 24;
    return (uint64_t)code_off + code_size <= size;
}

static void list_add(char *buf, size_t cap, const char *name)
{
    size_t n = strlen(buf);

    if (n + 1 < cap)
        snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", name);
}

static int fail(struct ngl_sel *sel, const char *msg)
{
    snprintf(sel->err, sizeof sel->err, "%s", msg);
    return -1;
}

static int pick_from_txt(const struct ngl_game_src *src, int txt_idx, struct ngl_sel *sel)
{
    uint8_t buf[TXT_MAX + 1];
    uint32_t got;
    const struct ngl_entry *te = ent(src, txt_idx);
    char *p, *end, value[160], shown[160];
    size_t vl = 0;
    int idx;
    const char *d, *slash;

    if (te->size > TXT_MAX)
        return fail(sel, TXT_NAME " is larger than 4096 bytes");
    if (read_head(src, txt_idx, buf, TXT_MAX, 0, &got))
        return fail(sel, "cannot read " TXT_NAME);
    buf[got] = 0;
    for (p = (char *)buf; *p;) {
        end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        if (end - p >= 5 && !strncmp(p, "Path:", 5)) {
            p += 5;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            while (end > p && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
                end--;
            vl = (size_t)(end - p);
            if (vl >= sizeof value)
                return fail(sel, TXT_NAME ": Path line is too long");
            memcpy(value, p, vl);
            value[vl] = 0;
            break;
        }
        p = *end ? end + 1 : end;
    }
    if (!vl)
        return fail(sel, TXT_NAME " has no Path: line");
    {
        size_t i;
        memcpy(shown, value, vl + 1);
        for (i = 0; i < vl; i++)
            if (value[i] == '\\')
                value[i] = '/';
    }
    {
        char full[sizeof value + 1];
        snprintf(full, sizeof full, "%s", value[0] == '/' ? value + 1 : value);
        d = full + sizeof APPS - 1;
        slash = ci_prefix(full, APPS) ? strchr(d, '/') : NULL;
        if (!slash || slash == d || strchr(slash + 1, '/') || !slash[1] ||
            (size_t)(slash - d) >= NGL_GAME_NAME || strlen(slash + 1) >= NGL_GAME_NAME) {
            snprintf(sel->err, sizeof sel->err, TXT_NAME ": Path is not \\system\\apps\\<dir>\\<file>");
            return -1;
        }
        idx = find_file(src, full);
        if (idx < 0) {
            snprintf(sel->err, sizeof sel->err, TXT_NAME ": %.150s is not in the source", shown);
            return -1;
        }
    }
    {
        const char *sp = ent(src, idx)->path + sizeof APPS - 1;
        const char *sl = strchr(sp, '/');
        memcpy(sel->dir, sp, (size_t)(sl - sp));
        sel->dir[sl - sp] = 0;
        snprintf(sel->app, sizeof sel->app, "%s", sl + 1);
    }
    return 0;
}

static int pick_by_header(const struct ngl_game_src *src, struct ngl_sel *sel)
{
    int i, n = src->ops->count(src->ctx), found = 0;
    char ok_list[NGL_GAME_ERR] = "", bad_list[NGL_GAME_ERR] = "";

    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = ent(src, i);
        char dir[NGL_GAME_NAME];

        if (!e || e->is_dir || !app_candidate(e->path, dir))
            continue;
        if (e32_complete(src, i, e->size)) {
            if (!found++) {
                snprintf(sel->dir, sizeof sel->dir, "%s", dir);
                snprintf(sel->app, sizeof sel->app, "%s", e->path + sizeof APPS - 1 + strlen(dir) + 1);
            }
            list_add(ok_list, sizeof ok_list, e->path + sizeof APPS - 1);
        } else {
            list_add(bad_list, sizeof bad_list, e->path + sizeof APPS - 1);
        }
    }
    if (found == 1)
        return 0;
    if (found > 1)
        snprintf(sel->err, sizeof sel->err, "several apps (%.200s), add " TXT_NAME, ok_list);
    else if (bad_list[0])
        snprintf(sel->err, sizeof sel->err, "no app with a complete E32 header (incomplete: %.150s), add " TXT_NAME, bad_list);
    else
        snprintf(sel->err, sizeof sel->err, "no app found in system/apps, add " TXT_NAME);
    return -1;
}

static int alias_taken(const struct ngl_game_src *src, const char *alias)
{
    char dirpath[NGL_GAME_NAME + 16];
    int i, n = src->ops->count(src->ctx);
    size_t l;

    snprintf(dirpath, sizeof dirpath, APPS "%s", alias);
    l = strlen(dirpath);
    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = ent(src, i);
        if (e && strlen(e->path) >= l && (e->path[l] == '/' || e->path[l] == 0) && ci_prefix(e->path, dirpath))
            return 1;
    }
    return 0;
}

static int make_alias(const struct ngl_game_src *src, struct ngl_sel *sel)
{
    size_t dl = strlen(sel->dir), len;
    char want[NGL_GAME_NAME + 8];

    snprintf(want, sizeof want, "%s.app", sel->dir);
    if (!ci_eq(sel->app, want)) {
        snprintf(sel->err, sizeof sel->err, "path is longer than 31 chars and %.60s is not %.60s.app", sel->app, sel->dir);
        return -1;
    }
    for (len = 2; len <= NGL_ALIAS_MAX && len < dl; len++) {
        memcpy(sel->alias, sel->dir, len);
        sel->alias[len] = 0;
        if (!alias_taken(src, sel->alias)) {
            snprintf(sel->path, sizeof sel->path, PATH_PRE "%s\\%s.app", sel->alias, sel->alias);
            return 0;
        }
    }
    sel->alias[0] = 0;
    snprintf(sel->err, sizeof sel->err, "no free alias folder for %.60s", sel->dir);
    return -1;
}

static void join_path(char *dst, const char *dir, const char *app)
{
    size_t dl = strlen(dir), al = strlen(app);

    memcpy(dst, PATH_PRE, PATH_PRE_N);
    memcpy(dst + PATH_PRE_N, dir, dl);
    dst[PATH_PRE_N + dl] = '\\';
    memcpy(dst + PATH_PRE_N + dl + 1, app, al + 1);
}

int ngl_game_select(const struct ngl_game_src *src, struct ngl_sel *sel)
{
    int txt, n = src->ops->count(src->ctx), i;

    memset(sel, 0, sizeof *sel);
    txt = -1;
    for (i = 0; i < n; i++) {
        const struct ngl_entry *e = ent(src, i);
        if (e && !e->is_dir && ci_eq(e->path, TXT_NAME)) {
            txt = i;
            break;
        }
    }
    if (txt >= 0 ? pick_from_txt(src, txt, sel) : pick_by_header(src, sel))
        return -1;
    if (PATH_PRE_N + strlen(sel->dir) + 1 + strlen(sel->app) <= NGL_SLOT_CHARS) {
        join_path(sel->path, sel->dir, sel->app);
        return 0;
    }
    return make_alias(src, sel);
}

static int al_count(void *ctx)
{
    struct ngl_alias *a = ctx;
    return a->nbase + a->n;
}

static const struct ngl_entry *al_entry(void *ctx, int idx)
{
    struct ngl_alias *a = ctx;
    return idx < a->nbase ? a->base.ops->entry(a->base.ctx, idx) : &a->extra[idx - a->nbase];
}

static int al_extract(void *ctx, int idx, const struct ngl_sink *sink, uint64_t off)
{
    struct ngl_alias *a = ctx;
    return a->base.ops->extract(a->base.ctx, idx < a->nbase ? idx : a->from[idx - a->nbase], sink, off);
}

static const struct ngl_src_ops alias_ops = { al_count, al_entry, al_extract };

static int ends_ci(const char *s, const char *sfx)
{
    size_t l = strlen(s), n = strlen(sfx);

    return l > n && ci_eq(s + l - n, sfx);
}

static int extra_has(const struct ngl_alias *out, const char *path)
{
    int k;

    for (k = 0; k < out->n; k++)
        if (ci_eq(out->extra[k].path, path))
            return 1;
    return 0;
}

static uint64_t clusters_of(uint32_t size)
{
    return ((uint64_t)size + CL_ROUND - 1) / CL_ROUND;
}

static int mirror_pass(const struct ngl_game_src *base, const struct ngl_sel *sel, struct ngl_alias *out, int nren, int small, uint64_t *clusters)
{
    char pre[NGL_GAME_NAME + 16];
    size_t pl;
    int i;

    snprintf(pre, sizeof pre, APPS "%s/", sel->dir);
    pl = strlen(pre);
    for (i = 0; i < out->nbase; i++) {
        const struct ngl_entry *e = ent(base, i);
        const char *rel;
        char path[sizeof e->path];
        int k, skip = 0;

        if (!e || !ci_prefix(e->path, pre) || !e->path[pl])
            continue;
        rel = e->path + pl;
        for (k = 0; k < nren; k++)
            skip |= out->from[k] == i;
        if (skip)
            continue;
        if (small && (e->is_dir || strchr(rel, '/') || !(ends_ci(rel, ".dll") || ends_ci(rel, ".exe"))))
            continue;
        snprintf(path, sizeof path, "%.*s%s/%s", (int)(sizeof APPS - 1), e->path, sel->alias, rel);
        if (extra_has(out, path))
            continue;
        if (out->n >= NGL_ALIAS_EXTRA)
            return 1;
        memcpy(out->extra[out->n].path, path, sizeof path);
        out->extra[out->n].size = e->size;
        out->extra[out->n].is_dir = e->is_dir;
        out->from[out->n] = i;
        out->n++;
        *clusters += clusters_of(e->size);
    }
    return 0;
}

static int dir_in_extra(const struct ngl_entry *e, const char *pre, size_t at)
{
    size_t l = strlen(e->path);

    return ci_prefix(e->path, pre) && ((l > at && e->path[at] == '/') || (l == at && e->is_dir));
}

static uint64_t mirror_dirs(const struct ngl_alias *out, size_t first)
{
    uint64_t dirs = 0;
    int j, k;

    for (k = 0; k < out->n; k++) {
        const char *p = out->extra[k].path;
        size_t at, len = strlen(p);

        for (at = first; at <= len; at++) {
            char pre[sizeof out->extra[k].path];

            if (p[at] != '/' && !(at == len && out->extra[k].is_dir))
                continue;
            memcpy(pre, p, at);
            pre[at] = 0;
            for (j = 0; j < k && !dir_in_extra(&out->extra[j], pre, at); j++)
                ;
            dirs += j == k;
        }
    }
    return dirs;
}

static void alias_one(const struct ngl_game_src *base, const struct ngl_sel *sel, struct ngl_alias *out, const char *sfx, uint64_t *total)
{
    char from[NGL_GAME_NAME * 2 + 32];
    int idx;
    const struct ngl_entry *e;

    snprintf(from, sizeof from, APPS "%s/%s%s", sel->dir, sel->dir, sfx);
    idx = find_file(base, from);
    if (idx < 0)
        return;
    e = base->ops->entry(base->ctx, idx);
    // keep the source spelling of System/Apps: the FAT merge compares directory names exactly
    snprintf(out->extra[out->n].path, sizeof out->extra[out->n].path, "%.*s%s/%s%s", (int)(sizeof APPS - 1), e->path, sel->alias, sel->alias, sfx);
    out->extra[out->n].size = e->size;
    out->extra[out->n].is_dir = 0;
    out->from[out->n] = idx;
    out->n++;
    *total += clusters_of(e->size);
}

int ngl_alias_wrap(const struct ngl_game_src *base, const struct ngl_sel *sel, struct ngl_alias *out)
{
    size_t k;
    int i, nren, over;
    uint64_t total = 0;

    memset(out, 0, sizeof *out);
    out->base = *base;
    out->nbase = base->ops->count(base->ctx);
    if (!sel->alias[0]) {
        out->src = *base;
        return 0;
    }
    for (k = 0; k < sizeof alias_sfx / sizeof alias_sfx[0]; k++)
        alias_one(base, sel, out, alias_sfx[k], &total);
    // the guest opens <alias>.rNN and <alias>_caption.rNN: same case-insensitive match and lowercase spelling as .rsc
    for (k = 0; k < 200; k++) {
        char sfx[16];

        snprintf(sfx, sizeof sfx, "%s.r%02d", k >= 100 ? "_caption" : "", (int)(k % 100));
        alias_one(base, sel, out, sfx, &total);
    }
    if (!out->n)
        return -1;
    nren = out->n;
    for (i = 0; i < out->nbase; i++)
        total += clusters_of(ent(base, i)->size);
    // the whole folder is mirrored only when image and mirror fit; else the DLLs and EXEs next to the app
    over = mirror_pass(base, sel, out, nren, 0, &total);
    // a directory takes a cluster too: ngl_fat rejects an image that is a few clusters over
    if (!over)
        total += mirror_dirs(out, sizeof APPS - 1 + strlen(sel->alias));
    if (over || total > NGL_FAT_CLUSTERS) {
        out->n = nren;
        mirror_pass(base, sel, out, nren, 1, &total);
    }
    out->src.ops = &alias_ops;
    out->src.ctx = out;
    return 0;
}

static int sv_count(void *ctx)
{
    return ((const struct ngl_sorted *)ctx)->n;
}

static const struct ngl_entry *sv_entry(void *ctx, int i)
{
    const struct ngl_sorted *s = ctx;
    return i >= 0 && i < s->n ? s->in.ops->entry(s->in.ctx, s->idx[i]) : NULL;
}

static int sv_extract(void *ctx, int i, const struct ngl_sink *sink, uint64_t off)
{
    const struct ngl_sorted *s = ctx;
    return i >= 0 && i < s->n ? s->in.ops->extract(s->in.ctx, s->idx[i], sink, off) : -1;
}

static const struct ngl_src_ops sv_ops = { sv_count, sv_entry, sv_extract };

// ngl_alias_wrap appends the alias files after the sorted source; ngl_fat_build needs strcmp order
int ngl_sorted_view(const struct ngl_game_src *in, struct ngl_sorted *out)
{
    int i, j;

    memset(out, 0, sizeof *out);
    out->in = *in;
    out->n = in->ops->count(in->ctx);
    if (out->n < 0)
        return -1;
    out->idx = malloc((size_t)(out->n > 0 ? out->n : 1) * sizeof out->idx[0]);
    if (!out->idx)
        return -1;
    for (i = 0; i < out->n; i++) {
        const char *p = in->ops->entry(in->ctx, i)->path;

        for (j = i; j > 0 && strcmp(in->ops->entry(in->ctx, out->idx[j - 1])->path, p) > 0; j--)
            out->idx[j] = out->idx[j - 1];
        out->idx[j] = i;
    }
    out->src.ops = &sv_ops;
    out->src.ctx = out;
    return 0;
}

void ngl_sorted_free(struct ngl_sorted *s)
{
    free(s->idx);
    s->idx = NULL;
    s->n = 0;
}
