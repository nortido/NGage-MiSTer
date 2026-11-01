// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// A build that includes miniz.h first defines this; Main finds its own miniz.h by -I.
#ifndef NGL_MINIZ_PRELOADED
#include "miniz.h"
#endif

#include "ngl_src.h"

struct ngl_src {
    int is_zip;
    char root[512];
    struct ngl_entry *e;
    int n, cap;
    int *zidx;
    mz_zip_archive zip;
};

struct zcb {
    const struct ngl_sink *sink;
    uint64_t base;
    uint32_t size;
};

static int add_entry(struct ngl_src *s, const char *path, uint32_t size, int is_dir, char *err, int errlen)
{
    struct ngl_entry *e;

    if (strlen(path) >= sizeof e->path) {
        snprintf(err, (size_t)errlen, "path too long: %.60s", path);
        return -1;
    }
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        struct ngl_entry *ne = realloc(s->e, (size_t)cap * sizeof *ne);
        int *nz = s->is_zip ? realloc(s->zidx, (size_t)cap * sizeof *nz) : s->zidx;
        if (ne)
            s->e = ne;
        if (nz)
            s->zidx = nz;
        if (!ne || (s->is_zip && !nz)) {
            snprintf(err, (size_t)errlen, "out of memory");
            return -1;
        }
        s->cap = cap;
    }
    e = &s->e[s->n++];
    memset(e, 0, sizeof *e);
    strcpy(e->path, path);
    e->size = size;
    e->is_dir = (uint8_t)is_dir;
    return 0;
}

static int scan_dir(struct ngl_src *s, const char *rel, char *err, int errlen)
{
    char dirpath[1024];
    DIR *d;
    struct dirent *de;
    int rc = 0;

    snprintf(dirpath, sizeof dirpath, "%s%s%s", s->root, rel[0] ? "/" : "", rel);
    d = opendir(dirpath);
    if (!d) {
        snprintf(err, (size_t)errlen, "cannot open %.80s", dirpath);
        return -1;
    }
    while (!rc && (de = readdir(d))) {
        char path[1100], full[1700];
        struct stat st;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
        if (strlen(path) >= sizeof s->e->path) {
            snprintf(err, (size_t)errlen, "path too long: %.60s", path);
            rc = -1;
            break;
        }
        snprintf(full, sizeof full, "%s/%s", s->root, path);
        if (stat(full, &st) != 0) {
            snprintf(err, (size_t)errlen, "cannot stat %.80s", full);
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = add_entry(s, path, 0, 1, err, errlen);
            if (!rc)
                rc = scan_dir(s, path, err, errlen);
        } else if (S_ISREG(st.st_mode)) {
            if ((uint64_t)st.st_size > 0xffffffffu) {
                snprintf(err, (size_t)errlen, "file too large: %.60s", path);
                rc = -1;
            } else {
                rc = add_entry(s, path, (uint32_t)st.st_size, 0, err, errlen);
            }
        }
    }
    closedir(d);
    return rc;
}

static int scan_top(struct ngl_src *s, char *err, int errlen)
{
    DIR *d = opendir(s->root);
    struct dirent *de;
    int rc = 0;

    if (!d) {
        snprintf(err, (size_t)errlen, "cannot open %.80s", s->root);
        return -1;
    }
    while (!rc && (de = readdir(d))) {
        char full[1700];
        struct stat st;

        // a libs folder: subfolders and entries that cannot hold a library are no error
        if (strlen(de->d_name) >= sizeof s->e->path)
            continue;
        snprintf(full, sizeof full, "%s/%s", s->root, de->d_name);
        if (stat(full, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > 0xffffffffu)
            continue;
        rc = add_entry(s, de->d_name, (uint32_t)st.st_size, 0, err, errlen);
    }
    closedir(d);
    return rc;
}

static int path_ok(const char *p)
{
    if (!*p)
        return 0;
    for (;;) {
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!p[n])
            return 1;
        p += n + 1;
    }
}

