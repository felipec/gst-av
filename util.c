/*
 * Copyright (C) 2009-2012 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#include "util.h"
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <gst/gst.h>

int64_t gstav_timestamp_to_pts(AVCodecContext *ctx, int64_t ts)
{
	AVRational bq = { 1, GST_SECOND * ctx->ticks_per_frame };
	if (ts == -1)
		return AV_NOPTS_VALUE;
	return av_rescale_q(ts, bq, ctx->time_base);
}

int64_t gstav_pts_to_timestamp(AVCodecContext *ctx, int64_t pts)
{
	AVRational bq = { 1, GST_SECOND * ctx->ticks_per_frame };
	if (pts == (int64_t) AV_NOPTS_VALUE)
		return -1;
	return av_rescale_q(pts, ctx->time_base, bq);
}
