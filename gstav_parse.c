/*
 * Copyright (C) 2007-2010 Nokia Corporation
 * Copyright (C) 2009 Marco Ballesio
 *
 * Authors:
 * Juha Alanen <juha.m.alanen@nokia.com>
 * Marco Ballesio <marco.ballesio@nokia.com>
 * Felipe Contreras <felipe.contreras@nokia.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#include <gst/gst.h>
#include <stdlib.h>
#include <string.h>

#include "gstav_vdec.h"

#include "gstav_parse.h"
#include "get_bits.h"

static inline void
set_framesize(struct gst_av_vdec *vdec,
		int width, int height,
		int par_num, int par_den,
		int crop_width, int crop_height)
{
	AVCodecContext *ctx = vdec->av_ctx;
	gint w = crop_width ? crop_width : width;
	gint h = crop_height ? crop_height : height;

	ctx->width = w;
	ctx->height = h;

	/* why does it crash? */
#if 0
	ctx->coded_width = width;
	ctx->coded_height = height;
#endif
}

bool gst_av_h263_parse(struct gst_av_vdec *vdec, GstBuffer *buf)
{
	struct get_bit_context s;
	unsigned bits;
	unsigned type;
	bool baseline = true;
	int width, height;
	int par_num = 11, par_den = 12;
	struct size {
		int width;
		int height;
	} sizes[] = {
		{ 0, 0 },
		{ 128, 96 },
		{ 176, 144 },
		{ 352, 288 },
		{ 704, 576 },
		{ 1408, 1152 },
	};
	struct par {
		int num;
		int den;
	} pars[] = {
		{ 0, 0 },
		{ 1, 1 },
		{ 12, 11 },
		{ 10, 11 },
		{ 16, 11 },
		{ 40, 33 },
	};

	init_get_bits(&s, buf->data, buf->size * 8);

	if (get_bits_left(&s) < 38)
		goto not_enough;

	/* picture start code */
	if (get_bits(&s, 22) != 0x20)
		goto bail;

	/* temporal reference */
	skip_bits(&s, 8);

	type = get_bits(&s, 8) & 0x7;
	switch (type) {
	case 0:
	case 6:
		/* forbidden or reserved */
		goto bail;
	case 7: {
		unsigned custom_pf;
		bool extended_par = false;

		/* extended */
		baseline = false;

		if (get_bits_left(&s) < 54)
			goto not_enough;

		/* Updated Full Extended PTYPE */

		if (get_bits(&s, 3) != 1) {
			/* spec wise, should be present at start */
			goto bail;
		}

		type = get_bits(&s, 18) >> 15;
		if (type == 0 || type == 7) {
			goto bail;
		} else if (type != 6) {
			width = sizes[type].width;
			height = sizes[type].height;
			/* have all we need, exit */
			goto exit;
		}

		/* mandatory PLUSPTYPE part */

		skip_bits(&s, 9);

		/* CPM */
		if (get_bits1(&s)) {
			if (get_bits_left(&s) < 25)
				goto not_enough;

			/* PSBI */
			skip_bits(&s, 2);
		}

		custom_pf = get_bits(&s, 4);
		if (custom_pf == 0x0F) {
			extended_par = true;
		} else if (custom_pf && custom_pf < 6) {
			par_num = pars[custom_pf].num;
			par_den = pars[custom_pf].den;
		}

		bits = get_bits(&s, 19);
		height = (bits & 0x1FF) * 4;
		bits >>= 10;
		width = ((bits & 0x1FF) + 1) * 4;
		if (!extended_par)
			goto exit;

		if (get_bits_left(&s) < 16)
			goto not_enough;

		bits = get_bits(&s, 16);
		if (bits) {
			par_num = bits >> 8;
			par_den = bits & 0x0F;
		}
		break;
	}
	default:
		/* regular */

		width = sizes[type].width;
		height = sizes[type].height;

		if (get_bits_left(&s) < 11)
			goto not_enough;

		/* optional PTYPE part */
		baseline = !(get_bits(&s, 5) & 1);

		/* PQUANT */
		skip_bits(&s, 5);

		/* CPM */
		baseline = baseline && !get_bits1(&s);
		break;
	}

exit:
	set_framesize(vdec, width, height, par_num, par_den, 0, 0);
	return true;

not_enough:
bail:
	return false;
}

