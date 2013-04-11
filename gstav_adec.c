/*
 * Copyright (C) 2009-2012 Felipe Contreras
 * Copyright (C) 2005 Michael Ahlberg, Måns Rullgård
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "gstav_adec.h"
#include "plugin.h"

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <gst/tag/tag.h>

#include <stdlib.h>
#include <string.h> /* for memcpy */
#include <stdbool.h>

#define GST_CAT_DEFAULT gstav_debug

static GstElementClass *parent_class;

#define BUFFER_SIZE AVCODEC_MAX_AUDIO_FRAME_SIZE
#define MAX_DIFF 20 * 1000000

struct oggvorbis_private {
	unsigned int len[3];
	unsigned char *packet[3];
};

struct ring {
	size_t in, out;
};

struct obj {
	GstElement element;
	GstPad *sinkpad, *srcpad;
	AVCodec *codec;
	AVCodecContext *av_ctx;
	bool got_header;
	struct oggvorbis_private priv;
	uint64_t timestamp;
	AVPacket pkt;
	uint8_t *buffer_data;
	size_t buffer_size;
	struct ring ring;
	int (*header_func)(struct obj *self, GstBuffer *buf);
	uint64_t next_timestamp;
	int bps;
	GMutex *mutex;
};

struct obj_class {
	GstElementClass parent_class;
};

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

static int
default_header(struct obj *self, GstBuffer *buf)
{
	return 0;
}

static unsigned int
fixup_vorbis_headers(struct oggvorbis_private *priv, uint8_t **buf)
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
vorbis_header(struct obj *self, GstBuffer *buf)
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

static int
flac_header(struct obj *self, GstBuffer *buf)
{
	unsigned type = (buf->data[0] & 0x7F);
	/* frame sync code */
	if (type == 127)
		return 0;
	return 1;
}

static inline uint32_t
calculate_duration(struct obj *self, uint64_t size)
{
	uint32_t samples;
	samples = size / (self->av_ctx->channels * (self->bps >> 3));
	return gst_util_uint64_scale_int(samples, GST_SECOND, self->av_ctx->sample_rate);
}

static inline void
calculate_timestamp(struct obj *self, GstBuffer *out_buf)
{
	out_buf->timestamp = self->timestamp;
	out_buf->duration = calculate_duration(self, out_buf->size);
	self->timestamp += out_buf->duration;
}

static void
check_timestamps(struct obj *self, GstBuffer *buf)
{
	if (G_UNLIKELY(self->timestamp == GST_CLOCK_TIME_NONE)) {
		self->next_timestamp = self->timestamp = buf->timestamp;
	} else if (abs(buf->timestamp - self->next_timestamp) > MAX_DIFF) {
		int64_t progress = self->next_timestamp - self->timestamp;

		GST_DEBUG_OBJECT(self, "reseting timestamp: %li ns",
				buf->timestamp - self->next_timestamp);
		self->next_timestamp = self->timestamp = buf->timestamp;
		self->timestamp -= progress;
	}
}

