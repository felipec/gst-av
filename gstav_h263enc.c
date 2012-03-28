/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "gstav_h263enc.h"
#include "gstav_venc.h"
#include "plugin.h"

#include <libavcodec/avcodec.h>
#include <gst/tag/tag.h>

#include <stdlib.h>
#include <string.h> /* for memcpy */
#include <stdbool.h>

#include "util.h"

#define GST_CAT_DEFAULT gstav_debug

static GstElementClass *parent_class;

struct obj {
	struct gst_av_venc parent;
};

struct obj_class {
	GstElementClass parent_class;
};

static GstStateChangeReturn
change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret;
	struct gst_av_venc *base;

	base = (struct gst_av_venc *)element;

	switch (transition) {
	case GST_STATE_CHANGE_NULL_TO_READY:
		base->initialized = false;
		break;

	default:
		break;
	}

	ret = parent_class->change_state(element, transition);

	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition) {
	case GST_STATE_CHANGE_READY_TO_NULL:
		if (base->av_ctx) {
			gst_av_codec_close(base->av_ctx);
			av_freep(&base->av_ctx);
		}
		break;

	default:
		break;
	}

	return ret;
}

static GstCaps *
generate_src_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("video/x-h263",
			"variant", G_TYPE_STRING, "itu",
			"h263version", G_TYPE_STRING, "h263",
			NULL);

	gst_caps_append_structure(caps, struc);

	return caps;
}

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps, *templ;
	struct size {
		int width;
		int height;
	} sizes[] = {
		{ 352, 288 },
		{ 704, 576 },
		{ 176, 144 },
		{ 128, 96 },
	};

	caps = gst_caps_new_empty();
	templ = gst_caps_new_simple("video/x-raw-yuv",
			"format", GST_TYPE_FOURCC, GST_MAKE_FOURCC('I', '4', '2', '0'),
			"pixel-aspect-ratio", GST_TYPE_FRACTION, 12, 11,
			NULL);

	for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
		GstCaps *tmp;
		tmp = gst_caps_copy(templ);
		gst_caps_set_simple(tmp,
				"width", G_TYPE_INT, sizes[i].width,
				"height", G_TYPE_INT, sizes[i].height,
				NULL);
		gst_caps_append(caps, tmp);
	}

	gst_caps_unref(templ);

	return caps;
}

static void
instance_init(GTypeInstance *instance, void *g_class)
{
	struct gst_av_venc *venc = (struct gst_av_venc *)instance;

	venc->codec_id = CODEC_ID_H263;
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;

	gst_element_class_set_details_simple(element_class,
			"av h263 video encoder",
			"Coder/Encoder/Video",
			"H.263 encoder wrapper for libavcodec",
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

static void
class_init(void *g_class, void *class_data)
{
	GstElementClass *gstelement_class = g_class;

	parent_class = g_type_class_ref(GST_TYPE_ELEMENT);

	avcodec_register_all();

	gstelement_class->change_state = change_state;
}

GType
gst_av_h263enc_get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct obj_class),
			.class_init = class_init,
			.base_init = base_init,
			.instance_size = sizeof(struct obj),
			.instance_init = instance_init,
		};

		type = g_type_register_static(GST_AV_VENC_TYPE, "GstAVH263Enc", &type_info, 0);
	}

	return type;
}