static inline bool mpeg4_next_start_code(struct get_bit_context *s)
{
	if (get_bits_left(s) < 8 - (int) s->index % 8)
		goto failed;
	if (get_bits1(s))
		goto failed;

	while (s->index % 8 != 0) {
		if (!get_bits1(s))
			goto failed;
	}

	return true;

failed:
	return false;
}

static inline bool mpeg4_skip_user_data(struct get_bit_context *s, unsigned *bits)
{
	while (*bits == 0x1B2) {
		do {
			unsigned b;
			if (get_bits_left(s) < 8)
				goto failed;
			b = get_bits(s, 8);
			*bits = (*bits << 8) | b;
		} while ((*bits >> 8) != 0x1);
	}

	return true;

failed:
	return false;
}

bool gst_av_mpeg4_parse(struct gst_av_vdec *vdec, GstBuffer *buf)
{
	struct get_bit_context s;
	unsigned bits;
	int time_increment_resolution;
	int width, height;
	unsigned ar;

	init_get_bits(&s, buf->data, buf->size * 8);

	if (get_bits_left(&s) < 32)
		goto failed;

	/* Expect Visual Object Sequence startcode (0x000001B0) */
	bits = get_bits(&s, 32);
	if (bits != 0x1B0) {
		unsigned i;

		/* find Video Object startcode and take it from there */
		for (i = 4; i < buf->size; i++) {
			if (G_UNLIKELY(bits <= 0x11F)) {
				init_get_bits(&s, buf->data + i - 4, (buf->size - i + 4) * 8);
				goto VOS;
			}
			bits = (bits << 8) | buf->data[i];
		}
		goto failed;
	}

	if (get_bits_left(&s) < 40)
		goto failed;

	/* profile and level indication */
	bits = get_bits(&s, 8);

	/* Expect Visual Object startcode (0x000001B5) */
	bits = get_bits(&s, 32);
	/* but skip optional user data */
	if (!mpeg4_skip_user_data(&s, &bits))
		goto failed;
	if (bits != 0x1B5)
		goto failed;

	if (get_bits_left(&s) < 6)
		goto failed;
	if (get_bits1(&s)) {
		if (get_bits_left(&s) < 12)
			goto failed;
		/* Skip visual_object_verid and priority */
		skip_bits(&s, 7);
	}

	/* Only support video ID */
	if (get_bits(&s, 4) != 1)
		goto failed;

	/* video signal type */
	if (get_bits1(&s)) {
		if (get_bits_left(&s) < 5)
			goto failed;

		/* video signal type, ignore format and range */
		skip_bits(&s, 4);

		if (get_bits1(&s)) {
			if (get_bits_left(&s) < 24)
				goto failed;
			/* ignore color description */
			skip_bits(&s, 24);
		}
	}

	if (!mpeg4_next_start_code(&s))
		goto failed;

VOS:
	if (get_bits_left(&s) < 32)
		goto failed;

	/* expecting a video object startcode */
	bits = get_bits(&s, 32);
	/* skip optional user data */
	if (!mpeg4_skip_user_data(&s, &bits))
		goto failed;
	if (bits > 0x11F)
		goto failed;

	if (get_bits_left(&s) < 47)
		goto failed;
	/* expecting a video object layer startcode */
	bits = get_bits(&s, 32);
	if (bits < 0x120 || bits > 0x12F)
		goto failed;

	/* ignore random accessible vol and video object type indication */
	skip_bits(&s, 9);

	if (get_bits1(&s)) {
		if (get_bits_left(&s) < 12)
			goto failed;
		/* skip video object layer verid and priority */
		skip_bits(&s, 7);
	}

	/* aspect ratio */
	ar = get_bits(&s, 4);
	if (ar == 0) {
		goto failed;
	} else if (ar == 0xf) {
		/* info is extended par */
		if (get_bits_left(&s) < 17)
			goto failed;
		/* aspect_ratio_width */
		skip_bits(&s, 8);
		/* aspect_ratio_height */
		skip_bits(&s, 8);
	} else if (ar < 0x6) {
		/* TODO get aspect ratio width and height from aspect ratio table */
	}

	if (get_bits1(&s)) {
		if (get_bits_left(&s) < 4)
			goto failed;
		/* vol control parameters, skip chroma and low delay */
		skip_bits(&s, 3);
		if (get_bits1(&s)) {
			if (get_bits_left(&s) < 79)
				goto failed;
			/* skip vbv_parameters */
			skip_bits(&s, 79);
		}
	}

	if (get_bits_left(&s) < 21)
		goto failed;

	/* layer shape */
	if (get_bits(&s, 2))
		/* only support rectangular */
		goto failed;

	if (!get_bits1(&s)) /* marker bit */
		goto failed;

	time_increment_resolution = get_bits(&s, 16);

	if (!get_bits1(&s)) /* marker bit */
		goto failed;

	if (get_bits1(&s)) {
		/* fixed time increment */
		int n;

		/*
		 * Length of the time increment is the minimal number of bits
		 * needed to represent time_increment_resolution.
		 */
		for (n = 0; time_increment_resolution >> n; n++)
			;
		if (get_bits_left(&s) < n)
			goto failed;
		skip_bits(&s, n);
	}

	/* assuming rectangular shape */

	if (get_bits_left(&s) < 29)
		goto failed;

	if (!get_bits1(&s)) /* marker bit */
		goto failed;
	width = get_bits(&s, 13);
	if (!width)
		goto failed;
	if (!get_bits1(&s)) /* marker bit */
		goto failed;
	height = get_bits(&s, 13);
	if (!height)
		goto failed;
	if (!get_bits1(&s)) /* marker bit */
		goto failed;

	set_framesize(vdec, width, height, 0, 0, 0, 0);
	return true;

failed:
	return false;
}

