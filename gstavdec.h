/*
 * Copyright (C) 2009 Nokia Corporation.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef GST_AVDEC_H
#define GST_AVDEC_H

#include <gst/gst.h>
#include <libavcodec/avcodec.h>

G_BEGIN_DECLS

#define GST_AVDEC(obj) (GstAVDec *) (obj)
#define GST_AVDEC_TYPE (gst_avdec_get_type())
#define GST_AVDEC_CLASS(obj) (GstAVDecClass *) (obj)

typedef struct GstAVDec GstAVDec;
typedef struct GstAVDecClass GstAVDecClass;

struct oggvorbis_private {
	unsigned int len[3];
	unsigned char *packet[3];
};

struct GstAVDec {
	GstElement element;
	GstPad *sinkpad, *srcpad;
	AVCodec *codec;
	AVCodecContext *av_ctx;
	int seq;
	int header;
	struct oggvorbis_private priv;
	gint64 granulepos;
	AVPacket pkt;
};

struct GstAVDecClass {
	GstElementClass parent_class;
};

GType gst_avdec_get_type(void);

G_END_DECLS

#endif /* GST_AVDEC_H */
