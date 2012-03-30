/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "gstav_venc.h"
#include "plugin.h"

#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <gst/tag/tag.h>

#include <stdlib.h>
#include <string.h> /* for memcpy */

#define GST_CAT_DEFAULT gstav_debug

static GstElementClass *parent_class;

#define obj gst_av_venc

struct obj_class {
	GstElementClass parent_class;
};

static GstFlowReturn
pad_chain(GstPad *pad, GstBuffer *buf)
{
	struct obj *self;
	GstFlowReturn ret = GST_FLOW_OK;
	AVCodecContext *ctx;
	AVFrame *frame = NULL;
	int read;
	GstBuffer *out_buf;

	self = (struct obj *)((GstObject *)pad)->parent;
	ctx = self->av_ctx;

	if (G_UNLIKELY(!self->initialized)) {
		GstCaps *new_caps;
		GstStructure *struc;

		self->initialized = true;
		if (gst_av_codec_open(ctx, self->codec) < 0) {
			ret = GST_FLOW_ERROR;
			goto leave;
		}

		new_caps = gst_pad_get_caps(self->srcpad);
		struc = gst_caps_get_structure(new_caps, 0);

		gst_structure_set(struc,
				"width", G_TYPE_INT, ctx->width,
				"height", G_TYPE_INT, ctx->height,
				NULL);

		if (ctx->time_base.num)
			gst_structure_set(struc,
					"framerate", GST_TYPE_FRACTION,
					ctx->time_base.den, ctx->time_base.num,
					NULL);

		if (ctx->sample_aspect_ratio.num)
			gst_structure_set(struc,
					"pixel-aspect-ratio", GST_TYPE_FRACTION,
					ctx->sample_aspect_ratio.num, ctx->sample_aspect_ratio.den,
					NULL);

		GST_INFO_OBJECT(self, "caps are: %" GST_PTR_FORMAT, new_caps);
		gst_pad_set_caps(self->srcpad, new_caps);
		gst_caps_unref(new_caps);
	}

	frame = avcodec_alloc_frame();
	avpicture_fill((AVPicture *)frame, buf->data, PIX_FMT_YUV420P,
			ctx->width, ctx->height);

	{
		AVRational bq = { 1, GST_SECOND };
		frame->pts = av_rescale_q(buf->timestamp / ctx->ticks_per_frame, bq, ctx->time_base);
	}

	out_buf = gst_buffer_new_and_alloc(ctx->width * ctx->height * 2);
	gst_buffer_set_caps(out_buf, self->srcpad->caps);
	read = avcodec_encode_video(ctx, out_buf->data, out_buf->size, frame);
	if (read < 0) {
		ret = GST_FLOW_ERROR;
		goto leave;
	}

	out_buf->size = read;
	{
		AVRational bq = { 1, GST_SECOND };
		out_buf->timestamp = av_rescale_q(ctx->coded_frame->pts, ctx->time_base, bq);
	}
	ret = gst_pad_push(self->srcpad, out_buf);

leave:
	av_free(frame);
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
			gst_av_codec_close(self->av_ctx);
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
	AVCodecContext *ctx;

	self = (struct obj *)((GstObject *)pad)->parent;

	in_struc = gst_caps_get_structure(caps, 0);

	self->codec = avcodec_find_encoder(self->codec_id);
	if (!self->codec)
		return false;

	self->av_ctx = ctx = avcodec_alloc_context3(self->codec);

	gst_structure_get_int(in_struc, "width", &ctx->width);
	gst_structure_get_int(in_struc, "height", &ctx->height);

	gst_structure_get_fraction(in_struc, "pixel-aspect-ratio",
			&ctx->sample_aspect_ratio.num, &ctx->sample_aspect_ratio.den);

	gst_structure_get_fraction(in_struc, "framerate",
			&ctx->time_base.den, &ctx->time_base.num);

	ctx->pix_fmt = PIX_FMT_YUV420P;
	ctx->rtp_payload_size = 1;
	ctx->me_method = ME_ZERO;

	if (self->init_ctx)
		self->init_ctx(self, ctx);

	return true;
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
class_init(void *g_class, void *class_data)
{
	GstElementClass *gstelement_class = g_class;

	parent_class = g_type_class_ref(GST_TYPE_ELEMENT);

	gstelement_class->change_state = change_state;
}

GType
gst_av_venc_get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct obj_class),
			.class_init = class_init,
			.instance_size = sizeof(struct obj),
			.instance_init = instance_init,
		};

		type = g_type_register_static(GST_TYPE_ELEMENT, "GstAVVideoEnc", &type_info, 0);
	}

	return type;
}
