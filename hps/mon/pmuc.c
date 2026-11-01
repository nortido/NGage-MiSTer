// SPDX-License-Identifier: GPL-3.0-or-later
#include "pmuc.h"
#if NGH_TICK_CUR
#include "awp.h"
#endif

#if NGH_PMU_CENSUS
void pmuc_reset(struct pmuc *p, const uint32_t *tot, uint32_t ins, uint32_t minst)
{
    uint32_t i, k;

    for (i = 0; i < PMUC_NSEC; i++) {
        p->ins[i] = 0;
        for (k = 0; k < PMUC_N; k++)
            p->ev[i][k] = 0;
    }
    for (k = 0; k < PMUC_N; k++) {
        p->mon[k] = 0;
        p->m0[k] = 0;
        p->t0[k] = tot[k];
    }
    p->i0 = ins;
    p->mi0 = minst;
    p->secs = 0;
}
#endif

#if NGH_PMU_EV
void pmuc_trap(struct pmuc *p, const uint32_t *e0, const uint32_t *e1)
{
    uint32_t k;

    for (k = 0; k < PMUC_N; k++)
        p->mon[k] += e1[k] - e0[k];
}
#endif

#if NGH_PMU_CENSUS
void pmuc_second(struct pmuc *p, const uint32_t *tot, uint32_t ins, uint32_t minst)
{
    uint32_t s = p->secs & (PMUC_NSEC - 1u), k;

    // modulo 2^32 differences: a counter that wrapped since the last second still gives its delta
    p->ins[s] = (ins - p->i0) - (minst - p->mi0);
    for (k = 0; k < PMUC_N; k++) {
        p->ev[s][k] = (tot[k] - p->t0[k]) - (p->mon[k] - p->m0[k]);
        p->t0[k] = tot[k];
        p->m0[k] = p->mon[k];
    }
    p->i0 = ins;
    p->mi0 = minst;
    p->secs++;
}

uint32_t pmuc_rate(uint64_t ev, uint64_t ins)
{
    uint64_t n = ev * PMUC_PER;
    uint32_t q = 0, b;

    if (!ins)
        return 0;
    if (n >= ins << 16)
        return PMUC_SAT;
    // the A9 has no divide instruction and the monitor links no libgcc: 16 quotient bits by shift and subtract
    for (b = 16u; b-- > 0u;) {
        if (n >= ins << b) {
            n -= ins << b;
            q |= 1u << b;
        }
    }
    return q;
}

void pmuc_rows(const struct pmuc *p, uint32_t *row)
{
    uint64_t ins = 0, ev[PMUC_N];
    uint32_t r[PMUC_N + 1u], i, k;

    for (k = 0; k < PMUC_N; k++)
        ev[k] = 0;
    for (i = 0; i < PMUC_NSEC; i++) {
        ins += p->ins[i];
        for (k = 0; k < PMUC_N; k++)
            ev[k] += p->ev[i][k];
    }
    for (k = 0; k < PMUC_N; k++)
        r[k] = pmuc_rate(ev[k], ins);
    r[PMUC_N] = 0;
    row[0] = ins > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ins;
    row[1] = p->secs;
    for (k = 0; k < PMUC_NROW - 2u; k++)
        row[2u + k] = r[2u * k] << 16 | r[2u * k + 1u];
}
#endif

#if NGH_PMU_FRAME
void pmf_ent(struct pmf *p, uint32_t cyc, uint32_t ins, const uint32_t *ev)
{
    uint32_t k;

    p->cyc = cyc;
    p->ins = ins;
    for (k = 0; k < PMUC_N; k++)
        p->ev[k] = ev[k];
}

void pmf_frame(struct pmf_rec *r, const struct pmf *p, uint32_t mcyc, uint32_t lwcy, uint32_t minst, const uint32_t *mon,
               uint32_t frames)
{
    r[0].flags = NGH_PMUC_SET << 8;
    r[0].pc = p->ins - minst;
    r[0].a = p->ev[0] - mon[0];
    r[0].b = p->ev[1] - mon[1];
    r[0].c = p->ev[2] - mon[2];
    r[1].flags = 1u | NGH_PMUC_SET << 8;
    r[1].pc = p->cyc - mcyc - lwcy;
    r[1].a = p->ev[3] - mon[3];
    r[1].b = p->ev[4] - mon[4];
    r[1].c = frames;
}

static uint32_t pmf_sat(uint32_t n)
{
    return n < PMF_SAT ? n : PMF_SAT;
}

void pmf_part2(struct pmf_rec *r, struct pmf *p, const uint32_t *evs)
{
    uint32_t k;

    r->flags = 2u | NGH_PMUC_SET << 8;
    r->pc = (evs[0] & 0xFFu) | (evs[1] & 0xFFu) << 8 | (evs[2] & 0xFFu) << 16 | (evs[3] & 0xFFu) << 24;
    r->a = evs[4];
    r->b = pmf_sat(p->ncls[PMS_RUN]) | pmf_sat(p->ncls[PMS_STALL]) << 10 | pmf_sat(p->ncls[PMS_IDLE]) << 20;
    r->c = p->nfail;
    for (k = 0; k < PMS_NCLS; k++)
        p->ncls[k] = 0;
    p->nfail = 0;
}

void pmf_sample(struct pmf_rec *r, struct pmf *p, const struct pmf_io *io, uint32_t lr, uint32_t spsr, uint32_t gins,
                int stall)
{
    uint32_t pc = lr - 4u, w = 0, fl = 0, cls = stall ? PMS_STALL : p->idle ? PMS_IDLE : PMS_RUN;

    if ((spsr & PMS_PSR_T) && (pc & 2u))
        fl = PMS_F_HI;
    // a stalled guest sat at this pc since the last tick: its word says nothing new
    if (cls != PMS_STALL) {
        fl |= io->rd(io->c, pc & ~3u, &w) & (PMS_F_ROM | PMS_F_NOMAP);
        if (fl & (PMS_F_ROM | PMS_F_NOMAP))
            w = 0;
        if (fl & PMS_F_NOMAP)
            p->nfail++;
    }
    p->ncls[cls]++;
    p->idle = 0;
    r->flags = 0;
    r->pc = pc;
    r->a = w;
    r->b = (spsr & PMS_PSR) | cls << 16 | fl << 20;
    r->c = gins - p->pins;
    p->pins = gins;
}

#if NGH_TICK_CUR
void pmf_cur(struct pmf_rec *r, uint32_t pc, int (*rd)(void *c, uint32_t va, uint32_t *w), void *c)
{
    uint32_t w;

    r->flags = 3u | NGH_PMUC_SET << 8;
    r->pc = pc;
    // a refused page reads as AWP_BAD, as in the audio-wait probe's records
    r->a = rd(c, AWP_CUR, &w) ? AWP_BAD : w;
    r->b = 0;
    r->c = 0;
}
#endif
#endif
