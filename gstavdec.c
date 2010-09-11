/*
 * Copyright (C) 2009-2010 Felipe Contreras
 * Copyright (C) 2005 Michael Ahlberg, Måns Rullgård
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "gstavdec.h"
#include "plugin.h"

#include <gst/tag/tag.h>

#include <stdlib.h>
#include <string.h> /* for memcpy */

#define GST_CAT_DEFAULT gstav_debug

static GstElementClass *parent_class;

#define BUFFER_SIZE 0x20000

static inline uint8_t get_byte(const uint8_t **b)
{
	return *((*b)++);
}

static inline uint32_t get_le32(const uint8_t **b)
{
	struct unaligned_32 { uint32_t l; } __attribute__((packed));
	*b += sizeof(uint32_t);
	return ((const struct unaligned_32 *)(*b - sizeof(uint32_t)))->l;
}

static unsigned int
fixup_vorbis_headers(struct oggvorbis_private *priv,
		uint8_t **buf)
{
	int i, offset, len;
	unsigned char *ptr;

	len = priv->len[0] + priv->len[1] + priv->len[2];
	ptr = *buf = g_malloc0(len + len / 255 + 64);

	ptr[0] = 2;
	offset = 1;
	offset += av_xiphlacing(&ptr[offset], priv->len[0]);
	offset += av_xiphlacing(&ptr[offset], priv->len[1]);
	for (i = 0; i < 3; i++) {
		memcpy(&ptr[offset], priv->packet[i], priv->len[i]);
		offset += priv->len[i];
		g_free(priv->packet[i]);
	}
	*buf = g_realloc(*buf, offset + FF_INPUT_BUFFER_PADDING_SIZE);
	return offset;
}

static int
vorbis_header(GstAVDec *self,
		GstBuffer *buf)
{
	const uint8_t *p = buf->data;
	struct oggvorbis_private *priv = &self->priv;
	int pkt_type = *p;

	if (!(pkt_type & 1))
		return 0;

	if (buf->size < 1 || pkt_type > 5)
		return -1;

	priv->len[pkt_type >> 1] = buf->size;
	priv->packet[pkt_type >> 1] = g_malloc0(buf->size);
	memcpy(priv->packet[pkt_type >> 1], buf->data, buf->size);

	if (pkt_type == 1) {
		/* tag */
		unsigned blocksize, bs0, bs1;
		p += 7; /* skip "\001vorbis" tag */

		if (buf->size != 30)
			return -1;

		if (get_le32(&p) != 0)
			return -1;

		self->av_ctx->channels = get_byte(&p);
		self->av_ctx->sample_rate = get_le32(&p);
		p += 4; /* max bitrate */
		self->av_ctx->bit_rate = get_le32(&p);
		p += 4; /* min bitrate */

		blocksize = get_byte(&p);
		bs0 = blocksize & 15;
		bs1 = blocksize >> 4;

		if (bs0 > bs1)
			return -1;
		if (bs0 < 6 || bs1 > 13)
			return -1;

		if (get_byte(&p) != 1)
			return -1;
	}
	else if (pkt_type == 3) {
		/* comment */
	}
	else {
		/* extradata */
		self->av_ctx->extradata_size =
			fixup_vorbis_headers(&self->priv, &self->av_ctx->extradata);
	}

	return 1;
}

static inline void
calculate_timestamp(GstAVDec *self,
		GstBuffer *out_buf)
{
	gint64 samples;

	samples = out_buf->size / (self->av_ctx->channels * sizeof(int16_t));

	out_buf->offset = self->granulepos - samples;
	out_buf->offset_end = self->granulepos;

	out_buf->timestamp = gst_util_uint64_scale_int(self->granulepos - samples,
			GST_SECOND, self->av_ctx->sample_rate);
	out_buf->duration = gst_util_uint64_scale_int(samples,
			GST_SECOND, self->av_ctx->sample_rate);
	self->granulepos += samples;
}

