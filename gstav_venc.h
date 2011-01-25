/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#ifndef GST_AV_VENC_H
#define GST_AV_VENC_H

#include <gst/gst.h>
#include <libavcodec/avcodec.h>
#include <stdbool.h>

#define GST_AV_VENC_TYPE (gst_av_venc_get_type())

GType gst_av_venc_get_type(void);

struct gst_av_venc {
	GstElement element;
	GstPad *sinkpad, *srcpad;
	AVCodec *codec;
	AVCodecContext *av_ctx;
	bool initialized;
	int codec_id;
	void (*init_ctx)(struct gst_av_venc *base, AVCodecContext *ctx);
};

#endif /* GST_AV_VENC_H */
