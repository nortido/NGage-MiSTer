// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_TYPES_H
#define NGL_TYPES_H

#include <stdint.h>

#define NGL_IMG_SECTORS 0x20000u
#define NGL_SLOT_CHARS  31
#define NGL_MON_PA      0x38000000u

struct ngl_entry {
    char path[200];
    uint32_t size;
    uint8_t is_dir;
};

struct ngl_sink {
    int (*write)(void *ctx, uint64_t off, const void *buf, uint32_t len);
    void *ctx;
};

struct ngl_src_ops {
    int (*count)(void *src);
    const struct ngl_entry *(*entry)(void *src, int idx);
    int (*extract)(void *src, int idx, const struct ngl_sink *sink, uint64_t off);
};

struct ngl_io {
    int (*sys_read)(void *ctx, const char *path, char *buf, int len);
    int (*sys_write)(void *ctx, const char *path, const char *val);
    uint32_t (*rd32)(void *ctx, uint32_t pa);
    int (*wr32)(void *ctx, uint32_t pa, uint32_t val);
    int (*mem_read)(void *ctx, uint32_t pa, void *buf, uint32_t len);
    int (*mem_write)(void *ctx, uint32_t pa, const void *buf, uint32_t len);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    int (*set_affinity)(void *ctx, int cpu);
    void (*log)(void *ctx, const char *msg);
    void *ctx;
};

#endif
