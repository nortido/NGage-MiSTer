// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_EVLOG_H
#define NGH_EVLOG_H

#include <stdint.h>

#define EV_MAGIC    0x5645474Eu

struct evhdr {
    uint32_t magic;
    uint32_t rec_size;
    uint32_t nrec;
    uint32_t head;
    uint32_t dropped;
    uint32_t reserved[3];
};

struct evrec {
    uint32_t seq;
    uint16_t kind;
    uint16_t flags;
    uint32_t pc;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint64_t t64;
};

// CP15: a=insn b=val c=mode. MMIO: a=pa b=val. ABT: a=far b=fsr; FILL: a=pages b=tables c=skips<<20|us. IRQ HELD: a=us b=holds c=max us. REENTRY: a=far. MARK: a=slot b=r0 c=mode.
enum { EVK_CP15 = 1, EVK_MMIO = 2, EVK_IRQ = 3, EVK_ABT = 4, EVK_REENTRY = 5, EVK_MARK = 6 };
// NGH_PMU_FRAME only (7 is the audio-wait probe's kind). PMF: flags = part | set << 8. PMS: pc = the interrupted guest pc,
// a = its word, b = spsr mode, T, I | class << 16 | word flags << 20, c = guest instructions since the last sample.
// PMF part 3 (FL8-AW, NGH_TICK_CUR) right after each PMS: pc = that sample's pc, a = the guest current-thread word or
// 0xFFFFFFFF if unread, b = c = 0 (NGH_LIM_XHOLD builds: b = xam, c = xan); a new kind would reach the readers that drop only 8 and 9.
#define EVK_PMF     8u
#define EVK_PMS     9u

#define EVF_SZ_B    0u
#define EVF_SZ_H    1u
#define EVF_SZ_W    2u
#define EVF_SZ_MASK 3u
#define EVF_WRITE   4u
#define EVF_CDRV    8u

enum { EVABT_D = 0, EVABT_P = 1, EVABT_U = 2, EVABT_FILL = 3 };
enum { EVRE_ABT = 0, EVRE_UND = 1 };
#define EVF_MARK_EXIT 1u
#define EVF_MARK_LEARN 2u
#define EVF_EXIT_CAP 0x10u
// NGH_MK_CAP arms: one mark or exitif hit is EV_MKCAP_NREC MARK records with EVF_EXIT_CAP, pc = tag | k << 8 |
// slot; k1 r0 r1 r2, k2 r3 lr sp, k3 cur own mode, k4 USR lr, sp and the scan, k5-k7 candidates, k8 offsets, k9-k10 names
#define EV_MKCAP_TAG 0xE11D0000u
#define EV_MKCAP_NREC 10u
#define EV_MKCAP_REGS 1u
#define EV_MKCAP_LRSP 2u
#define EV_MKCAP_THR 3u
#define EV_MKCAP_USR 4u
#define EV_MKCAP_W0 5u
#define EV_MKCAP_OFF 8u
#define EV_MKCAP_NM0 9u
#define EV_MKCAP_NM1 10u
#define EVF_FILL_STOP 4u
#define EVF_IRQ_HELD 1u

_Static_assert(sizeof(struct evhdr) == 32, "ring header");
_Static_assert(sizeof(struct evrec) == 32, "ring record");

#endif
