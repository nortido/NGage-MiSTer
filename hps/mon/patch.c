// SPDX-License-Identifier: GPL-3.0-or-later
#include "patch.h"

static const struct patch_entry *cur_list;
static uint32_t cur_n;
static uint32_t mk_idx[PATCH_NMARK];
static uint8_t mk_thumb[PATCH_NMARK];
static uint32_t mk_n;

uint32_t udf_encode(uint32_t idx)
{
    return 0xE7F000F0u | ((idx >> 4) & 0xFFFu) << 8 | (idx & 0xFu);
}

int udf_decode(uint32_t word)
{
    if ((word & 0xFFF000F0u) != 0xE7F000F0u)
        return -1;
    return (int)(((word >> 8) & 0xFFFu) << 4 | (word & 0xFu));
}

uint32_t bkpt_encode(uint32_t idx)
{
    return 0xE1200070u | ((idx >> 4) & 0xFFFu) << 8 | (idx & 0xFu);
}

int bkpt_decode(uint32_t word)
{
    if ((word & 0xFFF000F0u) != 0xE1200070u)
        return -1;
    return (int)(((word >> 8) & 0xFFFu) << 4 | (word & 0xFu));
}

int site_decode(uint32_t word)
{
    int i = udf_decode(word);

    return i >= 0 ? i : bkpt_decode(word);
}

// a trap overwrites the banked LR/SPSR of the mode it enters: EKA1 runs kernel code in UND mode,
// so sites trap through BKPT (Prefetch Abort, ABT); only the FSR/FAR reads of guest abort handlers keep UDF
uint32_t patch_word(uint32_t idx, uint32_t orig)
{
    uint32_t crn = (orig >> 16) & 0xFu;

    if ((orig & 0x0F100F10u) == 0x0E100F10u && (crn == 5u || crn == 6u))
        return udf_encode(idx);
    return bkpt_encode(idx);
}

uint32_t patch_off(uint32_t off)
{
    return off & PATCH_OFF;
}

uint32_t mark_encode(uint32_t slot, int thumb)
{
    return thumb ? 0xBE00u | slot : bkpt_encode(0xFFF0u | slot);
}

int mark_decode(uint32_t word, int thumb)
{
    int i;

    if (thumb)
        return (word & 0xFFFFFFF8u) == 0xBE00u ? (int)(word & 7u) : -1;
    i = bkpt_decode(word);
    return i >= 0xFFF0 && i < 0xFFF0 + (int)PATCH_NMARK ? i - 0xFFF0 : -1;
}

int patch_mark_slot(uint32_t word, int thumb)
{
    int s = mark_decode(word, thumb);

    if (s < 0 || (uint32_t)s >= mk_n || (mk_thumb[s] != 0) != (thumb != 0))
        return -1;
    return s;
}

uint32_t patch_mark_count(void)
{
    return mk_n;
}

int patch_mark_is_exit(uint32_t slot)
{
    return slot < mk_n && (cur_list[mk_idx[slot]].off & PATCH_MKX) != 0;
}

uint32_t patch_mark_orig(uint32_t slot)
{
    return slot < mk_n ? cur_list[mk_idx[slot]].orig : 0;
}

int patch_mark_kind(uint32_t slot)
{
    uint32_t off;

    if (slot >= mk_n)
        return -1;
    off = cur_list[mk_idx[slot]].off;
    if (off & PATCH_MKX)
        return PATCH_K_EXIT;
    if ((off & PATCH_PCM) == PATCH_PCM)
        return PATCH_K_PCM;
    if (off & PATCH_LRN)
        return PATCH_K_LEARN;
    return (off & PATCH_XIF) ? PATCH_K_EXITIF : PATCH_K_MARK;
}

uint32_t patch_mark_param(uint32_t slot, uint32_t k)
{
    return patch_mark_kind(slot) == PATCH_K_LEARN && k < 2u ? cur_list[mk_idx[slot] + 1u + k].orig : 0;
}

