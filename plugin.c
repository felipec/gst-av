/*
 * Copyright (C) 2009-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1.
 */

#include "plugin.h"

#include "gstavdec.h"

GstDebugCategory *gstav_debug;

static gboolean
plugin_init(GstPlugin *plugin)
{
#ifndef GST_DISABLE_GST_DEBUG
	gstav_debug = _gst_debug_category_new("av", 0, "libav stuff");
#endif

	if (!gst_element_register(plugin, "avdec", GST_RANK_PRIMARY + 1, GST_AVDEC_TYPE))
		return FALSE;

	return TRUE;
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
