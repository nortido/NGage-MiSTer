// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_SAV_H
#define NGL_SAV_H

#include <stddef.h>
#include <stdint.h>

#include "ngl_types.h"

#define NGL_SAV_HDR_BYTES     32u
#define NGL_SAV_REC_BYTES     516u
#define NGL_SAV_VERSION       1u
#define NGL_SAV_MAX_RECORDS   8192u
#define NGL_SAV_BITMAP_BYTES  (NGL_IMG_SECTORS / 8u)
#define NGL_SAV_DEBOUNCE_MS   2000u
#define NGL_SAV_GAP_MS        10000u
#define NGL_SAV_MAX_FLUSHES   200u
#define NGL_SAV_ASIDE_TRIES   1000u
#define NGL_SAV_MAX_FILE      (NGL_SAV_HDR_BYTES + NGL_SAV_MAX_RECORDS * NGL_SAV_REC_BYTES)

enum {
    NGL_SAV_OK = 0,
    NGL_SAV_ABSENT,
    NGL_SAV_FOREIGN,
    NGL_SAV_MISMATCH,
    NGL_SAV_CORRUPT,
    NGL_SAV_FULL,
    NGL_SAV_RANGE,
    NGL_SAV_SINK,
    NGL_SAV_NAME
};

struct ngl_sav {
    uint8_t *bitmap;
    uint32_t dirty;
    uint32_t last_write_ms;
    uint32_t last_flush_ms;
    uint32_t sched_flushes;
    uint8_t pending;
    uint8_t flushed_once;
    uint8_t full;
};

struct ngl_sav_info {
    uint32_t id;
    uint32_t count;
};

typedef int (*ngl_sav_exists_fn)(void *ctx, const char *path);

void ngl_sav_init(struct ngl_sav *s, uint8_t *bitmap);
uint32_t ngl_sav_crc32(const void *buf, size_t len);
uint32_t ngl_sav_image_id(const uint8_t *img);

int ngl_sav_store(struct ngl_sav *s, uint8_t *img, uint64_t off, const void *data, uint32_t len,
                  uint32_t now_ms);
uint32_t ngl_sav_size(const struct ngl_sav *s);
int ngl_sav_serialize(const struct ngl_sav *s, const uint8_t *img, uint32_t image_id,
                      const struct ngl_sink *sink);
int ngl_sav_parse(const uint8_t *buf, size_t len, uint32_t image_id, struct ngl_sav_info *info);
int ngl_sav_apply(struct ngl_sav *s, uint8_t *img, const uint8_t *buf, size_t len,
                  uint32_t image_id, uint32_t *applied);

int ngl_sav_due(const struct ngl_sav *s, uint32_t now_ms, int unload);
void ngl_sav_flushed(struct ngl_sav *s, uint32_t now_ms, int scheduled);
void ngl_sav_flush_failed(struct ngl_sav *s, uint32_t now_ms, int scheduled);

int ngl_sav_aside_name(const char *sav_path, uint32_t id, ngl_sav_exists_fn exists, void *ctx,
                       char *out, size_t cap);
int ngl_sav_path(const char *dir, const char *zip_path, char *out, size_t cap);
const char *ngl_sav_str(int status);

#endif
