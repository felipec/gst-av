/*
 * Copyright (C) 2012 Felipe Contreras
 * Copyright (C) 2009 Marco Ballesio
 *
 * Authors:
 * Marco Ballesio <marco.ballesio@gmail.com>
 * Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#ifndef GST_AV_PARSE_H
#define GST_AV_PARSE_H

#include <glib.h>

struct gst_av_vdec;

bool gst_av_h263_parse(struct gst_av_vdec *vdec, GstBuffer *buf);
bool gst_av_mpeg4_parse(struct gst_av_vdec *vdec, GstBuffer *buf);
bool gst_av_h264_parse(struct gst_av_vdec *vdec, GstBuffer *buf);

#endif
