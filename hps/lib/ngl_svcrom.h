// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_SVCROM_H
#define NGL_SVCROM_H

#include <stddef.h>
#include <stdint.h>

#define NGL_SVCROM_SIZE     20971520u
#define NGL_SVCROM_3C1_LEN  18777938u
#define NGL_SVCROM_326_LEN  1361703u
#define NGL_SVCROM_CRC_3C1  0x7bc70477u
#define NGL_SVCROM_CRC_326  0xc3b897c7u
#define NGL_SVCROM_CRC_OUT  0xaf2623d1u

enum {
    NGL_SVCROM_3C1 = 0,
    NGL_SVCROM_326,
    NGL_SVCROM_OUT
};

enum {
    NGL_SVCROM_OK = 0,
    NGL_SVCROM_E_SIZE,
    NGL_SVCROM_E_CRC,
    NGL_SVCROM_E_BLOCK,
    NGL_SVCROM_E_ADDR,
    NGL_SVCROM_E_ORDER,
    NGL_SVCROM_E_TRUNC
};

int ngl_svcrom_check(const uint8_t *b, size_t len, int id);
int ngl_svcrom_build(const uint8_t *c1, size_t c1len, const uint8_t *s26, size_t s26len,
                     uint8_t *out, size_t cap);
const char *ngl_svcrom_strerror(int err);

#endif