static const uint16_t cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static int utf8_valid(const unsigned char *s)
{
    while (*s) {
        unsigned c = *s;
        int len = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c < 0xf5 ? 4 : 0;
        unsigned cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;

        if (!len)
            return 0;
        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (s[i] & 0x3f);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp < 0xe000))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))
            return 0;
        s += len;
    }
    return 1;
}

static int cp437_to_utf8(const char *in, char *out, size_t outlen)
{
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        uint16_t cp = *p < 0x80 ? *p : cp437_hi[*p - 0x80];
        size_t k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;

        if (o + k >= outlen)
            return -1;
        if (k == 1) {
            out[o++] = (char)cp;
        } else if (k == 2) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xe0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = 0;
    return 0;
}

static int scan_zip(struct ngl_src *s, char *err, int errlen)
{
    mz_uint total = mz_zip_reader_get_num_files(&s->zip);

    for (mz_uint i = 0; i < total; i++) {
        mz_zip_archive_file_stat st;
        char name[512];
        size_t len;
        int dir;

        if (!mz_zip_reader_file_stat(&s->zip, i, &st)) {
            snprintf(err, (size_t)errlen, "not a valid zip: bad member %u", (unsigned)i);
            return -1;
        }
        // Zip names without the UTF-8 flag are CP437 by spec.
        if (!(st.m_bit_flag & 0x800) && !utf8_valid((const unsigned char *)st.m_filename)) {
            if (cp437_to_utf8(st.m_filename, name, sizeof name)) {
                snprintf(err, (size_t)errlen, "path too long: %.60s", st.m_filename);
                return -1;
            }
        } else {
            snprintf(name, sizeof name, "%s", st.m_filename);
        }
        len = strlen(name);
        dir = len && name[len - 1] == '/';
        if (dir)
            name[len - 1] = 0;
        if (!path_ok(name)) {
            snprintf(err, (size_t)errlen, "bad path in zip: %.60s", st.m_filename);
            return -1;
        }
        if (st.m_uncomp_size > 0xffffffffu) {
            snprintf(err, (size_t)errlen, "file too large: %.60s", name);
            return -1;
        }
        if (add_entry(s, name, dir ? 0 : (uint32_t)st.m_uncomp_size, dir, err, errlen))
            return -1;
        s->zidx[s->n - 1] = (int)i;
    }
    return 0;
}

struct sortitem {
    struct ngl_entry e;
    int zi;
};

static int cmp_item(const void *a, const void *b)
{
    return strcmp(((const struct sortitem *)a)->e.path, ((const struct sortitem *)b)->e.path);
}

static int sort_entries(struct ngl_src *s, char *err, int errlen)
{
    struct sortitem *it = malloc((size_t)(s->n ? s->n : 1) * sizeof *it);

    if (!it) {
        snprintf(err, (size_t)errlen, "out of memory");
        return -1;
    }
    for (int i = 0; i < s->n; i++) {
        it[i].e = s->e[i];
        it[i].zi = s->is_zip ? s->zidx[i] : 0;
    }
    qsort(it, (size_t)s->n, sizeof *it, cmp_item);
    for (int i = 0; i < s->n; i++) {
        s->e[i] = it[i].e;
        if (s->is_zip)
            s->zidx[i] = it[i].zi;
    }
    free(it);
    for (int i = 1; i < s->n; i++)
        if (!strcmp(s->e[i - 1].path, s->e[i].path)) {
            snprintf(err, (size_t)errlen, "duplicate path: %.60s", s->e[i].path);
            return -1;
        }
    return 0;
}

void ngl_src_close(struct ngl_src *s)
{
    if (!s)
        return;
    if (s->is_zip)
        mz_zip_reader_end(&s->zip);
    free(s->e);
    free(s->zidx);
    free(s);
}