static GstFlowReturn
pad_chain(GstPad *pad, GstBuffer *buf)
{
	struct obj *self;
	GstFlowReturn ret = GST_FLOW_OK;
	AVCodecContext *av_ctx;

	self = (struct obj *)((GstObject *)pad)->parent;
	av_ctx = self->av_ctx;

	if (G_UNLIKELY(!self->got_header)) {
		int hdr = self->header_func(self, buf);
		if (!hdr) {
			GstCaps *new_caps;
			GstStructure *s;
			int bps;

			self->got_header = true;
			if (gst_av_codec_open(av_ctx, self->codec) < 0) {
				ret = GST_FLOW_ERROR;
				goto leave;
			}

#if LIBAVUTIL_VERSION_MAJOR < 51
			bps = av_get_bits_per_sample_format(av_ctx->sample_fmt);
#elif LIBAVUTIL_VERSION_MAJOR < 52 && !(LIBAVUTIL_VERSION_MAJOR == 51 && LIBAVUTIL_VERSION_MINOR >= 4)
			bps = av_get_bits_per_sample_fmt(av_ctx->sample_fmt);
#else
			bps = av_get_bytes_per_sample(av_ctx->sample_fmt) << 3;
#endif

			self->bps = bps;

			s = gst_structure_new("audio/x-raw-int",
					"rate", G_TYPE_INT, av_ctx->sample_rate,
					"channels", G_TYPE_INT, av_ctx->channels,
					"endianness", G_TYPE_INT, G_BYTE_ORDER,
					"width", G_TYPE_INT, bps,
					NULL);

			switch (av_ctx->sample_fmt) {
#if LIBAVCODEC_VERSION_MAJOR < 54
			case SAMPLE_FMT_S16:
			case SAMPLE_FMT_S32:
#else
			case AV_SAMPLE_FMT_S16:
			case AV_SAMPLE_FMT_S32:
#endif
				gst_structure_set(s,
						"signed", G_TYPE_BOOLEAN, TRUE,
						"depth", G_TYPE_INT, bps,
						NULL);
				break;
			default:
				break;
			}

			new_caps = gst_caps_new_full(s, NULL);

			GST_INFO_OBJECT(self, "caps are: %" GST_PTR_FORMAT, new_caps);
			gst_pad_set_caps(self->srcpad, new_caps);
			gst_caps_unref(new_caps);
		}
	}

	if (G_LIKELY(self->got_header)) {
		AVPacket pkt;

		av_init_packet(&pkt);
		pkt.data = self->pkt.data;
		pkt.size = buf->size;

		memcpy(pkt.data, buf->data, buf->size);
		memset(pkt.data + pkt.size, 0, FF_INPUT_BUFFER_PADDING_SIZE);

		check_timestamps(self, buf);

		if (buf->duration != GST_CLOCK_TIME_NONE)
			self->next_timestamp += buf->duration;

		do {
			void *buffer_data;
			int buffer_size;
			int read;
			unsigned total_buffer_size;

			buffer_data = self->buffer_data + self->ring.in;
			buffer_size = self->buffer_size - self->ring.in;
#if LIBAVCODEC_VERSION_MAJOR < 54 && !(LIBAVCODEC_VERSION_MAJOR == 53 && LIBAVCODEC_VERSION_MINOR >= 25)
			g_mutex_lock(self->mutex);
			read = avcodec_decode_audio3(av_ctx, buffer_data, &buffer_size, &pkt);
			g_mutex_unlock(self->mutex);
			if (read < 0) {
				GST_WARNING_OBJECT(self, "error: %i", read);
				break;
			}
#else
			AVFrame frame;
			int got_frame = 0, planar, plane_size;

			g_mutex_lock(self->mutex);
			read = avcodec_decode_audio4(av_ctx, &frame, &got_frame, &pkt);
			g_mutex_unlock(self->mutex);
			if (read < 0) {
				GST_WARNING_OBJECT(self, "error: %i", read);
				break;
			}

			if (!got_frame)
				goto next;

			planar = av_sample_fmt_is_planar(av_ctx->sample_fmt);
			buffer_size = av_samples_get_buffer_size(&plane_size, av_ctx->channels,
					frame.nb_samples, av_ctx->sample_fmt, 1);

			memcpy(buffer_data, frame.extended_data[0], plane_size);

			if (planar && av_ctx->channels > 1) {
				uint8_t *out = ((uint8_t *)buffer_data) + plane_size;
				for (int ch = 1; ch < av_ctx->channels; ch++) {
					memcpy(out, frame.extended_data[ch], plane_size);
					out += plane_size;
				}
			}
#endif

			if (buf->duration == GST_CLOCK_TIME_NONE)
				self->next_timestamp += calculate_duration(self, buffer_size);

			self->ring.in += buffer_size;
			if (self->ring.in >= 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE) {
				memmove(self->buffer_data,
						self->buffer_data + self->ring.out,
						self->ring.in - self->ring.out);
				self->ring.in -= self->ring.out;
				self->ring.out = 0;
			}

			if (BUFFER_SIZE > 0)
				total_buffer_size = BUFFER_SIZE;
			else
				total_buffer_size = buffer_size;

			if (self->ring.in - self->ring.out >= total_buffer_size) {
				GstBuffer *out_buf;
				out_buf = gst_buffer_new_and_alloc(total_buffer_size);
				memcpy(out_buf->data, self->buffer_data + self->ring.out, out_buf->size);
				calculate_timestamp(self, out_buf);
				gst_buffer_set_caps(out_buf, self->srcpad->caps);

				self->ring.out += out_buf->size;

				ret = gst_pad_push(self->srcpad, out_buf);
			}
#if LIBAVCODEC_VERSION_MAJOR >= 54 || (LIBAVCODEC_VERSION_MAJOR == 53 && LIBAVCODEC_VERSION_MINOR >= 25)
next:
#endif
			pkt.size -= read;
			pkt.data += read;
		} while (pkt.size > 0);
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
		self->got_header = false;
		av_new_packet(&self->pkt, AVCODEC_MAX_AUDIO_FRAME_SIZE);
		self->buffer_size = 3 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
		self->buffer_data = av_malloc(self->buffer_size);
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
			av_freep(&self->av_ctx->extradata);
			av_freep(&self->av_ctx);
		}
		av_free_packet(&self->pkt);
		av_freep(&self->buffer_data);
		break;

	default:
		break;
	}

	return ret;
}

