/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "plugin.h"

#include "gstav_adec.h"
#include "gstav_vdec.h"
#include "gstav_h263enc.h"
#include "gstav_h264enc.h"

#include <stdbool.h>

GstDebugCategory *gstav_debug;

static GStaticMutex gst_av_codec_mutex = G_STATIC_MUTEX_INIT;

int gst_av_codec_open(AVCodecContext *avctx, AVCodec *codec)
{
	int ret;

	g_static_mutex_lock(&gst_av_codec_mutex);
	ret = avcodec_open(avctx, codec);
	g_static_mutex_unlock(&gst_av_codec_mutex);

	return ret;
}

int gst_av_codec_close(AVCodecContext *avctx)
{
	int ret;

	g_static_mutex_lock(&gst_av_codec_mutex);
	ret = avcodec_close(avctx);
	g_static_mutex_unlock(&gst_av_codec_mutex);

	return ret;
}

static gboolean
plugin_init(GstPlugin *plugin)
{
#ifndef GST_DISABLE_GST_DEBUG
	gstav_debug = _gst_debug_category_new("av", 0, "libav stuff");
#endif

	if (!gst_element_register(plugin, "avadec", GST_RANK_PRIMARY + 1, GST_AV_ADEC_TYPE))
		return false;

	if (!gst_element_register(plugin, "avvdec", GST_RANK_PRIMARY + 1, GST_AV_VDEC_TYPE))
		return false;

	if (!gst_element_register(plugin, "avh263enc", GST_RANK_PRIMARY + 1, GST_AV_H263ENC_TYPE))
		return false;

	if (!gst_element_register(plugin, "avh264enc", GST_RANK_PRIMARY + 1, GST_AV_H264ENC_TYPE))
		return false;

	return true;
}

GstPluginDesc gst_plugin_desc = {
	.major_version = 0,
	.minor_version = 10,
	.name = "av",
	.description = (gchar *) "libav plugin",
	.plugin_init = plugin_init,
	.version = VERSION,
	.license = "LGPL",
	.source = "none",
	.package = "none",
	.origin = "none",
};
