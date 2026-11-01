// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_STATUS_H
#define NGH_STATUS_H

#include <stddef.h>
#include <stdint.h>

#define NGST_MAGIC      0x5453474Eu
#define NGST_LAYOUT     1u

enum { NGST_STATE_BOOT = 0, NGST_STATE_RUN = 1, NGST_STATE_PARKED = 2 };
enum {
    NGST_PARK_NONE = 0, NGST_PARK_REQUEST = 1, NGST_PARK_FATAL = 2,
    NGST_PARK_FPGA_LOST = 3, NGST_PARK_WINDOW_ABORT = 4, NGST_PARK_MILESTONE = 5, NGST_PARK_L2 = 6,
    NGST_PARK_EXIT = 7
};
enum { NGST_LOG_STOP_FULL = 0, NGST_LOG_RING = 1 };
enum { NGST_PROFILE_NGAGE = 0, NGST_PROFILE_TEST = 1 };
#define NGST_MILE_MMU   0x1u
#define NGST_MILE_IRQ   0x2u
// tick fills fatal_rec without a park and clears the bit; kind and fatal stay
#define NGST_MILE_SNAP  0x4u
#define NGST_MC_INNER_WB  0x1u
#define NGST_MC_OUTER_WB  0x2u
#define NGST_MC_LCD_WT    0x4u
#define NGST_MC_L1PF      0x8u
#define NGST_MC_L2PH      0x10u
#define NGST_MC_SWCO_OFF  0x20u
enum {
    NGST_TRAP_CP15 = 0, NGST_TRAP_MMIO = 1, NGST_TRAP_UNDEF = 2,
    NGST_TRAP_DABT = 3, NGST_TRAP_PABT = 4, NGST_NTRAP = 8
};
enum {
    NGST_CP15_SCTLR = 0, NGST_CP15_ACTLR = 1, NGST_CP15_CPACR = 2, NGST_CP15_TTBR0 = 3,
    NGST_CP15_TTBR1 = 4, NGST_CP15_TTBCR = 5, NGST_CP15_DACR = 6, NGST_CP15_CONTEXTIDR = 7,
    NGST_NCP15 = 16
};

#define NGST_NPROBE     64
#define NGST_NFPS       64
#define NGST_NXPROBE    160

struct ngst_fatal {
    uint32_t kind;
    uint32_t mon_pc;
    uint32_t r[13];
    // banked order: usr/sys, svc, abt, und, irq, fiq
    uint32_t sp[6];
    uint32_t lr[6];
    // svc, abt, und, irq, fiq
    uint32_t spsr[5];
    uint32_t cpsr;
    uint32_t cp15[NGST_NCP15];
    uint32_t dfsr;
    uint32_t dfar;
    uint32_t ifsr;
    uint32_t ifar;
};

struct ngst_probe {
    uint32_t key;
    uint32_t val;
};

struct ngst {
    uint32_t magic;
    uint32_t layout;
    char version[16];
    uint32_t state;
    uint32_t park_reason;
    uint32_t heartbeat;
    uint32_t park_req;
    uint32_t flush_req;
    uint32_t flush_ack;
    uint32_t log_mode;
    uint32_t profile;
    uint32_t prog_size;
    uint32_t mile_mask;
    uint32_t traps[NGST_NTRAP];
    uint32_t forwards;
    uint32_t fills;
    uint32_t flushes;
    uint32_t irqs;
    uint32_t ticks;
    uint32_t fatal;
    uint32_t memcfg;
    uint32_t cdrv;
    struct ngst_fatal fatal_rec;
    struct ngst_probe probe[NGST_NPROBE];
    uint16_t lcd_fps[NGST_NFPS];
    uint32_t lcd_fps_idx;
    uint32_t cdrv_gen;
    uint32_t exit_seen;
    uint32_t gproc;
    uint32_t aud_bytes;
    uint32_t aud_chunks;
    uint32_t aud_drops;
    uint32_t aud_skips;
    uint32_t aud_rate;
    uint32_t aud_ch;
    uint32_t aud_cfg;
    uint32_t aud_trim;
    struct ngst_probe xprobe[NGST_NXPROBE];
};

// Byte offsets for scripts; asserted against the struct below.
#define NGST_OFF_STATE      0x18
#define NGST_OFF_PARK_REQ   0x24
#define NGST_OFF_FLUSH_REQ  0x28
#define NGST_OFF_FLUSH_ACK  0x2C
#define NGST_OFF_PROFILE    0x34
#define NGST_OFF_PROG_SIZE  0x38
#define NGST_OFF_MILE_MASK  0x3C
#define NGST_OFF_MEMCFG     0x78
#define NGST_OFF_CDRV       0x7C
#define NGST_OFF_CDRV_GEN   0x3D8
#define NGST_OFF_EXIT_SEEN  0x3DC
#define NGST_OFF_GPROC      0x3E0
#define NGST_OFF_XPROBE     0x404

#define NGST_CDRV_MAGIC     0x56524443u

_Static_assert(offsetof(struct ngst, state) == NGST_OFF_STATE, "state");
_Static_assert(offsetof(struct ngst, park_req) == NGST_OFF_PARK_REQ, "park_req");
_Static_assert(offsetof(struct ngst, flush_req) == NGST_OFF_FLUSH_REQ, "flush_req");
_Static_assert(offsetof(struct ngst, flush_ack) == NGST_OFF_FLUSH_ACK, "flush_ack");
_Static_assert(offsetof(struct ngst, profile) == NGST_OFF_PROFILE, "profile");
_Static_assert(offsetof(struct ngst, prog_size) == NGST_OFF_PROG_SIZE, "prog_size");
_Static_assert(offsetof(struct ngst, mile_mask) == NGST_OFF_MILE_MASK, "mile_mask");
_Static_assert(offsetof(struct ngst, memcfg) == NGST_OFF_MEMCFG, "memcfg");
_Static_assert(offsetof(struct ngst, cdrv) == NGST_OFF_CDRV, "cdrv");
_Static_assert(offsetof(struct ngst, cdrv_gen) == NGST_OFF_CDRV_GEN, "cdrv_gen");
_Static_assert(offsetof(struct ngst, exit_seen) == NGST_OFF_EXIT_SEEN, "exit_seen");
_Static_assert(offsetof(struct ngst, gproc) == NGST_OFF_GPROC, "gproc");
_Static_assert(offsetof(struct ngst, xprobe) == NGST_OFF_XPROBE, "xprobe");
_Static_assert(sizeof(struct ngst) == NGST_OFF_XPROBE + NGST_NXPROBE * sizeof(struct ngst_probe), "xprobe is the tail");
_Static_assert(offsetof(struct ngst, fatal_rec) == 0x80, "fatal_rec");
_Static_assert(sizeof(struct ngst) <= 4096, "status block is one page");

#endif
