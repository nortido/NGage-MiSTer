// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_PMUC_H
#define NGH_PMUC_H

#include <stdint.h>

#ifndef NGH_PMU_CENSUS
#define NGH_PMU_CENSUS  0
#endif
#ifndef NGH_LIM_COST
#define NGH_LIM_COST    0
#endif
// per-frame guest counts and the tick PC samples in the event ring, an evidence arm only
#ifndef NGH_PMU_FRAME
#define NGH_PMU_FRAME   0
#endif
// the limiter's cost budget needs the event counters too, without the census ring and rows
#define NGH_PMU_EV      (NGH_PMU_CENSUS || NGH_LIM_COST || NGH_PMU_FRAME)
#if NGH_PMU_FRAME && NGH_PMU_CENSUS
#error "the frame counts need a build without the census"
#endif
// FL8-AW: each tick sample also logs the guest's current thread (a PMF part 3 record), an evidence arm only
#ifndef NGH_TICK_CUR
#define NGH_TICK_CUR    0
#endif
#if NGH_TICK_CUR && !NGH_PMU_FRAME
#error "the current-thread record follows the tick samples"
#endif

// mispredicts, load/store, I miss, D miss, D access on counters 1..5; counter 0 stays the limiter's 0x68 and is
// the census's instruction count
#define PMUC_N          5u
#define PMUC_EVS        0x10u, 0x72u, 0x01u, 0x03u, 0x04u
#define PMUC_CNTEN      (((1u << PMUC_N) - 1u) << 1)
#define PMUC_NSEC       16u
#define PMUC_NROW       5u
#define PMUC_PER        64000u
#define PMUC_SAT        0xFFFFu
#ifndef NGH_PMUC_EVS
#define NGH_PMUC_EVS    PMUC_EVS
#endif
#ifndef NGH_PMUC_SET
#define NGH_PMUC_SET    0u
#endif

_Static_assert((PMUC_NSEC & (PMUC_NSEC - 1u)) == 0, "ring index by mask");
_Static_assert(2u + (PMUC_N + 1u) / 2u == PMUC_NROW, "instructions, seconds, two rates per row");

struct pmuc {
    uint32_t mon[PMUC_N];
    uint32_t t0[PMUC_N];
    uint32_t m0[PMUC_N];
    uint32_t i0;
    uint32_t mi0;
    uint32_t secs;
    uint32_t ins[PMUC_NSEC];
    uint32_t ev[PMUC_NSEC][PMUC_N];
};

void pmuc_reset(struct pmuc *p, const uint32_t *tot, uint32_t ins, uint32_t minst);
void pmuc_trap(struct pmuc *p, const uint32_t *e0, const uint32_t *e1);
void pmuc_second(struct pmuc *p, const uint32_t *tot, uint32_t ins, uint32_t minst);
uint32_t pmuc_rate(uint64_t ev, uint64_t ins);
void pmuc_rows(const struct pmuc *p, uint32_t *row);

#if NGH_PMU_FRAME
// part 0 and 1 per detected frame, part 2 once per second; the tick sample's class and word flags
enum { PMS_RUN = 0, PMS_STALL = 1, PMS_IDLE = 2, PMS_NCLS = 3 };
#define PMS_F_ROM       1u
#define PMS_F_NOMAP     2u
#define PMS_F_HI        4u
#define PMS_PSR         0xBFu
#define PMS_PSR_T       0x20u
#define PMF_SAT         1023u
_Static_assert(NGH_PMUC_SET <= 0xFFu, "the set id is a byte");

struct pmf_rec {
    uint32_t flags, pc, a, b, c;
};

struct pmf {
    uint32_t cyc, ins, ev[PMUC_N];
    uint32_t pins, idle, nfail;
    uint32_t ncls[PMS_NCLS];
};

struct pmf_io {
    void *c;
    uint32_t (*rd)(void *c, uint32_t va, uint32_t *w);
};

void pmf_ent(struct pmf *p, uint32_t cyc, uint32_t ins, const uint32_t *ev);
void pmf_frame(struct pmf_rec *r, const struct pmf *p, uint32_t mcyc, uint32_t lwcy, uint32_t minst, const uint32_t *mon,
               uint32_t frames);
void pmf_part2(struct pmf_rec *r, struct pmf *p, const uint32_t *evs);
void pmf_sample(struct pmf_rec *r, struct pmf *p, const struct pmf_io *io, uint32_t lr, uint32_t spsr, uint32_t gins,
                int stall);
#if NGH_TICK_CUR
void pmf_cur(struct pmf_rec *r, uint32_t pc, int (*rd)(void *c, uint32_t va, uint32_t *w), void *c);
#endif
#endif

#endif
