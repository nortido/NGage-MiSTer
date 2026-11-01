// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ngl_hw.h"

static void dry_line(struct ngl_dry *d, const char *fmt, ...)
{
    char l[160];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(l, sizeof l - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof l - 1 || d->len + (size_t)n + 2 > d->cap) {
        d->overflow = 1;
        return;
    }
    memcpy(d->buf + d->len, l, (size_t)n);
    d->len += (size_t)n;
    d->buf[d->len++] = '\n';
    d->buf[d->len] = 0;
}

static struct ngl_dry_word *dry_word(const struct ngl_dry *d, uint32_t pa)
{
    int i;
    for (i = 0; i < d->nw; i++)
        if (d->w[i].pa == pa)
            return (struct ngl_dry_word *)&d->w[i];
    return NULL;
}

void ngl_dry_set(struct ngl_dry *d, uint32_t pa, uint32_t val)
{
    struct ngl_dry_word *w = dry_word(d, pa);

    if (!w) {
        if (d->nw >= (int)(sizeof d->w / sizeof d->w[0])) {
            d->overflow = 1;
            return;
        }
        w = &d->w[d->nw++];
        w->pa = pa;
    }
    w->val = val;
}

uint32_t ngl_dry_get(const struct ngl_dry *d, uint32_t pa)
{
    const struct ngl_dry_word *w = dry_word(d, pa);
    return w ? w->val : 0;
}

int ngl_dry_file(struct ngl_dry *d, const char *path, const char *val)
{
    int i;

    if (strlen(path) >= sizeof d->f[0].path || strlen(val) >= sizeof d->f[0].val)
        return -1;
    for (i = 0; i < d->nf; i++)
        if (!strcmp(d->f[i].path, path))
            break;
    if (i == d->nf) {
        if (d->nf >= (int)(sizeof d->f / sizeof d->f[0]))
            return -1;
        d->nf++;
        strcpy(d->f[i].path, path);
    }
    strcpy(d->f[i].val, val);
    return 0;
}

static int dry_sys_read(void *c, const char *path, char *buf, int len)
{
    struct ngl_dry *d = c;
    int i;
    size_t n;

    dry_line(d, "read %s", path);
    for (i = 0; i < d->nf; i++)
        if (!strcmp(d->f[i].path, path))
            break;
    if (i == d->nf || len < 1)
        return -2;
    n = strlen(d->f[i].val);
    if (n > (size_t)len - 1)
        n = (size_t)len - 1;
    memcpy(buf, d->f[i].val, n);
    buf[n] = 0;
    return (int)n;
}

static int dry_sys_write(void *c, const char *path, const char *val)
{
    struct ngl_dry *d = c;
    dry_line(d, "write %s %s", path, val);
    return ngl_dry_file(d, path, val) ? -28 : 0;
}

static uint32_t dry_rd32(void *c, uint32_t pa)
{
    struct ngl_dry *d = c;
    dry_line(d, "rd32 0x%08X", (unsigned)pa);
    return ngl_dry_get(d, pa);
}

static int dry_wr32(void *c, uint32_t pa, uint32_t val)
{
    struct ngl_dry *d = c;
    dry_line(d, "wr32 0x%08X 0x%08X", (unsigned)pa, (unsigned)val);
    ngl_dry_set(d, pa, val);
    return 0;
}

static int dry_mem_read(void *c, uint32_t pa, void *buf, uint32_t len)
{
    struct ngl_dry *d = c;
    uint8_t *b = buf;
    int i, k;

    dry_line(d, "memrd 0x%08X %u", (unsigned)pa, (unsigned)len);
    memset(buf, 0, len);
    for (i = 0; i < d->nw; i++)
        for (k = 0; k < 4; k++)
            if (d->w[i].pa + (uint32_t)k >= pa && d->w[i].pa + (uint32_t)k - pa < len)
                b[d->w[i].pa + (uint32_t)k - pa] = (uint8_t)(d->w[i].val >> (8 * k));
    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int dry_mem_write(void *c, uint32_t pa, const void *buf, uint32_t len)
{
    struct ngl_dry *d = c;
    const uint8_t *b = buf;

    // the dry store keeps only the first and last word: the library reads back nothing else
    dry_line(d, "memwr 0x%08X %u", (unsigned)pa, (unsigned)len);
    if (len >= 4) {
        ngl_dry_set(d, pa, le32(b));
        ngl_dry_set(d, pa + (len & ~3u) - 4, le32(b + (len & ~3u) - 4));
    }
    return 0;
}

static void dry_sleep(void *c, uint32_t ms)
{
    dry_line(c, "sleep %u", (unsigned)ms);
}

static int dry_affinity(void *c, int cpu)
{
    dry_line(c, "affinity %d", cpu);
    return 0;
}

static void dry_log(void *c, const char *msg)
{
    dry_line(c, "log %s", msg);
}

void ngl_dry_init(struct ngl_dry *d, struct ngl_io *io, char *buf, size_t cap)
{
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->cap = cap;
    if (cap)
        buf[0] = 0;
    io->sys_read = dry_sys_read;
    io->sys_write = dry_sys_write;
    io->rd32 = dry_rd32;
    io->wr32 = dry_wr32;
    io->mem_read = dry_mem_read;
    io->mem_write = dry_mem_write;
    io->sleep_ms = dry_sleep;
    io->set_affinity = dry_affinity;
    io->log = dry_log;
    io->ctx = d;
}

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int hw_page(struct ngl_hw *h, uint32_t pa)
{
    uint32_t pg = pa & ~0xFFFu;
    void *m;

    if (h->map && h->page == pg)
        return 0;
    if (h->map) {
        munmap((void *)h->map, 4096);
        h->map = NULL;
    }
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, (off_t)pg);
    if (m == MAP_FAILED)
        return -1;
    h->map = m;
    h->page = pg;
    return 0;
}

