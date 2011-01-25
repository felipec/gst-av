/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "gstav_vdec.h"
#include "plugin.h"

#include <libavcodec/avcodec.h>
#include <gst/tag/tag.h>

#include <stdlib.h>
#include <string.h> /* for memcpy */
#include <stdbool.h>

#define GST_CAT_DEFAULT gstav_debug

static GstElementClass *parent_class;

struct obj {
	GstElement element;
	GstPad *sinkpad, *srcpad;
	AVCodec *codec;
	AVCodecContext *av_ctx;
	bool initialized;
};

struct obj_class {
	GstElementClass parent_class;
};

/* TODO there must be a more straight-forward way */
static GstBuffer *convert_frame(struct obj *self, AVFrame *frame)
{
	AVCodecContext *ctx;
	int i;
	GstBuffer *out_buf;
	guint8 *p;

	ctx = self->av_ctx;
	out_buf = gst_buffer_new_and_alloc(ctx->width * ctx->height * 3 / 2);
	gst_buffer_set_caps(out_buf, self->srcpad->caps);

	for (i = 0; i < ctx->height * ctx->width * 3 / 2; i++)
		out_buf->data[i] = 0;
	p = out_buf->data;
	for (i = 0; i < ctx->height; i++)
		memcpy(p + i * ctx->width, frame->data[0] + i * frame->linesize[0], ctx->width);
	p = out_buf->data + ctx->width * ctx->height;
	for (i = 0; i < ctx->height / 2; i++)
		memcpy(p + i * ctx->width / 2, frame->data[1] + i * frame->linesize[1], ctx->width / 2);
	p = out_buf->data + ctx->width * ctx->height * 5 / 4;
	for (i = 0; i < ctx->height / 2; i++)
		memcpy(p + i * ctx->width / 2, frame->data[2] + i * frame->linesize[2], ctx->width / 2);

	return out_buf;
}

static GstFlowReturn
pad_chain(GstPad *pad, GstBuffer *buf)
{
	struct obj *self;
	GstFlowReturn ret = GST_FLOW_OK;
	AVCodecContext *ctx;
	AVFrame *frame;
	int got_pic;
	AVPacket pkt;
	int read;

	self = (struct obj *)((GstObject *)pad)->parent;
	ctx = self->av_ctx;

	if (G_UNLIKELY(!self->initialized)) {
		GstCaps *new_caps;

		self->initialized = true;
		if (avcodec_open(ctx, self->codec) < 0) {
			ret = GST_FLOW_ERROR;
			goto leave;
		}

		new_caps = gst_caps_new_simple("video/x-raw-yuv",
				"width", G_TYPE_INT, ctx->width,
				"height", G_TYPE_INT, ctx->height,
				"format", GST_TYPE_FOURCC, GST_MAKE_FOURCC('I','4','2','0'),
				"framerate", GST_TYPE_FRACTION, 0, 1,
				NULL);

		GST_INFO_OBJECT(self, "caps are: %" GST_PTR_FORMAT, new_caps);
		gst_pad_set_caps(self->srcpad, new_caps);
		gst_caps_unref(new_caps);
	}

	av_init_packet(&pkt);
	pkt.data = buf->data;
	pkt.size = buf->size;

	frame = avcodec_alloc_frame();

	read = avcodec_decode_video2(ctx, frame, &got_pic, &pkt);
	if (read < 0) {
		ret = GST_FLOW_ERROR;
		goto leave;
	}

	if (got_pic) {
		GstBuffer *out_buf;
		out_buf = convert_frame(self, frame);
		out_buf->timestamp = buf->timestamp;
		out_buf->duration = buf->duration;
		ret = gst_pad_push(self->srcpad, out_buf);
	}

leave:
	gst_buffer_unref(buf);

	return ret;
}

static GstStateChangeReturn
change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret;
	struct obj *self;

	self = (struct obj *)element;

	switch (transition) {
	case GST_STATE_CHANGE_NULL_TO_READY:
		self->av_ctx = avcodec_alloc_context();
		self->initialized = false;
		break;

	default:
		break;
	}

	ret = parent_class->change_state(element, transition);

	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition) {
	case GST_STATE_CHANGE_READY_TO_NULL:
		if (self->av_ctx) {
			avcodec_close(self->av_ctx);
			av_freep(&self->av_ctx);
		}
		break;

	default:
		break;
	}

	return ret;
}