static struct ngl_src *src_open(const char *path, int top, char *err, int errlen)
{
    struct ngl_src *s;
    struct stat st;
    int rc;

    if (stat(path, &st) != 0) {
        snprintf(err, (size_t)errlen, "cannot open %.80s", path);
        return NULL;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, (size_t)errlen, "out of memory");
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        size_t n = strlen(path);
        if (n >= sizeof s->root) {
            snprintf(err, (size_t)errlen, "path too long: %.60s", path);
            free(s);
            return NULL;
        }
        memcpy(s->root, path, n + 1);
        while (n > 1 && s->root[n - 1] == '/')
            s->root[--n] = 0;
        rc = top ? scan_top(s, err, errlen) : scan_dir(s, "", err, errlen);
    } else {
        s->is_zip = 1;
        if (!mz_zip_reader_init_file(&s->zip, path, 0)) {
            snprintf(err, (size_t)errlen, "not a valid zip: %.80s", path);
            s->is_zip = 0;
            free(s);
            return NULL;
        }
        rc = scan_zip(s, err, errlen);
    }
    if (!rc)
        rc = sort_entries(s, err, errlen);
    if (rc) {
        ngl_src_close(s);
        return NULL;
    }
    return s;
}

struct ngl_src *ngl_src_open(const char *path, char *err, int errlen)
{
    return src_open(path, 0, err, errlen);
}

struct ngl_src *ngl_src_open_top(const char *path, char *err, int errlen)
{
    return src_open(path, 1, err, errlen);
}

static int src_count(void *src)
{
    return ((struct ngl_src *)src)->n;
}

static const struct ngl_entry *src_entry(void *src, int idx)
{
    struct ngl_src *s = src;
    return idx >= 0 && idx < s->n ? &s->e[idx] : NULL;
}

static size_t zip_cb(void *op, mz_uint64 ofs, const void *buf, size_t n)
{
    struct zcb *c = op;

    if (ofs > c->size || n > c->size - ofs || c->sink->write(c->sink->ctx, c->base + ofs, buf, (uint32_t)n))
        return 0;
    return n;
}

static int extract_file(struct ngl_src *s, const struct ngl_entry *e, const struct ngl_sink *sink, uint64_t off)
{
    char full[1100];
    uint8_t buf[32768];
    uint32_t done = 0;
    int fd, rc = 0;

    snprintf(full, sizeof full, "%s/%s", s->root, e->path);
    fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    while (done < e->size) {
        uint32_t want = e->size - done < sizeof buf ? e->size - done : (uint32_t)sizeof buf;
        ssize_t n = read(fd, buf, want);
        if (n <= 0 || sink->write(sink->ctx, off + done, buf, (uint32_t)n)) {
            rc = -1;
            break;
        }
        done += (uint32_t)n;
    }
    close(fd);
    return rc;
}

static int src_extract(void *src, int idx, const struct ngl_sink *sink, uint64_t off)
{
    struct ngl_src *s = src;
    const struct ngl_entry *e = src_entry(src, idx);

    if (!e || e->is_dir)
        return -1;
    if (!s->is_zip)
        return extract_file(s, e, sink, off);
    {
        struct zcb c = {sink, off, e->size};
        return mz_zip_reader_extract_to_callback(&s->zip, (mz_uint)s->zidx[idx], zip_cb, &c, 0) ? 0 : -1;
    }
}

const struct ngl_src_ops ngl_src_ops = {src_count, src_entry, src_extract};

// Main treats the first ".zip" anywhere in a path as the archive boundary, so only a path whose first ".zip" is its own suffix names the archive itself.
int ngl_src_zip_path(const char *path)
{
    size_t n;
    const char *p;

    if (!path)
        return 0;
    n = strlen(path);
    if (n <= 4 || path[n - 5] == '/')
        return 0;
    for (p = path; p + 4 <= path + n; p++) {
        if (!strncasecmp(p, ".zip", 4))
            return p == path + n - 4;
    }
    return 0;
}
