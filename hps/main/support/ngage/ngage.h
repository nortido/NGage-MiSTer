// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGAGE_H
#define NGAGE_H

#include <stdint.h>

#define NGAGE_CORE_NAME     "NGage"

void ngage_recover();
void ngage_init();
void ngage_set_image(int index, const char *path);
int  ngage_sd_service(int disk, int op, uint32_t lba, int sz, int ack);
void ngage_poll();
void ngage_unload();
int  ngage_save_flush(const char *why);
int  ngage_core_ok();
int  ngage_browse_zip(const char *path);

#endif
