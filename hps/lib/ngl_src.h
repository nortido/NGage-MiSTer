// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_SRC_H
#define NGL_SRC_H

#include "ngl_types.h"

struct ngl_src;

extern const struct ngl_src_ops ngl_src_ops;

struct ngl_src *ngl_src_open(const char *path, char *err, int errlen);
struct ngl_src *ngl_src_open_top(const char *path, char *err, int errlen);
void ngl_src_close(struct ngl_src *s);
int ngl_src_zip_path(const char *path);

#endif
