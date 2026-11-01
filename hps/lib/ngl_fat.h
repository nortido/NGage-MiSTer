// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_FAT_H
#define NGL_FAT_H

#include <stdint.h>

#include "ngl_types.h"

#define NGL_IMG_BYTES    ((uint64_t)NGL_IMG_SECTORS * 512u)
#define NGL_FAT_CLUSTERS 32695u

struct ngl_report {
    uint32_t files;
    uint32_t dirs;
    uint32_t clusters;
    uint64_t bytes;
    char err[96];
};

int ngl_fat_build(const struct ngl_src_ops *ops, void *src, const struct ngl_sink *sink,
                  struct ngl_report *rep);
uint8_t ngl_fat_lfn_sum(const uint8_t name11[11]);

struct ngl_sink_file {
    struct ngl_sink sink;
    int fd;
};

struct ngl_sink_mem {
    struct ngl_sink sink;
    uint8_t *buf;
    uint32_t writes;
};

int ngl_sink_file_open(struct ngl_sink_file *f, const char *path);
void ngl_sink_file_close(struct ngl_sink_file *f);
int ngl_sink_mem_init(struct ngl_sink_mem *m);
void ngl_sink_mem_free(struct ngl_sink_mem *m);

#endif