static uint32_t rd_le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr_le(uint8_t *p, uint32_t u)
{
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static uint32_t rd_h(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static void wr_h(uint8_t *p, uint32_t u)
{
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
}

uint32_t patch_apply(uint8_t *rom, uint32_t rom_size, const struct patch_entry *list, uint32_t n)
{
    return patch_apply_route(rom, rom_size, list, n, 0);
}

uint32_t patch_apply_route(uint8_t *rom, uint32_t rom_size, const struct patch_entry *list, uint32_t n, int route)
{
    return patch_apply_tt(rom, rom_size, list, n, route, 0, 0);
}

uint32_t patch_apply_tt(uint8_t *rom, uint32_t rom_size, const struct patch_entry *list, uint32_t n, int route,
                        uint64_t prev_us, uint64_t now_us)
{
    uint32_t i, bad = 0;
    int marks = 0, pcm_n = 0, tt = 0;

    cur_list = list;
    cur_n = n;
    mk_n = 0;
    for (i = 0; i < n; i++)
        if (list[i].off & (PATCH_MK | PATCH_MKX))
            marks = 1;
    for (i = 0; i < n; i++) {
        uint32_t off = list[i].off;
        uint32_t w, u;

        if (off & PATCH_RW) {
            uint32_t o = patch_off(off);

            if ((off & ~PATCH_OFF) == (PATCH_RW | PATCH_TLO) && i + 3u < n && list[i + 1u].off == (off | PATCH_RWV) &&
                (list[i + 2u].off & ~PATCH_OFF) == (PATCH_RW | PATCH_THI) && patch_off(list[i + 2u].off) == o + 4u &&
                list[i + 3u].off == (list[i + 2u].off | PATCH_RWV) && (o & 3u) == 0 && rom_size >= 8 &&
                o <= rom_size - 8) {
                uint64_t v = (uint64_t)list[i + 3u].orig << 32 | list[i + 1u].orig, p = v + prev_us, q = v + now_us;
                uint32_t wl = rd_le(rom + o), wh = rd_le(rom + o + 4u);

                // the DDR ROM survives a cold restart: it holds the value this monitor run wrote at the previous boot
                if (tt || (wl != list[i].orig && wl != (uint32_t)p) ||
                    (wh != list[i + 2u].orig && wh != (uint32_t)(p >> 32)))
                    bad++;
                else {
                    wr_le(rom + o, (uint32_t)q);
                    wr_le(rom + o + 4u, (uint32_t)(q >> 32));
                }
                tt = 1;
                i += 3u;
                continue;
            }
            if ((off & (PATCH_RWV | PATCH_MK | PATCH_MKX | PATCH_LRN | PATCH_XIF)) || i + 1u >= n ||
                list[i + 1u].off != (off | PATCH_RWV) ||
                (o & 3u) != 0 || rom_size < 4 || o > rom_size - 4) {
                if (!(off & PATCH_RWV) && i + 1u < n && list[i + 1u].off == (off | PATCH_RWV))
                    i++;
                bad++;
                continue;
            }
            i++;
            w = rd_le(rom + o);
            u = list[i].orig;
            if ((off & PATCH_CDRV) && !route) {
                // a route-on start earlier in this power cycle left the replacement in the DDR ROM
                if (w == u)
                    wr_le(rom + o, list[i - 1u].orig);
                continue;
            }
            if (w != list[i - 1u].orig && w != u) {
                bad++;
                continue;
            }
            wr_le(rom + o, u);
            continue;
        }
        if (off & (PATCH_MK | PATCH_MKX)) {
            uint32_t o = patch_off(off), th = o & 3u, np = 0;
            int pcm = (off & (PATCH_MK | PATCH_MKX | PATCH_PCM)) == (PATCH_MK | PATCH_PCM);

            // a learn entry and its parameter pairs count as one entry, also when bad
            if ((off & PATCH_LRN) && !pcm)
                while (np < 2u && i + 1u + np < n && list[i + 1u + np].off == (off | PATCH_RWV))
                    np++;
            if ((off & PATCH_RWV) || ((off & PATCH_MK) && (off & PATCH_MKX)) ||
                (!pcm && (off & (PATCH_LRN | PATCH_XIF)) &&
                 ((off & (PATCH_MKX | PATCH_CDRV)) || (off & (PATCH_LRN | PATCH_XIF)) == (PATCH_LRN | PATCH_XIF))) ||
                ((off & PATCH_LRN) && !pcm && np != 2u) || (pcm && pcm_n) || mk_n >= PATCH_NMARK || th == 3u ||
                rom_size < 4 ||
                (o & ~1u) > rom_size - (th ? 2u : 4u)) {
                i += np;
                bad++;
                continue;
            }
            o &= ~1u;
            w = th ? rd_h(rom + o) : rd_le(rom + o);
            u = mark_encode(mk_n, th != 0);
            if (w != list[i].orig && w != u) {
                i += np;
                bad++;
                continue;
            }
            if (th)
                wr_h(rom + o, u);
            else
                wr_le(rom + o, u);
            mk_idx[mk_n] = i;
            mk_thumb[mk_n++] = th != 0;
            pcm_n += pcm;
            i += np;
            continue;
        }
        if (i > 0xFFFFu || (marks && i >= 0xFFF0u) || (off & 3u) != 0 || rom_size < 4 || off > rom_size - 4) {
            bad++;
            continue;
        }
        w = rd_le(rom + off);
        u = patch_word(i, list[i].orig);
        // a cold restart patches the ROM slot again: its own trap word is already in place
        if (w != list[i].orig && w != u) {
            bad++;
            continue;
        }
        wr_le(rom + off, u);
    }
    return bad;
}

uint32_t patch_count(void)
{
    return cur_n;
}

uint32_t patch_orig(uint32_t idx)
{
    return idx < cur_n ? cur_list[idx].orig : 0;
}