static gboolean
sink_setcaps(GstPad *pad, GstCaps *caps)
{
	struct obj *self;
	GstStructure *in_struc;
	const char *name;
	int codec_id;
	const GValue *codec_data;
	GstBuffer *buf;
	AVCodecContext *ctx;

	self = (struct obj *)((GstObject *)pad)->parent;
	ctx = self->av_ctx;

	in_struc = gst_caps_get_structure(caps, 0);

	gst_structure_get_int(in_struc, "width", &ctx->width);
	gst_structure_get_int(in_struc, "height", &ctx->height);

	codec_data = gst_structure_get_value(in_struc, "codec_data");
	if (!codec_data)
		goto next;
	buf = gst_value_get_buffer(codec_data);
	if (!buf)
		goto next;
	ctx->extradata = malloc(buf->size + FF_INPUT_BUFFER_PADDING_SIZE);
	memcpy(ctx->extradata, buf->data, buf->size);
	ctx->extradata_size = buf->size;

next:
	name = gst_structure_get_name(in_struc);
	if (strcmp(name, "video/x-h263") == 0)
		codec_id = CODEC_ID_H263;
	else if (strcmp(name, "video/x-h264") == 0)
		codec_id = CODEC_ID_H264;
	else if (strcmp(name, "video/mpeg") == 0) {
		int version;
		gst_structure_get_int(in_struc, "mpegversion", &version);
		switch (version) {
		case 4:
			codec_id = CODEC_ID_MPEG4;
			break;
		case 2:
			codec_id = CODEC_ID_MPEG2VIDEO;
			break;
		case 1:
			codec_id = CODEC_ID_MPEG1VIDEO;
			break;
		default:
			codec_id = CODEC_ID_NONE;
			break;
		}
	}
	else if (strcmp(name, "video/x-divx") == 0) {
		int version;
		gst_structure_get_int(in_struc, "divxversion", &version);
		switch (version) {
		case 5:
		case 4:
			codec_id = CODEC_ID_MPEG4;
			break;
		case 3:
			codec_id = CODEC_ID_MSMPEG4V3;
			break;
		default:
			codec_id = CODEC_ID_NONE;
			break;
		}
	}
	else if (strcmp(name, "video/x-xvid") == 0)
		codec_id = CODEC_ID_XVID;
	else if (strcmp(name, "video/x-3ivx") == 0)
		codec_id = CODEC_ID_MPEG4;
	else if (strcmp(name, "video/x-wmv") == 0) {
		int version;
		gst_structure_get_int(in_struc, "wmvversion", &version);
		switch (version) {
		case 3: {
			guint32 fourcc;
			codec_id = CODEC_ID_WMV3;
			if (gst_structure_get_fourcc(in_struc, "fourcc", &fourcc) ||
					gst_structure_get_fourcc(in_struc, "format", &fourcc))
			{
				if (fourcc == GST_MAKE_FOURCC('W', 'V', 'C', '1'))
					codec_id = CODEC_ID_VC1;
			}
			break;
		}
		case 2:
			codec_id = CODEC_ID_WMV2;
			break;
		case 1:
			codec_id = CODEC_ID_WMV1;
			break;
		default:
			codec_id = CODEC_ID_NONE;
			break;
		}

	}
	else
		codec_id = CODEC_ID_NONE;

	self->codec = avcodec_find_decoder(codec_id);
	if (!self->codec)
		return false;

	return true;
}

static GstCaps *
generate_src_template(void)
{
	GstCaps *caps = NULL;

	caps = gst_caps_new_simple("video/x-raw-yuv",
			"format", GST_TYPE_FOURCC, GST_MAKE_FOURCC('I', '4', '2', '0'),
			NULL);

	return caps;
}

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("video/x-h263",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("video/x-h264",
			"alignment", G_TYPE_STRING, "au",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("video/mpeg",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("video/x-divx",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("video/x-xvid",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("video/x-3ivx",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("video/x-wmv",
			NULL);

	gst_caps_append_structure(caps, struc);

	return caps;
}

static void
instance_init(GTypeInstance *instance, void *g_class)
{
	struct obj *self = (struct obj *)instance;
	GstElementClass *element_class = g_class;

	self->sinkpad =
		gst_pad_new_from_template(gst_element_class_get_pad_template(element_class, "sink"), "sink");

	gst_pad_set_chain_function(self->sinkpad, pad_chain);

	self->srcpad =
		gst_pad_new_from_template(gst_element_class_get_pad_template(element_class, "src"), "src");

	gst_pad_use_fixed_caps(self->srcpad);

	gst_element_add_pad((GstElement *)self, self->sinkpad);
	gst_element_add_pad((GstElement *)self, self->srcpad);

	gst_pad_set_setcaps_function(self->sinkpad, sink_setcaps);
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;
	GstElementDetails details;

	details.longname = "av video decoder";
	details.klass = "Coder/Decoder/Video";
	details.description = "Video decoder wrapper for libavcodec";
	details.author = "Felipe Contreras";

	gst_element_class_set_details(element_class, &details);

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
gst_av_vdec_get_type(void)
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

		type = g_type_register_static(GST_TYPE_ELEMENT, "GstAVVideoDec", &type_info, 0);
	}

	return type;
}