static GstFlowReturn
pad_chain(GstPad *pad,
		GstBuffer *buf)
{
	GstAVDec *self;
	GstFlowReturn ret = GST_FLOW_OK;

	self = (GstAVDec *)((GstObject *)pad)->parent;

	if (!self->got_header) {
		int hdr = vorbis_header(self, buf);
		if (!hdr) {
			self->got_header = true;
			if (avcodec_open(self->av_ctx, self->codec) < 0) {
				g_error("fail open");
				ret = GST_FLOW_ERROR;
				goto leave;
			}

			{
				GstCaps *new_caps;

				new_caps = gst_caps_new_simple("audio/x-raw-int",
						"rate", G_TYPE_INT, self->av_ctx->sample_rate,
						"signed", G_TYPE_BOOLEAN, TRUE,
						"channels", G_TYPE_INT, self->av_ctx->channels,
						"endianness", G_TYPE_INT, G_BYTE_ORDER,
						"width", G_TYPE_INT, 16,
						"depth", G_TYPE_INT, 16,
						NULL);

				GST_INFO_OBJECT(self, "caps are: %" GST_PTR_FORMAT, new_caps);
				gst_pad_set_caps(self->srcpad, new_caps);
			}
		}
	}

	if (self->got_header) {
		AVPacket pkt;
		void *buffer_data = self->pkt.data + self->ring.in;
		int buffer_size = self->pkt.size;

		av_init_packet(&pkt);
		pkt.data = buf->data;
		pkt.size = buf->size;
		avcodec_decode_audio3(self->av_ctx, buffer_data, &buffer_size, &pkt);

		self->ring.in += buffer_size;
		if (self->ring.in >= AVCODEC_MAX_AUDIO_FRAME_SIZE - 0x2000) {
			memcpy(self->pkt.data,
					self->pkt.data + self->ring.out,
					self->ring.in - self->ring.out);
			self->ring.in -= self->ring.out;
			self->ring.out = 0;
		}

		if (buf->offset_end != GST_BUFFER_OFFSET_NONE)
			self->granulepos = buf->offset_end;

		if (self->ring.in - self->ring.out >= BUFFER_SIZE) {
			GstBuffer *out_buf;
			out_buf = gst_buffer_new();
			out_buf->data = self->pkt.data + self->ring.out;
			out_buf->size = BUFFER_SIZE;
			calculate_timestamp(self, out_buf);
			gst_buffer_set_caps(out_buf, self->srcpad->caps);

			self->ring.out += out_buf->size;

			ret = gst_pad_push(self->srcpad, out_buf);
		}
	}

leave:
	gst_buffer_unref(buf);

	return ret;
}

static GstStateChangeReturn
change_state(GstElement *element,
		GstStateChange transition)
{
	GstStateChangeReturn ret;
	GstAVDec *self;

	self = (GstAVDec *)element;

	switch (transition) {
	case GST_STATE_CHANGE_NULL_TO_READY:
		self->codec = avcodec_find_decoder(CODEC_ID_VORBIS);
		if (!self->codec)
			return GST_STATE_CHANGE_FAILURE;
		self->av_ctx = avcodec_alloc_context();
		self->got_header = false;
		av_new_packet(&self->pkt, AVCODEC_MAX_AUDIO_FRAME_SIZE);
		break;

	default:
		break;
	}

	ret = parent_class->change_state(element, transition);

	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition) {
	case GST_STATE_CHANGE_READY_TO_NULL:
		av_free_packet(&self->pkt);
		/** @todo how exactly do we do this? */
#if 0
		if (self->av_ctx)
			avcodec_close(self->av_ctx);
#endif
		break;

	default:
		break;
	}

	return ret;
}

static GstCaps *
generate_src_template(void)
{
	GstCaps *caps = NULL;

	caps = gst_caps_new_simple("audio/x-raw-int",
			"rate", GST_TYPE_INT_RANGE, 8000, 96000,
			"signed", G_TYPE_BOOLEAN, TRUE,
			"endianness", G_TYPE_INT, G_BYTE_ORDER,
			"width", G_TYPE_INT, 16,
			"depth", G_TYPE_INT, 16,
			"channels", GST_TYPE_INT_RANGE, 1, 256,
			NULL);

	return caps;
}

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps = NULL;

	caps = gst_caps_new_simple("audio/x-vorbis",
			NULL);

	return caps;
}

static void
instance_init(GTypeInstance *instance,
		gpointer g_class)
{
	GstAVDec *self = (GstAVDec *)instance;
	GstElementClass *element_class = g_class;

	self->sinkpad =
		gst_pad_new_from_template(gst_element_class_get_pad_template(element_class, "sink"), "sink");

	gst_pad_set_chain_function(self->sinkpad, pad_chain);

	self->srcpad =
		gst_pad_new_from_template(gst_element_class_get_pad_template(element_class, "src"), "src");

	gst_pad_use_fixed_caps(self->srcpad);

	gst_element_add_pad((GstElement *)self, self->sinkpad);
	gst_element_add_pad((GstElement *)self, self->srcpad);
}

static void
base_init(gpointer g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;
	GstElementDetails details;

	details.longname = "avdec element";
	details.klass = "Coder/Decoder/Audio";
	details.description = "AVCodec stuff";
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
class_init(gpointer g_class,
		gpointer class_data)
{
	GstElementClass *gstelement_class = g_class;

	parent_class = g_type_class_ref(GST_TYPE_ELEMENT);

	avcodec_register_all();

	gstelement_class->change_state = change_state;
}

GType
gst_avdec_get_type(void)
{
	static GType type = 0;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(GstAVDecClass),
			.class_init = class_init,
			.base_init = base_init,
			.instance_size = sizeof(GstAVDec),
			.instance_init = instance_init,
		};

		type = g_type_register_static(GST_TYPE_ELEMENT, "GstAVDec", &type_info, 0);
	}

	return type;
}