static gboolean
sink_event(GstPad *pad, GstEvent *event)
{
	struct obj *self;
	gboolean ret = TRUE;

	self = (struct obj *)(gst_pad_get_parent(pad));

	switch (event->type) {
	case GST_EVENT_FLUSH_START:
		self->timestamp = GST_CLOCK_TIME_NONE;
		g_mutex_lock(self->mutex);
		avcodec_flush_buffers(self->av_ctx);
		g_mutex_unlock(self->mutex);
		break;
	case GST_EVENT_EOS: {
		/* flush current buffer */
		GstBuffer *out_buf;
		GstFlowReturn r;

		out_buf = gst_buffer_new_and_alloc(self->ring.in - self->ring.out);
		memcpy(out_buf->data, self->buffer_data + self->ring.out, out_buf->size);
		calculate_timestamp(self, out_buf);
		gst_buffer_set_caps(out_buf, self->srcpad->caps);

		self->ring.out += out_buf->size;

		r = gst_pad_push(self->srcpad, out_buf);
		if (r != GST_FLOW_OK)
			goto leave;
		break;
	}
	default:
		break;
	}

	ret = gst_pad_push_event(self->srcpad, event);

leave:
	gst_object_unref(self);

	return ret;
}

static gboolean
sink_setcaps(GstPad *pad, GstCaps *caps)
{
	struct obj *self;
	GstStructure *in_struc;
	const char *name;
	int codec_id;
	AVCodecContext *ctx;
	const GValue *codec_data;
	GstBuffer *buf;

	self = (struct obj *)((GstObject *)pad)->parent;

	in_struc = gst_caps_get_structure(caps, 0);

	name = gst_structure_get_name(in_struc);
	if (strcmp(name, "audio/x-vorbis") == 0)
		codec_id = CODEC_ID_VORBIS;
	else if (strcmp(name, "audio/x-flac") == 0)
		codec_id = CODEC_ID_FLAC;
	else if (strcmp(name, "audio/mpeg") == 0) {
		int version;
		gst_structure_get_int(in_struc, "mpegversion", &version);
		switch (version) {
		case 1:
			codec_id = CODEC_ID_MP3;
			break;
		case 2:
		case 4:
			codec_id = CODEC_ID_AAC;
			break;
		default:
			codec_id = CODEC_ID_NONE;
		}
	} else
		codec_id = CODEC_ID_NONE;

	self->codec = avcodec_find_decoder(codec_id);
	if (!self->codec)
		return false;

	self->av_ctx = ctx = avcodec_alloc_context3(self->codec);

	switch (codec_id) {
	case CODEC_ID_VORBIS:
		self->header_func = vorbis_header;
		break;
	case CODEC_ID_FLAC: {
		const GValue *stream_header;
		const GValue *stream_info;
		GstBuffer *buf;

		self->header_func = flac_header;

		stream_header = gst_structure_get_value(in_struc, "streamheader");
		if (!stream_header)
			return true;

		stream_info = gst_value_array_get_value(stream_header, 0);

		buf = gst_value_get_buffer(stream_info);
		buf->data += 17;
		buf->size -= 17;

		ctx->extradata = malloc(buf->size + FF_INPUT_BUFFER_PADDING_SIZE);
		memcpy(ctx->extradata, buf->data, buf->size);
		memset(ctx->extradata + buf->size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
		ctx->extradata_size = buf->size;
		break;
	}
	case CODEC_ID_MP3:
	case CODEC_ID_AAC: {
		gst_structure_get_int(in_struc, "rate", &ctx->sample_rate);
		gst_structure_get_int(in_struc, "channels", &ctx->channels);
		break;
	}
	}

	codec_data = gst_structure_get_value(in_struc, "codec_data");
	if (!codec_data)
		goto next;
	buf = gst_value_get_buffer(codec_data);
	if (!buf)
		goto next;
	ctx->extradata = av_malloc(buf->size + FF_INPUT_BUFFER_PADDING_SIZE);
	memcpy(ctx->extradata, buf->data, buf->size);
	memset(ctx->extradata + buf->size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
	ctx->extradata_size = buf->size;

next:
	return true;
}

static GstCaps *
generate_src_template(void)
{
	GstCaps *caps = NULL;

	caps = gst_caps_new_simple("audio/x-raw-int",
			"rate", GST_TYPE_INT_RANGE, 8000, 96000,
			"signed", G_TYPE_BOOLEAN, TRUE,
			"endianness", G_TYPE_INT, G_BYTE_ORDER,
			"width", GST_TYPE_INT_RANGE, 16, 32,
			"depth", GST_TYPE_INT_RANGE, 16, 32,
			"channels", GST_TYPE_INT_RANGE, 1, 256,
			NULL);

	return caps;
}

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("audio/x-vorbis",
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("audio/x-flac",
			"framed", G_TYPE_BOOLEAN, TRUE,
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("audio/mpeg",
			"mpegversion", G_TYPE_INT, 1,
			"layer", GST_TYPE_INT_RANGE, 1, 3,
			"parsed", G_TYPE_BOOLEAN, TRUE,
			NULL);

	gst_caps_append_structure(caps, struc);

	struc = gst_structure_new("audio/mpeg",
			"mpegversion", G_TYPE_INT, 4,
			"framed", G_TYPE_BOOLEAN, TRUE,
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
	gst_pad_set_event_function(self->sinkpad, sink_event);

	self->srcpad =
		gst_pad_new_from_template(gst_element_class_get_pad_template(element_class, "src"), "src");

	gst_pad_use_fixed_caps(self->srcpad);

	gst_element_add_pad((GstElement *)self, self->sinkpad);
	gst_element_add_pad((GstElement *)self, self->srcpad);

	gst_pad_set_setcaps_function(self->sinkpad, sink_setcaps);

	self->header_func = default_header;
	self->timestamp = GST_CLOCK_TIME_NONE;
	self->mutex = g_mutex_new();
}

static void
finalize(GObject *obj)
{
	struct obj *self = (struct obj *)obj;
	g_mutex_free(self->mutex);
	((GObjectClass *)parent_class)->finalize(obj);
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;

	gst_element_class_set_details_simple(element_class,
			"av audio decoder",
			"Coder/Decoder/Audio",
			"Audio decoder wrapper for libavcodec",
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
	GObjectClass *gobject_class = g_class;

	parent_class = g_type_class_ref(GST_TYPE_ELEMENT);

	gstelement_class->change_state = change_state;
	gobject_class->finalize = finalize;
}

GType
gst_av_adec_get_type(void)
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

		type = g_type_register_static(GST_TYPE_ELEMENT, "GstAVAudioDec", &type_info, 0);
	}

	return type;
}
