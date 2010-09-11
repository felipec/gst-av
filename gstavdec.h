/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#ifndef GST_AVDEC_H
#define GST_AVDEC_H

#include <gst/gst.h>
#include <libavcodec/avcodec.h>

#include <stdbool.h>

#define GST_AVDEC(obj) (GstAVDec *) (obj)
#define GST_AVDEC_TYPE (gst_avdec_get_type())
#define GST_AVDEC_CLASS(obj) (GstAVDecClass *) (obj)

typedef struct GstAVDec GstAVDec;
typedef struct GstAVDecClass GstAVDecClass;

struct oggvorbis_private {
	unsigned int len[3];
	unsigned char *packet[3];
};

struct ring {
	int in, out;
};

struct GstAVDec {
	GstElement element;
	GstPad *sinkpad, *srcpad;
	AVCodec *codec;
	AVCodecContext *av_ctx;
	bool got_header;
	struct oggvorbis_private priv;
	gint64 granulepos;
	AVPacket pkt;
	struct ring ring;
	int (*header_func)(GstAVDec *self, GstBuffer *buf);
};

struct GstAVDecClass {
	GstElementClass parent_class;
};

GType gst_avdec_get_type(void);

#endif /* GST_AVDEC_H */