static unsigned read_bits(struct get_bit_context *s, int n)
{
	n = MIN(n, get_bits_left(s));
	if (n == 0)
		return 0;
	return get_bits(s, n);
}

/* read unsigned Exp-Golomb code */
static unsigned get_ue_golomb(struct get_bit_context *s)
{
	unsigned i;

	for (i = 0; i < 32; i++) {
		if (read_bits(s, 1) != 0)
			break;
		if (get_bits_left(s) <= 0)
			break;
	}

	return (1 << i) - 1 + read_bits(s, i);
}

/* read signed Exp-Golomb code */
static int get_se_golomb(struct get_bit_context *s)
{
	int i = 0;

	i = get_ue_golomb(s);
	/* (-1)^(i+1) Ceil (i / 2) */
	i = (i + 1) / 2 * (i & 1 ? 1 : -1);

	return i;
}

#define CHECK_EOS(s) \
	do { \
		if (get_bits_left(s) <= 0) \
			goto not_enough_data; \
	} while (0)

/* remove emulation prevention bytes (if needed) */
static bool rbsp_unescape(uint8_t *b, unsigned len, uint8_t **ret, unsigned *ret_len)
{
	unsigned i, si, di;
	uint8_t *dst;

	for (i = 0; i + 3 < len; i++) {
		if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] <= 3) {
			if (b[i + 2] != 3)
				/* startcode */
				len = i;
			break;
		}
	}

	if (i >= len - 3)
		return false;

	/* escaped */

	dst = malloc(len);

	memcpy(dst, b, i);
	si = di = i;
	while (si + 2 < len) {
		if (b[si] == 0 && b[si + 1] == 0 && b[si + 2] == 3) {
			dst[di++] = 0;
			dst[di++] = 0;
			si += 3;
		} else {
			dst[di++] = b[si++];
		}
	}
	while (si < len)
		dst[di++] = b[si++];

	*ret = dst;
	*ret_len = di;

	return true;
}

