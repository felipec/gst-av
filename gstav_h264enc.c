/*
 * Copyright (C) 2009-2012 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "gstav_h264enc.h"
#include "gstav_venc.h"
#include "plugin.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <gst/tag/tag.h>

#include <stdlib.h>
#include <string.h> /* for memcpy */
#include <stdbool.h>

#include "util.h"

#define GST_CAT_DEFAULT gstav_debug

struct obj {
	struct gst_av_venc parent;
};

struct obj_class {
	GstElementClass parent_class;
};

#if LIBAVUTIL_VERSION_MAJOR < 52 && !(LIBAVUTIL_VERSION_MAJOR == 51 && LIBAVUTIL_VERSION_MINOR >= 12)
static int av_opt_set(void *obj, const char *name, const char *val, int search_flags)
{
	return av_set_string3(obj, name, val, 0, NULL);
}
#endif

static void init_ctx(struct gst_av_venc *base, AVCodecContext *ctx)
{
	av_opt_set(ctx->priv_data, "preset", "medium", 0);
	av_opt_set(ctx->priv_data, "profile", "baseline", 0);
	av_opt_set(ctx->priv_data, "x264opts", "annexb=1", 0);
	av_opt_set_int(ctx->priv_data, "aud", 1, 0);
}

static GstCaps *
generate_src_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("video/x-h264",
			"stream-format", G_TYPE_STRING, "byte-stream",
			"alignment", G_TYPE_STRING, "au",
			NULL);

	gst_caps_append_structure(caps, struc);

	return caps;
}

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps;

	caps = gst_caps_new_simple("video/x-raw-yuv",
			"format", GST_TYPE_FOURCC, GST_MAKE_FOURCC('I', '4', '2', '0'),
			NULL);

	return caps;
}

static void
instance_init(GTypeInstance *instance, void *g_class)
{
	struct gst_av_venc *venc = (struct gst_av_venc *)instance;

	venc->codec_id = CODEC_ID_H264;
	venc->init_ctx = init_ctx;
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;

	gst_element_class_set_details_simple(element_class,
			"av h264 video encoder",
			"Coder/Encoder/Video",
			"H.264 encoder wrapper for libavcodec",
			"Felipe Contreras");

	template = gst_pad_template_new("src", GST_PAD_SRC,
			GST_PAD_ALWAYS,
			generate_src_template());

	gst_element_class_add_pad_template(element_class, template);

	template = gst_pad_template_new("sink", GST_PAD_SINK,
			GST_PAD_ALWAYS,
			generate_sink_template());

	gst_element_class_add_pad_template(element_class, template);
}

GType
gst_av_h264enc_get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct obj_class),
			.base_init = base_init,
			.instance_size = sizeof(struct obj),
			.instance_init = instance_init,
		};

		type = g_type_register_static(GST_AV_VENC_TYPE, "GstAVH264Enc", &type_info, 0);
	}

	return type;
}
