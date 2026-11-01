// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_PATCH_H
#define NGH_PATCH_H

#include <stdint.h>

struct patch_entry {
    uint32_t off;
    uint32_t orig;
};

// rewrite kind: {off | PATCH_RW, orig} then {off | PATCH_RW | PATCH_RWV, replacement}, applied without a trap
#define PATCH_RW  0x80000000u
#define PATCH_RWV 0x40000000u
// mark kind {off | PATCH_MK, orig} counts a hit and emulates orig, exit kind {off | PATCH_MKX, orig} parks.
// Offset low bits: 00 ARM word (orig = the word), 10 Thumb halfword at off, 01 Thumb halfword at off-1 (orig = the
// 16-bit halfword), 11 bad. Trap words: BKPT 0xFFF0+slot (ARM), 0xBE00+slot (Thumb); at most PATCH_NMARK sites.
#define PATCH_MK  0x20000000u
#define PATCH_MKX 0x10000000u
// cdrv flag on a rewrite (applied only with the C: route on, else the original word is put back) or on a mark (the
// format guard)
#define PATCH_CDRV 0x08000000u
// Kinds on a mark: learn {off | MK | LRN, orig}, then {off | MK | LRN | RWV, va} and {the same, field}, stores
// [[va] + field] once per guest boot; exitif {off | MK | XIF, orig} flags the exit when r0 holds that value
#define PATCH_LRN 0x04000000u
#define PATCH_XIF 0x02000000u
// pcm kind {off | MK | PCM, orig}: all three modifiers at once, a combination no other kind uses; at most one
#define PATCH_PCM (PATCH_LRN | PATCH_XIF | PATCH_CDRV)
// time kind: rewrite pairs {o | RW | TLO} then {(o + 4) | RW | THI}, written as value + elapsed us each boot
#define PATCH_TLO PATCH_XIF
#define PATCH_THI PATCH_LRN
#define PATCH_OFF 0x01FFFFFFu
#define PATCH_NMARK 8u

enum { PATCH_K_MARK = 0, PATCH_K_EXIT = 1, PATCH_K_LEARN = 2, PATCH_K_EXITIF = 3, PATCH_K_PCM = 4 };

_Static_assert(((PATCH_RW | PATCH_RWV | PATCH_MK | PATCH_MKX | PATCH_CDRV | PATCH_LRN | PATCH_XIF) & PATCH_OFF) == 0,
               "flags outside the offset");
_Static_assert(PATCH_OFF >= 0x01FFFFFFu, "32 MiB offsets");

uint32_t udf_encode(uint32_t idx);
int udf_decode(uint32_t word);
uint32_t bkpt_encode(uint32_t idx);
int bkpt_decode(uint32_t word);
int site_decode(uint32_t word);
uint32_t patch_word(uint32_t idx, uint32_t orig);
uint32_t patch_off(uint32_t off);
uint32_t mark_encode(uint32_t slot, int thumb);
int mark_decode(uint32_t word, int thumb);
int patch_mark_slot(uint32_t word, int thumb);
uint32_t patch_mark_count(void);
int patch_mark_is_exit(uint32_t slot);
int patch_mark_kind(uint32_t slot);
uint32_t patch_mark_param(uint32_t slot, uint32_t k);
uint32_t patch_mark_orig(uint32_t slot);
uint32_t patch_apply(uint8_t *rom, uint32_t rom_size, const struct patch_entry *list, uint32_t n);
uint32_t patch_apply_route(uint8_t *rom, uint32_t rom_size, const struct patch_entry *list, uint32_t n, int route);
uint32_t patch_apply_tt(uint8_t *rom, uint32_t rom_size, const struct patch_entry *list, uint32_t n, int route,
                        uint64_t prev_us, uint64_t now_us);
uint32_t patch_count(void);
uint32_t patch_orig(uint32_t idx);

#endif