bool gst_av_h264_parse(struct gst_av_vdec *vdec, GstBuffer *buf)
{
	struct get_bit_context s;
	guint8 b, profile, chroma, frame;
	guint fc_top, fc_bottom, fc_left, fc_right;
	gint width, height;
	gint crop_width, crop_height;
	guint subwc[] = { 1, 2, 2, 1 }, subhc[] = { 1, 2, 1, 1 };
	guint32 d;
	bool avc;
	uint8_t *rbsp_buffer = NULL;
	unsigned rbsp_len;

	init_get_bits(&s, buf->data, buf->size * 8);

	/* auto-detect whether avc or byte-stream;
	 * as unconvential codec-data cases contain bytestream NALs */
	if (get_bits_left(&s) < 32)
		goto not_enough_data;
	d = get_bits(&s, 32);
	avc = (d != 1 && (d >> 8) != 1);

try_again:
	if (avc) {
		/* provided buffer is then codec_data */
		if (get_bits_left(&s) < 32)
			goto not_enough_data;

		/* configuration version == 1 */
		if (buf->data[0] != 1)
			return FALSE;

		/* reserved */
		d = get_bits(&s, 8);
		if ((d & 0xfc) != 0xfc)
			return FALSE;
		d = get_bits(&s, 8);

		/* number of SPS */
		if ((d & 0x1f) == 0)
			return false;

		skip_bits(&s, 16);
	} else {
		s.index = 0;

		/* frame size is recorded in Sequence Parameter Set (SPS) */
		/* locate SPS NAL unit in bytestream */
		while (get_bits_left(&s) >= 32) {
			uint32_t d = show_bits(&s, 32);
			if ((d >> 8 == 0x1) && ((d & 0x1F) == 0x07))
				break;
			skip_bits(&s, 8);
		}
		if (get_bits_left(&s) < 32)
			goto bail;
		skip_bits(&s, 24);
	}

	/* pointing at NAL SPS, now analyze it */
	if (get_bits_left(&s) < 40) {
		if (avc) {
			avc = false;
			goto try_again;
		} else {
			goto not_enough_data;
		}
	}

	if (rbsp_unescape(buf->data + (get_bits_count(&s) >> 3),
				get_bits_left(&s) >> 3,
				&rbsp_buffer, &rbsp_len))
	{
		/* reinitialize bitreader */
		init_get_bits(&s, rbsp_buffer, rbsp_len << 3);
	}

	b = get_bits(&s, 8);

	/* forbidden bit */
	if (b & 0x80)
		goto bail;

	/* need SPS NAL unit */
	if ((b & 0x1f) != 0x07)
		goto bail;

	profile = get_bits(&s, 8);

	if (get_bits_left(&s) < 16)
		goto not_enough_data;
	skip_bits(&s, 16);

	/* seq_parameter_set_id */
	get_ue_golomb(&s);
	CHECK_EOS(&s);
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
			profile == 44 || profile == 83 || profile == 86)
	{
		int scp_flag = 0;

		/* chroma_format_idc */
		chroma = get_ue_golomb(&s);
		CHECK_EOS(&s);
		if (chroma == 3) {
			/* separate_colour_plane_flag */
			if (get_bits_left(&s) < 1)
				goto not_enough_data;
			scp_flag = get_bits1(&s);
		}
		/* bit_depth_luma_minus8 */
		get_ue_golomb(&s);
		CHECK_EOS(&s);
		/* bit_depth_chroma_minus8 */
		get_ue_golomb(&s);
		CHECK_EOS(&s);

		if (get_bits_left(&s) < 2)
			goto not_enough_data;
		/* qpprime_y_zero_transform_bypass_flag */
		skip_bits(&s, 1);
		/* seq_scaling_matrix_present_flag */
		if (get_bits1(&s)) {
			int i, j, m;

			m = (chroma != 3) ? 8 : 12;
			for (i = 0; i < m; i++) {
				if (get_bits_left(&s) < 1)
					goto not_enough_data;
				/* seq_scaling_list_present_flag[i] */
				if (get_bits1(&s)) {
					int last_scale = 8, next_scale = 8, delta_scale;

					j = (i < 6) ? 16 : 64;
					for (; j > 0; j--) {
						if (next_scale) {
							delta_scale = get_se_golomb(&s);
							CHECK_EOS(&s);
							next_scale = (last_scale + delta_scale + 256) % 256;
						}
						if (next_scale)
							last_scale = next_scale;
					}
				}
			}
		}
		if (scp_flag)
			chroma = 0;
	} else {
		/* inferred value */
		chroma = 1;
	}
	/* log2_max_frame_num_minus4 */
	get_ue_golomb(&s);
	CHECK_EOS(&s);
	/* pic_order_cnt_type */
	b = get_ue_golomb(&s);
	CHECK_EOS(&s);
	if (b == 0) {
		/* log2_max_pic_order_cnt_lsb_minus4 */
		get_ue_golomb(&s);
		CHECK_EOS(&s);
	} else if (b == 1) {
		if (get_bits_left(&s) < 1)
			goto not_enough_data;
		/* delta_pic_order_always_zero_flag */
		skip_bits(&s, 1);
		/* offset_for_non_ref_pic */
		get_ue_golomb(&s);
		CHECK_EOS(&s);
		/* offset_for_top_to_bottom_field */
		get_ue_golomb(&s);
		CHECK_EOS(&s);
		/* num_ref_frames_in_pic_order_cnt_cycle */
		d = get_ue_golomb(&s);
		CHECK_EOS(&s);
		for (; d > 0;  d--) {
			/* offset_for_ref_frame[i] */
			get_ue_golomb(&s);
			CHECK_EOS(&s);
		}
	}
	/* num_ref_frames */
	get_ue_golomb(&s);
	CHECK_EOS(&s);
	/* gaps_in_frame_num_value_allowed_flag */
	read_bits(&s, 1);
	CHECK_EOS(&s);
	/* pic_width_in_mbs_minus1 */
	width = get_ue_golomb(&s) + 1;
	width *= 16;
	/* pic_height_in_map_units_minus1 */
	height = get_ue_golomb(&s) + 1;
	CHECK_EOS(&s);
	/* frame_mbs_only_flag */
	frame = read_bits(&s, 1);
	CHECK_EOS(&s);
	height *= 16 * (2 - frame);
	if (!frame) {
		/* mb_adaptive_frame_field_flag */
		read_bits(&s, 1);
		CHECK_EOS(&s);
	}
	/* direct_8x8_inference_flag */
	read_bits(&s, 1);
	CHECK_EOS(&s);
	/* frame_cropping_flag */
	b = read_bits(&s, 1);
	CHECK_EOS(&s);
	if (b) {
		fc_left = get_ue_golomb(&s);
		CHECK_EOS(&s);
		fc_right = get_ue_golomb(&s);
		CHECK_EOS(&s);
		fc_top = get_ue_golomb(&s);
		CHECK_EOS(&s);
		fc_bottom = get_ue_golomb(&s);
		CHECK_EOS(&s);
	} else
		fc_left = fc_right = fc_top = fc_bottom = 0;

	if (chroma > 3) {
		goto bail;
	}
	crop_width = width - (fc_left + fc_right) * subwc[chroma];
	crop_height = height - (fc_top + fc_bottom) * subhc[chroma] * (2 - frame);
	if (width < 0 || height < 0 || crop_width < 0 || crop_height < 0) {
		goto bail;
	}

	set_framesize(vdec, width, height, 0, 0, crop_width, crop_height);
	free(rbsp_buffer);
	return true;

not_enough_data:
bail:
	free(rbsp_buffer);
	return false;
}
