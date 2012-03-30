/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

struct AVCodecContext;

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

int64_t gstav_timestamp_to_pts(struct AVCodecContext *ctx, int64_t ts);
int64_t gstav_pts_to_timestamp(struct AVCodecContext *ctx, int64_t pts);

#endif /* UTIL_H */
