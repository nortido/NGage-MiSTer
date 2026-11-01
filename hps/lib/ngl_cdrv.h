// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_CDRV_H
#define NGL_CDRV_H

#include <stddef.h>
#include <stdint.h>

#define NGL_CDRV_HDR_BYTES      32u
#define NGL_CDRV_PAYLOAD_BYTES  0x400000u
#define NGL_CDRV_FILE_BYTES     (NGL_CDRV_HDR_BYTES + NGL_CDRV_PAYLOAD_BYTES)
#define NGL_CDRV_VERSION        1u
#define NGL_CDRV_BLOCK_BYTES    0x10000u
#define NGL_CDRV_DEBOUNCE_MS    2000u
#define NGL_CDRV_GAP_MS         10000u
#define NGL_CDRV_MAX_FLUSHES    100u
#define NGL_CDRV_NAME           "c_drive.bin"

enum {
    NGL_CDRV_OK = 0,
    NGL_CDRV_ABSENT,
    NGL_CDRV_FOREIGN,
    NGL_CDRV_ROMCHANGED,
    NGL_CDRV_CORRUPT,
    NGL_CDRV_IOERR,
    NGL_CDRV_NAME_ERR
};

enum {
    NGL_CDRV_ASIDE_NONE = 0,
    NGL_CDRV_ASIDE_RENAME,
    NGL_CDRV_ASIDE_COPY
};

enum {
    NGL_CDRV_INFO_NONE = 0,
    NGL_CDRV_INFO_ROM_CHANGED,
    NGL_CDRV_INFO_DAMAGED,
    NGL_CDRV_INFO_FOREIGN,
    NGL_CDRV_INFO_CANNOT_READ
};

enum {
    NGL_CDRV_SNAP_OK = 0,
    NGL_CDRV_SNAP_BUSY,
    NGL_CDRV_SNAP_IO
};

struct ngl_cdrv_info {
    uint32_t id;
    uint32_t crc;
};

struct ngl_cdrv_action {
    uint8_t use_payload;
    uint8_t fill_ff;
    uint8_t aside;
    uint8_t persist;
    uint8_t write_now;
    uint8_t info;
};

struct ngl_cdrv_sched {
    uint32_t gen;
    uint32_t last_change_ms;
    uint32_t last_flush_ms;
    uint32_t sched_flushes;
    uint8_t pending;
    uint8_t flushed_once;
};

typedef int (*ngl_cdrv_gen_fn)(void *ctx, uint32_t *gen);
typedef int (*ngl_cdrv_read_fn)(void *ctx, uint32_t off, uint8_t *dst, uint32_t len);

void ngl_cdrv_hdr(uint8_t *out, uint32_t rom_id, uint32_t payload_crc);
int ngl_cdrv_parse(const uint8_t *buf, size_t len, uint32_t rom_id, struct ngl_cdrv_info *info);
struct ngl_cdrv_action ngl_cdrv_action(int status);
int ngl_cdrv_guard(struct ngl_cdrv_action a, int payload_nonblank, uint32_t row);

int ngl_cdrv_snapshot(ngl_cdrv_gen_fn rd_gen, ngl_cdrv_read_fn mem_read, void *ctx, uint8_t *dst,
                      unsigned tries);

void ngl_cdrv_observe(struct ngl_cdrv_sched *s, uint32_t gen, uint32_t now_ms);
void ngl_cdrv_rebase(struct ngl_cdrv_sched *s, uint32_t gen);
void ngl_cdrv_mark_pending(struct ngl_cdrv_sched *s, uint32_t now_ms);
int ngl_cdrv_due(const struct ngl_cdrv_sched *s, uint32_t now_ms, int force);
void ngl_cdrv_flushed(struct ngl_cdrv_sched *s, uint32_t gen, uint32_t now_ms, int scheduled);
void ngl_cdrv_flush_failed(struct ngl_cdrv_sched *s, uint32_t now_ms, int scheduled);

int ngl_cdrv_path(const char *dir, const char *name, char *out, size_t cap);
const char *ngl_cdrv_str(int status);

#endif
