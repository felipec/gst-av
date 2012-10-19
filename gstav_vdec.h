/*
 * Copyright (C) 2009-2012 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#ifndef GST_AV_VDEC_H
#define GST_AV_VDEC_H

#include <gst/gst.h>
#include <libavcodec/avcodec.h>
#include <stdbool.h>

#define GST_AV_VDEC_TYPE (gst_av_vdec_get_type())

GType gst_av_vdec_get_type(void);

struct gst_av_vdec {
	GstElement element;
	GstPad *sinkpad, *srcpad;
	AVCodec *codec;
	AVCodecContext *av_ctx;
	bool initialized;
	bool (*parse_func)(struct gst_av_vdec *vdec, GstBuffer *buf);
	GMutex *mutex;
};

#endif /* GST_AV_VDEC_H */