static uint32_t hw_rd32(void *c, uint32_t pa)
{
    struct ngl_hw *h = c;
    // a failed map reads as all ones: bridges in reset, no magic, no live state
    if ((pa & 3u) || hw_page(h, pa))
        return 0xFFFFFFFFu;
    return h->map[(pa & 0xFFFu) >> 2];
}

static int hw_wr32(void *c, uint32_t pa, uint32_t val)
{
    struct ngl_hw *h = c;
    if ((pa & 3u) || hw_page(h, pa))
        return -1;
    h->map[(pa & 0xFFFu) >> 2] = val;
    return 0;
}

static int hw_mem_read(void *c, uint32_t pa, void *buf, uint32_t len)
{
    struct ngl_hw *h = c;
    uint8_t *b = buf;
    uint32_t i, v;

    // word accesses only: the O_SYNC mapping is device memory, where memcpy may fault
    if ((pa & 3u) || (len & 3u))
        return -1;
    for (i = 0; i < len; i += 4) {
        if (hw_page(h, pa + i))
            return -1;
        v = h->map[((pa + i) & 0xFFFu) >> 2];
        memcpy(b + i, &v, 4);
    }
    return 0;
}

static int hw_mem_write(void *c, uint32_t pa, const void *buf, uint32_t len)
{
    struct ngl_hw *h = c;
    const uint8_t *b = buf;
    uint32_t i, v;

    if ((pa & 3u) || (len & 3u))
        return -1;
    for (i = 0; i < len; i += 4) {
        if (hw_page(h, pa + i))
            return -1;
        memcpy(&v, b + i, 4);
        h->map[((pa + i) & 0xFFFu) >> 2] = v;
    }
    return 0;
}

static int hw_sys_read(void *c, const char *path, char *buf, int len)
{
    int fd, e;
    ssize_t n;

    (void)c;
    if (len < 1)
        return -EINVAL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    n = read(fd, buf, (size_t)len - 1);
    e = errno;
    close(fd);
    if (n < 0)
        return -e;
    buf[n] = 0;
    return (int)n;
}

static int hw_sys_write(void *c, const char *path, const char *val)
{
    size_t l = strlen(val);
    ssize_t n;
    int fd, e;

    (void)c;
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    n = write(fd, val, l);
    e = errno;
    close(fd);
    if (n < 0)
        return -e;
    return (size_t)n == l ? 0 : -EIO;
}

static void hw_sleep(void *c, uint32_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };

    (void)c;
    while (nanosleep(&ts, &ts) && errno == EINTR)
        ;
}

static int hw_affinity(void *c, int cpu)
{
    cpu_set_t s;

    (void)c;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof s, &s) ? -errno : 0;
}

static void hw_log(void *c, const char *msg)
{
    (void)c;
    fprintf(stderr, "ngl: %s\n", msg);
}

int ngl_hw_open(struct ngl_hw *h, struct ngl_io *io)
{
    h->map = NULL;
    h->page = 0;
    h->fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (h->fd < 0)
        return -errno;
    io->sys_read = hw_sys_read;
    io->sys_write = hw_sys_write;
    io->rd32 = hw_rd32;
    io->wr32 = hw_wr32;
    io->mem_read = hw_mem_read;
    io->mem_write = hw_mem_write;
    io->sleep_ms = hw_sleep;
    io->set_affinity = hw_affinity;
    io->log = hw_log;
    io->ctx = h;
    return 0;
}

void ngl_hw_close(struct ngl_hw *h)
{
    if (h->map)
        munmap((void *)h->map, 4096);
    h->map = NULL;
    if (h->fd >= 0)
        close(h->fd);
    h->fd = -1;
}
#endif
