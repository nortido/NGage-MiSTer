// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "ngl_fat.h"

static int file_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
    struct ngl_sink_file *f = ctx;
    const uint8_t *p = buf;

    if (off > NGL_IMG_BYTES || len > NGL_IMG_BYTES - off)
        return -1;
    while (len) {
        ssize_t n = pwrite(f->fd, p, len, (off_t)off);
        if (n <= 0)
            return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint32_t)n;
    }
    return 0;
}

int ngl_sink_file_open(struct ngl_sink_file *f, const char *path)
{
    f->fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (f->fd < 0)
        return -1;
    if (ftruncate(f->fd, (off_t)NGL_IMG_BYTES) < 0) {
        close(f->fd);
        return -1;
    }
    f->sink.write = file_write;
    f->sink.ctx = f;
    return 0;
}

void ngl_sink_file_close(struct ngl_sink_file *f)
{
    close(f->fd);
}

static int mem_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
    struct ngl_sink_mem *m = ctx;

    if (off > NGL_IMG_BYTES || len > NGL_IMG_BYTES - off)
        return -1;
    memcpy(m->buf + off, buf, len);
    m->writes++;
    return 0;
}

int ngl_sink_mem_init(struct ngl_sink_mem *m)
{
    m->buf = calloc(1, NGL_IMG_BYTES);
    if (!m->buf)
        return -1;
    m->writes = 0;
    m->sink.write = mem_write;
    m->sink.ctx = m;
    return 0;
}

void ngl_sink_mem_free(struct ngl_sink_mem *m)
{
    free(m->buf);
    m->buf = NULL;
}
