/* GStreamer
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _GST_NV_HDMI_OVERLAY_SINK_H_
#define _GST_NV_HDMI_OVERLAY_SINK_H_

#include "gstomxvideosink.h"

G_BEGIN_DECLS
#define GST_TYPE_NV_HDMI_OVERLAY_SINK   (gst_nv_hdmi_overlay_sink_get_type())
#define GST_NV_HDMI_OVERLAY_SINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NV_HDMI_OVERLAY_SINK,GstNvHDMIOverlaySink))
#define GST_NV_HDMI_OVERLAY_SINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NV_HDMI_OVERLAY_SINK,GstNvHDMIOverlaySinkClass))
#define GST_NV_HDMI_OVERLAY_SINK_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_NV_HDMI_OVERLAY_SINK,GstNvHDMIOverlaySinkClass))
#define GST_IS_NV_HDMI_OVERLAY_SINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NV_HDMI_OVERLAY_SINK))
#define GST_IS_NV_HDMI_OVERLAY_SINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NV_HDMI_OVERLAY_SINK))
typedef struct _GstNvHDMIOverlaySink GstNvHDMIOverlaySink;
typedef struct _GstNvHDMIOverlaySinkClass GstNvHDMIOverlaySinkClass;

struct _GstNvHDMIOverlaySink
{
  GstOmxVideoSink parent;
};

struct _GstNvHDMIOverlaySinkClass
{
  GstOmxVideoSinkClass parent_class;
};

GType gst_nv_hdmi_overlay_sink_get_type (void);

G_END_DECLS
#endif
