/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (c) 2013-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <string.h>

#include "gstomxvideoenc.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_video_enc_debug_category);
#define GST_CAT_DEFAULT gst_omx_video_enc_debug_category

#define GST_TYPE_OMX_VID_ENC_RCMODE (gst_omx_videnc_rc_mode_get_type ())
static GType
gst_omx_videnc_rc_mode_get_type (void)
{
  static volatile gsize rcmode_type_type = 0;
  static const GEnumValue rcmode_type[] = {
    {NVX_VIDEO_RateControlMode_CBR, "GST_OMX_VIDENC_RCMODE_TYPE_CONSTANT",
        "cbr"},
    {NVX_VIDEO_RateControlMode_VBR, "GST_OMX_VIDENC_RCMODE_TYPE_VARIABLE",
        "vbr"},
    {NVX_VIDEO_RateControlMode_VBR2, "GST_OMX_VIDENC_RCMODE_TYPE_VARIABLE2",
        "vbr2"},
    {0, NULL, NULL}
  };

  if (g_once_init_enter (&rcmode_type_type)) {
    GType tmp =
        g_enum_register_static ("GstOmxVideoEncRCModeType", rcmode_type);
    g_once_init_leave (&rcmode_type_type, tmp);
  }

  return (GType) rcmode_type_type;
}

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* prototypes */
static void gst_omx_video_enc_finalize (GObject * object);
static void gst_omx_video_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_video_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);


static GstStateChangeReturn
gst_omx_video_enc_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_omx_video_enc_open (GstVideoEncoder * encoder);
static gboolean gst_omx_video_enc_close (GstVideoEncoder * encoder);
static gboolean gst_omx_video_enc_start (GstVideoEncoder * encoder);
static gboolean gst_omx_video_enc_stop (GstVideoEncoder * encoder);
static gboolean gst_omx_video_enc_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state);
static gboolean gst_omx_video_enc_reset (GstVideoEncoder * encoder,
    gboolean hard);
static GstFlowReturn gst_omx_video_enc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame);
static gboolean gst_omx_video_enc_finish (GstVideoEncoder * encoder);
static gboolean gst_omx_video_enc_propose_allocation (GstVideoEncoder * encoder,
    GstQuery * query);
static GstCaps *gst_omx_video_enc_getcaps (GstVideoEncoder * encoder,
    GstCaps * filter);

static GstFlowReturn gst_omx_video_enc_drain (GstOMXVideoEnc * self,
    gboolean at_eos);

static GstFlowReturn gst_omx_video_enc_handle_output_frame (GstOMXVideoEnc *
    self, GstOMXPort * port, GstOMXBuffer * buf, GstVideoCodecFrame * frame);

static GstCaps *gst_omx_video_enc_negotiate_caps (GstVideoEncoder * encoder,
    GstCaps * caps, GstCaps * filter);

static void
gst_omx_video_enc_check_nvfeatures (GstOMXVideoEnc * self,
    GstVideoCodecState * state);

enum
{
  PROP_0,
  PROP_RC_MODE,
  PROP_BITRATE,
  PROP_QUANT_I_FRAMES,
  PROP_QUANT_P_FRAMES,
  PROP_QUANT_B_FRAMES,
  PROP_INTRA_FRAME_INTERVAL
};

/* FIXME: Better defaults */
#define DEFAULT_RC_MODE                          NVX_VIDEO_RateControlMode_VBR2
#define GST_OMX_VIDEO_ENC_BITRATE_DEFAULT        (4000000)
#define GST_OMX_VIDEO_ENC_QUANT_I_FRAMES_DEFAULT (0xffffffff)
#define GST_OMX_VIDEO_ENC_QUANT_P_FRAMES_DEFAULT (0xffffffff)
#define GST_OMX_VIDEO_ENC_QUANT_B_FRAMES_DEFAULT (0xffffffff)
#define DEFAULT_INTRA_FRAME_INTERVAL             60

#ifdef USE_OMX_TARGET_TEGRA
#define ENCODER_CONF_LOCATION   "/etc/enctune.conf"
#endif

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_video_enc_debug_category, "omxvideoenc", 0, \
      "debug category for gst-omx video encoder base class");

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstOMXVideoEnc, gst_omx_video_enc,
    GST_TYPE_VIDEO_ENCODER, DEBUG_INIT);

#ifdef USE_OMX_TARGET_TEGRA
#define FORMATS "I420, NV12"
#endif

static void
gst_omx_video_enc_class_init (GstOMXVideoEncClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoEncoderClass *video_encoder_class = GST_VIDEO_ENCODER_CLASS (klass);


  gobject_class->finalize = gst_omx_video_enc_finalize;
  gobject_class->set_property = gst_omx_video_enc_set_property;
  gobject_class->get_property = gst_omx_video_enc_get_property;

  g_object_class_install_property (gobject_class, PROP_RC_MODE,
      g_param_spec_enum ("rc-mode", "rc-mode",
          "Encoding rate control mode",
          GST_TYPE_OMX_VID_ENC_RCMODE, DEFAULT_RC_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_BITRATE,
      g_param_spec_uint ("bitrate", "Target Bitrate",
          "Target bitrate",
          0, G_MAXUINT, GST_OMX_VIDEO_ENC_BITRATE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING));

  g_object_class_install_property (gobject_class, PROP_QUANT_I_FRAMES,
      g_param_spec_uint ("quant-i-frames", "I-Frame Quantization",
          "Quantization parameter for I-frames (0xffffffff=component default)",
          0, G_MAXUINT, GST_OMX_VIDEO_ENC_QUANT_I_FRAMES_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_QUANT_P_FRAMES,
      g_param_spec_uint ("quant-p-frames", "P-Frame Quantization",
          "Quantization parameter for P-frames (0xffffffff=component default)",
          0, G_MAXUINT, GST_OMX_VIDEO_ENC_QUANT_P_FRAMES_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_QUANT_B_FRAMES,
      g_param_spec_uint ("quant-b-frames", "B-Frame Quantization",
          "Quantization parameter for B-frames (0xffffffff=component default)",
          0, G_MAXUINT, GST_OMX_VIDEO_ENC_QUANT_B_FRAMES_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_INTRA_FRAME_INTERVAL,
      g_param_spec_uint ("iframeinterval", "Intra Frame interval",
          "Encoding Intra Frame occurance frequency",
          0, G_MAXUINT, DEFAULT_INTRA_FRAME_INTERVAL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_omx_video_enc_change_state);

  video_encoder_class->open = GST_DEBUG_FUNCPTR (gst_omx_video_enc_open);
  video_encoder_class->close = GST_DEBUG_FUNCPTR (gst_omx_video_enc_close);
  video_encoder_class->start = GST_DEBUG_FUNCPTR (gst_omx_video_enc_start);
  video_encoder_class->stop = GST_DEBUG_FUNCPTR (gst_omx_video_enc_stop);
  video_encoder_class->reset = GST_DEBUG_FUNCPTR (gst_omx_video_enc_reset);
  video_encoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_omx_video_enc_set_format);
  video_encoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_omx_video_enc_handle_frame);
  video_encoder_class->finish = GST_DEBUG_FUNCPTR (gst_omx_video_enc_finish);
  video_encoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_omx_video_enc_propose_allocation);
  video_encoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_omx_video_enc_getcaps);

  klass->cdata.type = GST_OMX_COMPONENT_TYPE_FILTER;
  klass->cdata.default_sink_template_caps = "video/x-raw(memory:NVMM), "
#ifdef USE_OMX_TARGET_TEGRA
      "format = (string) { " FORMATS " }, "
#endif
      "width = " GST_VIDEO_SIZE_RANGE ", "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE
      ";" "video/x-raw, "
#ifdef USE_OMX_TARGET_TEGRA
      "format = (string) { " FORMATS " }, "
#endif
      "width = " GST_VIDEO_SIZE_RANGE ", "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;

  klass->handle_output_frame =
      GST_DEBUG_FUNCPTR (gst_omx_video_enc_handle_output_frame);
}

static void
gst_omx_video_enc_init (GstOMXVideoEnc * self)
{
  self->bitrate = GST_OMX_VIDEO_ENC_BITRATE_DEFAULT;
  self->rc_mode = DEFAULT_RC_MODE;
  self->quant_i_frames = GST_OMX_VIDEO_ENC_QUANT_I_FRAMES_DEFAULT;
  self->quant_p_frames = GST_OMX_VIDEO_ENC_QUANT_P_FRAMES_DEFAULT;
  self->quant_b_frames = GST_OMX_VIDEO_ENC_QUANT_B_FRAMES_DEFAULT;
  self->hw_path = FALSE;

  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);
}

static gboolean
gst_omx_video_enc_open (GstVideoEncoder * encoder)
{
  GstOMXVideoEnc *self = GST_OMX_VIDEO_ENC (encoder);
  GstOMXVideoEncClass *klass = GST_OMX_VIDEO_ENC_GET_CLASS (self);
  gint in_port_index, out_port_index;

  self->enc =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);
  self->started = FALSE;

  if (!self->enc)
    return FALSE;

  if (gst_omx_component_get_state (self->enc,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  in_port_index = klass->cdata.in_port_index;
  out_port_index = klass->cdata.out_port_index;

  if (in_port_index == -1 || out_port_index == -1) {
    OMX_PORT_PARAM_TYPE param;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&param);

    err =
        gst_omx_component_get_parameter (self->enc, OMX_IndexParamVideoInit,
        &param);
    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't get port information: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      /* Fallback */
      in_port_index = 0;
      out_port_index = 1;
    } else {
      GST_DEBUG_OBJECT (self, "Detected %u ports, starting at %u",
          (guint) param.nPorts, (guint) param.nStartPortNumber);
      in_port_index = param.nStartPortNumber + 0;
      out_port_index = param.nStartPortNumber + 1;
    }
  }

  self->enc_in_port = gst_omx_component_add_port (self->enc, in_port_index);
  self->enc_out_port = gst_omx_component_add_port (self->enc, out_port_index);

  if (!self->enc_in_port || !self->enc_out_port)
    return FALSE;

  /* Set properties */
  {
    OMX_ERRORTYPE err;

#ifdef USE_OMX_TARGET_TEGRA

    OMX_INDEXTYPE eIndex;
    NVX_PARAM_TEMPFILEPATH enc_conf_loc;
    gchar *location = g_strdup (ENCODER_CONF_LOCATION);

    GST_DEBUG_OBJECT (self, "Setting encoder to use default configurations");

    err = gst_omx_component_get_index (self->enc,
        (char *) NVX_INDEX_PARAM_TEMPFILEPATH, &eIndex);

    if (err == OMX_ErrorNone) {
      if (location != NULL) {
        GST_OMX_INIT_STRUCT (&enc_conf_loc);
        enc_conf_loc.pTempPath = location;
        err =
            gst_omx_component_set_parameter (self->enc, eIndex, &enc_conf_loc);
      }
    } else {
      GST_WARNING_OBJECT (self, "Coudn't get extension index for %s",
          (char *) NVX_INDEX_PARAM_TEMPFILEPATH);
    }

    if (err != OMX_ErrorNone)
      GST_WARNING_OBJECT (self, "Couldn't set configurations");

    g_free (location);

#endif

    if (self->quant_i_frames != 0xffffffff ||
        self->quant_p_frames != 0xffffffff ||
        self->quant_b_frames != 0xffffffff) {
      OMX_VIDEO_PARAM_QUANTIZATIONTYPE quant_param;

      GST_OMX_INIT_STRUCT (&quant_param);
      quant_param.nPortIndex = self->enc_out_port->index;

      err = gst_omx_component_get_parameter (self->enc,
          OMX_IndexParamVideoQuantization, &quant_param);

      if (err == OMX_ErrorNone) {

        if (self->quant_i_frames != 0xffffffff)
          quant_param.nQpI = self->quant_i_frames;
        if (self->quant_p_frames != 0xffffffff)
          quant_param.nQpP = self->quant_p_frames;
        if (self->quant_b_frames != 0xffffffff)
          quant_param.nQpB = self->quant_b_frames;

        err =
            gst_omx_component_set_parameter (self->enc,
            OMX_IndexParamVideoQuantization, &quant_param);
        if (err == OMX_ErrorUnsupportedIndex) {
          GST_WARNING_OBJECT (self,
              "Setting quantization parameters not supported by the component");
        } else if (err == OMX_ErrorUnsupportedSetting) {
          GST_WARNING_OBJECT (self,
              "Setting quantization parameters %u %u %u not supported by the component",
              self->quant_i_frames, self->quant_p_frames, self->quant_b_frames);
        } else if (err != OMX_ErrorNone) {
          GST_ERROR_OBJECT (self,
              "Failed to set quantization parameters: %s (0x%08x)",
              gst_omx_error_to_string (err), err);
          return FALSE;
        }
      } else {
        GST_ERROR_OBJECT (self,
            "Failed to get quantization parameters: %s (0x%08x)",
            gst_omx_error_to_string (err), err);

      }
    }
  }

  return TRUE;
}

static gboolean
gst_omx_video_enc_shutdown (GstOMXVideoEnc * self)
{
  OMX_STATETYPE state;

  GST_DEBUG_OBJECT (self, "Shutting down encoder");

  state = gst_omx_component_get_state (self->enc, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->enc, OMX_StateIdle);
      gst_omx_component_get_state (self->enc, 5 * GST_SECOND);
    }
    gst_omx_component_set_state (self->enc, OMX_StateLoaded);
    gst_omx_port_deallocate_buffers (self->enc_in_port);
    gst_omx_port_deallocate_buffers (self->enc_out_port);
    if (state > OMX_StateLoaded)
      gst_omx_component_get_state (self->enc, 5 * GST_SECOND);
  }

  return TRUE;
}

static gboolean
gst_omx_video_enc_close (GstVideoEncoder * encoder)
{
  GstOMXVideoEnc *self = GST_OMX_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Closing encoder");

  if (!gst_omx_video_enc_shutdown (self))
    return FALSE;

  self->enc_in_port = NULL;
  self->enc_out_port = NULL;
  if (self->enc)
    gst_omx_component_free (self->enc);
  self->enc = NULL;

  return TRUE;
}

static void
gst_omx_video_enc_finalize (GObject * object)
{
  GstOMXVideoEnc *self = GST_OMX_VIDEO_ENC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  G_OBJECT_CLASS (gst_omx_video_enc_parent_class)->finalize (object);
}

static void
gst_omx_video_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOMXVideoEnc *self = GST_OMX_VIDEO_ENC (object);

  switch (prop_id) {
    case PROP_RC_MODE:
      self->rc_mode = g_value_get_enum (value);
      break;
    case PROP_BITRATE:
      self->bitrate = g_value_get_uint (value);
      if (self->enc) {
        OMX_VIDEO_CONFIG_BITRATETYPE config;
        OMX_ERRORTYPE err;

        GST_OMX_INIT_STRUCT (&config);
        config.nPortIndex = self->enc_out_port->index;
        config.nEncodeBitrate = self->bitrate;
        err =
            gst_omx_component_set_config (self->enc,
            OMX_IndexConfigVideoBitrate, &config);
        if (err != OMX_ErrorNone)
          GST_ERROR_OBJECT (self,
              "Failed to set bitrate parameter: %s (0x%08x)",
              gst_omx_error_to_string (err), err);
      }
      break;
    case PROP_QUANT_I_FRAMES:
      self->quant_i_frames = g_value_get_uint (value);
      break;
    case PROP_QUANT_P_FRAMES:
      self->quant_p_frames = g_value_get_uint (value);
      break;
    case PROP_QUANT_B_FRAMES:
      self->quant_b_frames = g_value_get_uint (value);
      break;
    case PROP_INTRA_FRAME_INTERVAL:
      self->iframeinterval = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_video_enc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstOMXVideoEnc *self = GST_OMX_VIDEO_ENC (object);

  switch (prop_id) {
    case PROP_RC_MODE:
      g_value_set_enum (value, self->rc_mode);
      break;
    case PROP_BITRATE:
      g_value_set_uint (value, self->bitrate);
      break;
    case PROP_QUANT_I_FRAMES:
      g_value_set_uint (value, self->quant_i_frames);
      break;
    case PROP_QUANT_P_FRAMES:
      g_value_set_uint (value, self->quant_p_frames);
      break;
    case PROP_QUANT_B_FRAMES:
      g_value_set_uint (value, self->quant_b_frames);
      break;
    case PROP_INTRA_FRAME_INTERVAL:
      g_value_set_uint (value, self->iframeinterval);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_omx_video_enc_change_state (GstElement * element, GstStateChange transition)
{
  GstOMXVideoEnc *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  g_return_val_if_fail (GST_IS_OMX_VIDEO_ENC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_OMX_VIDEO_ENC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;

      self->draining = FALSE;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->enc_in_port)
        gst_omx_port_set_flushing (self->enc_in_port, 5 * GST_SECOND, TRUE);
      if (self->enc_out_port)
        gst_omx_port_set_flushing (self->enc_out_port, 5 * GST_SECOND, TRUE);

      g_mutex_lock (&self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      g_mutex_unlock (&self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret =
      GST_ELEMENT_CLASS (gst_omx_video_enc_parent_class)->change_state (element,
      transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_FLUSHING;
      self->started = FALSE;

      if (!gst_omx_video_enc_shutdown (self))
        ret = GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

#define MAX_FRAME_DIST_TICKS  (5 * OMX_TICKS_PER_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstVideoCodecFrame *
_find_nearest_frame (GstOMXVideoEnc * self, GstOMXBuffer * buf)
{
  GList *l, *best_l = NULL;
  GList *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GList *frames;

  frames = gst_video_encoder_get_frames (GST_VIDEO_ENCODER (self));

  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *tmp = l->data;
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > buf->omx_buf->nTimeStamp)
      diff = timestamp - buf->omx_buf->nTimeStamp;
    else
      diff = buf->omx_buf->nTimeStamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_l = l;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((buf->omx_buf->nTimeStamp == 0 && timestamp == 0) || diff == 0)
        break;
    }
  }

  if (best_id) {
    for (l = frames; l && l != best_l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_ticks, diff_frames;

      /* This happens for frames that were just added but
       * which were not passed to the component yet. Ignore
       * them here!
       */
      if (!id)
        continue;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_ticks = 0;
      else
        diff_ticks = best_timestamp - id->timestamp;
      diff_frames = best->system_frame_number - tmp->system_frame_number;

      if (diff_ticks > MAX_FRAME_DIST_TICKS
          || diff_frames > MAX_FRAME_DIST_FRAMES) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }

  if (finish_frames) {
    g_warning ("Too old frames, bug in encoder -- please file a bug");
    for (l = finish_frames; l; l = l->next) {
      gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (self), l->data);
    }
  }

  if (best)
    gst_video_codec_frame_ref (best);

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}

static GstFlowReturn
gst_omx_video_enc_handle_output_frame (GstOMXVideoEnc * self, GstOMXPort * port,
    GstOMXBuffer * buf, GstVideoCodecFrame * frame)
{
  GstOMXVideoEncClass *klass = GST_OMX_VIDEO_ENC_GET_CLASS (self);
  GstFlowReturn flow_ret = GST_FLOW_OK;

  if ((buf->omx_buf->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
      && buf->omx_buf->nFilledLen > 0) {
    GstVideoCodecState *state;
    GstBuffer *codec_data;
    GstMapInfo map = GST_MAP_INFO_INIT;
    GstCaps *caps;

    GST_DEBUG_OBJECT (self, "Handling codec data");

    caps = klass->get_caps (self, self->enc_out_port, self->input_state);
    if (self->codec_data) {
      codec_data = self->codec_data;
    } else {
      codec_data = gst_buffer_new_and_alloc (buf->omx_buf->nFilledLen);

      gst_buffer_map (codec_data, &map, GST_MAP_WRITE);
      memcpy (map.data,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
          buf->omx_buf->nFilledLen);
      gst_buffer_unmap (codec_data, &map);
    }
    state =
        gst_video_encoder_set_output_state (GST_VIDEO_ENCODER (self), caps,
        self->input_state);
    state->codec_data = codec_data;
    if (!gst_video_encoder_negotiate (GST_VIDEO_ENCODER (self))) {
      gst_video_codec_frame_unref (frame);
      return GST_FLOW_NOT_NEGOTIATED;
    }
    flow_ret = GST_FLOW_OK;
  } else if (buf->omx_buf->nFilledLen > 0) {
    GstBuffer *outbuf;
    GstMapInfo map = GST_MAP_INFO_INIT;

    GST_DEBUG_OBJECT (self, "Handling output data");

    if (buf->omx_buf->nFilledLen > 0) {
      outbuf = gst_buffer_new_and_alloc (buf->omx_buf->nFilledLen);

      gst_buffer_map (outbuf, &map, GST_MAP_WRITE);
      memcpy (map.data,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
          buf->omx_buf->nFilledLen);
      gst_buffer_unmap (outbuf, &map);
    } else {
      outbuf = gst_buffer_new ();
    }

    GST_BUFFER_TIMESTAMP (outbuf) =
        gst_util_uint64_scale (buf->omx_buf->nTimeStamp, GST_SECOND,
        OMX_TICKS_PER_SECOND);
    if (buf->omx_buf->nTickCount != 0)
      GST_BUFFER_DURATION (outbuf) =
          gst_util_uint64_scale (buf->omx_buf->nTickCount, GST_SECOND,
          OMX_TICKS_PER_SECOND);

    if ((klass->cdata.hacks & GST_OMX_HACK_SYNCFRAME_FLAG_NOT_USED)
        || (buf->omx_buf->nFlags & OMX_BUFFERFLAG_SYNCFRAME)) {
      if (frame)
        GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
      else
        GST_BUFFER_FLAG_UNSET (outbuf, GST_BUFFER_FLAG_DELTA_UNIT);
    } else {
      if (frame)
        GST_VIDEO_CODEC_FRAME_UNSET_SYNC_POINT (frame);
      else
        GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    if (frame) {
      frame->output_buffer = outbuf;
      flow_ret =
          gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (self), frame);
    } else {
      GST_ERROR_OBJECT (self, "No corresponding frame found");
      flow_ret = gst_pad_push (GST_VIDEO_ENCODER_SRC_PAD (self), outbuf);
    }
  } else if (frame != NULL) {
    flow_ret = gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (self), frame);
  }

  return flow_ret;
}

static void
gst_omx_video_enc_loop (GstOMXVideoEnc * self)
{
  GstOMXVideoEncClass *klass;
  GstOMXPort *port = self->enc_out_port;
  GstOMXBuffer *buf = NULL;
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstOMXAcquireBufferReturn acq_return;
  OMX_ERRORTYPE err;

  klass = GST_OMX_VIDEO_ENC_GET_CLASS (self);

  acq_return = gst_omx_port_acquire_buffer (port, &buf);
  if (acq_return == GST_OMX_ACQUIRE_BUFFER_ERROR) {
    goto component_error;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
    goto flushing;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_EOS) {
    goto eos;
  }

  if (!gst_pad_has_current_caps (GST_VIDEO_ENCODER_SRC_PAD (self))
      || acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
    GstCaps *caps;
    GstVideoCodecState *state;

    GST_DEBUG_OBJECT (self, "Port settings have changed, updating caps");

    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE
        && gst_omx_port_is_enabled (port)) {
      /* Reallocate all buffers */
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_deallocate_buffers (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    }

    GST_VIDEO_ENCODER_STREAM_LOCK (self);

    caps = klass->get_caps (self, self->enc_out_port, self->input_state);
    if (!caps) {
      if (buf)
        gst_omx_port_release_buffer (self->enc_out_port, buf);
      GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
      goto caps_failed;
    }

    GST_DEBUG_OBJECT (self, "Setting output state: %" GST_PTR_FORMAT, caps);

    state =
        gst_video_encoder_set_output_state (GST_VIDEO_ENCODER (self), caps,
        self->input_state);
    gst_video_codec_state_unref (state);

    if (!gst_video_encoder_negotiate (GST_VIDEO_ENCODER (self))) {
      if (buf)
        gst_omx_port_release_buffer (self->enc_out_port, buf);
      GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
      goto caps_failed;
    }

    GST_VIDEO_ENCODER_STREAM_UNLOCK (self);

    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_allocate_buffers (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_populate (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    }

    /* Now get a buffer */
    if (acq_return != GST_OMX_ACQUIRE_BUFFER_OK) {
      return;
    }
  }

  g_assert (acq_return == GST_OMX_ACQUIRE_BUFFER_OK);

  /* This prevents a deadlock between the srcpad stream
   * lock and the videocodec stream lock, if ::reset()
   * is called at the wrong time
   */
  if (gst_omx_port_is_flushing (self->enc_out_port)) {
    GST_DEBUG_OBJECT (self, "Flushing");
    gst_omx_port_release_buffer (self->enc_out_port, buf);
    goto flushing;
  }

  GST_DEBUG_OBJECT (self, "Handling buffer: 0x%08x %" G_GUINT64_FORMAT,
      (guint) buf->omx_buf->nFlags, (guint64) buf->omx_buf->nTimeStamp);

  GST_VIDEO_ENCODER_STREAM_LOCK (self);
  frame = _find_nearest_frame (self, buf);

  g_assert (klass->handle_output_frame);
  flow_ret = klass->handle_output_frame (self, self->enc_out_port, buf, frame);

  GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));

  err = gst_omx_port_release_buffer (port, buf);
  if (err != OMX_ErrorNone)
    goto release_error;

  self->downstream_flow_ret = flow_ret;

  GST_DEBUG_OBJECT (self, "Read frame from component");

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_VIDEO_ENCODER_STREAM_UNLOCK (self);

  return;

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->enc),
            gst_omx_component_get_last_error (self->enc)));
    gst_pad_push_event (GST_VIDEO_ENCODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    self->started = FALSE;
    return;
  }

eos:
  {
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      flow_ret = GST_FLOW_OK;
      gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    } else {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);

    GST_VIDEO_ENCODER_STREAM_LOCK (self);
    self->downstream_flow_ret = flow_ret;

    /* Here we fallback and pause the task for the EOS case */
    if (flow_ret != GST_FLOW_OK)
      goto flow_error;

    GST_VIDEO_ENCODER_STREAM_UNLOCK (self);

    return;
  }
flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");

      gst_pad_push_event (GST_VIDEO_ENCODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_NOT_LINKED || flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Internal data stream error."),
          ("stream stopped, reason %s", gst_flow_get_name (flow_ret)));

      gst_pad_push_event (GST_VIDEO_ENCODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    }
    self->started = FALSE;
    GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
    return;
  }
reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure output port"));
    gst_pad_push_event (GST_VIDEO_ENCODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;
    return;
  }
caps_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL), ("Failed to set caps"));
    gst_pad_push_event (GST_VIDEO_ENCODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;
    return;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase output buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    gst_pad_push_event (GST_VIDEO_ENCODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_ENCODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
    return;
  }
}

static gboolean
gst_omx_video_enc_start (GstVideoEncoder * encoder)
{
  GstOMXVideoEnc *self;

  self = GST_OMX_VIDEO_ENC (encoder);

  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_omx_video_enc_stop (GstVideoEncoder * encoder)
{
  GstOMXVideoEnc *self;

  self = GST_OMX_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Stopping encoder");

  gst_omx_port_set_flushing (self->enc_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->enc_out_port, 5 * GST_SECOND, TRUE);

  gst_pad_stop_task (GST_VIDEO_ENCODER_SRC_PAD (encoder));

  if (gst_omx_component_get_state (self->enc, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->enc, OMX_StateIdle);

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->started = FALSE;
  self->eos = FALSE;

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);

  gst_omx_component_get_state (self->enc, 5 * GST_SECOND);

  return TRUE;
}

typedef struct
{
  GstVideoFormat format;
  OMX_COLOR_FORMATTYPE type;
} VideoNegotiationMap;

static void
video_negotiation_map_free (VideoNegotiationMap * m)
{
  g_slice_free (VideoNegotiationMap, m);
}

static GList *
gst_omx_video_enc_get_supported_colorformats (GstOMXVideoEnc * self)
{
  GstOMXPort *port = self->enc_in_port;
  GstVideoCodecState *state = self->input_state;
  OMX_VIDEO_PARAM_PORTFORMATTYPE param;
  OMX_ERRORTYPE err;
  GList *negotiation_map = NULL;
  gint old_index;

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = port->index;
  param.nIndex = 0;
  if (!state || state->info.fps_n == 0)
    param.xFramerate = 0;
  else
    param.xFramerate = (state->info.fps_n << 16) / (state->info.fps_d);

  old_index = -1;
  do {
    VideoNegotiationMap *m;

    err =
        gst_omx_component_get_parameter (self->enc,
        OMX_IndexParamVideoPortFormat, &param);

    /* FIXME: Workaround for Bellagio that simply always
     * returns the same value regardless of nIndex and
     * never returns OMX_ErrorNoMore
     */
    if (old_index == param.nIndex)
      break;

    if (err == OMX_ErrorNone || err == OMX_ErrorNoMore) {
      switch (param.eColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420PackedPlanar:
          m = g_slice_new (VideoNegotiationMap);
          m->format = GST_VIDEO_FORMAT_I420;
          m->type = param.eColorFormat;
          negotiation_map = g_list_append (negotiation_map, m);
          GST_DEBUG_OBJECT (self, "Component supports I420 (%d) at index %u",
              param.eColorFormat, (guint) param.nIndex);
          break;
        case OMX_COLOR_FormatYUV420SemiPlanar:
          m = g_slice_new (VideoNegotiationMap);
          m->format = GST_VIDEO_FORMAT_NV12;
          m->type = param.eColorFormat;
          negotiation_map = g_list_append (negotiation_map, m);
          GST_DEBUG_OBJECT (self, "Component supports NV12 (%d) at index %u",
              param.eColorFormat, (guint) param.nIndex);
          break;
        default:
          GST_DEBUG_OBJECT (self,
              "Component supports unsupported color format %d at index %u",
              param.eColorFormat, (guint) param.nIndex);
          break;
      }
    }
    old_index = param.nIndex++;
  } while (err == OMX_ErrorNone);

  return negotiation_map;
}

static OMX_ERRORTYPE
gst_omx_video_enc_reconfigure_output_port (GstOMXVideoEnc * self)
{

  GstOMXPort *port;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  port = self->enc_out_port;

  if (!gst_omx_port_is_enabled (port)) {
    err = gst_omx_port_set_enabled (port, TRUE);
    if (err != OMX_ErrorNone) {
      GST_INFO_OBJECT (self,
          "Failed to enable port: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      goto done;
    }
  }

  err = gst_omx_port_allocate_buffers (port);
  if (err != OMX_ErrorNone)
    goto done;

  err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
  if (err != OMX_ErrorNone)
    goto done;

  err = gst_omx_port_populate (port);
  if (err != OMX_ErrorNone)
    goto done;

  err = gst_omx_port_mark_reconfigured (port);
  if (err != OMX_ErrorNone)
    goto done;

done:
  return err;
}

static void
gst_omx_video_enc_check_nvfeatures (GstOMXVideoEnc * self,
    GstVideoCodecState * state)
{
  GstCapsFeatures *feature;

  feature = gst_caps_get_features (state->caps, 0);
  if (gst_caps_features_contains (feature, "memory:NVMM")) {
    OMX_ERRORTYPE err = OMX_ErrorNone;
    NVX_PARAM_USENVBUFFER param;
    OMX_INDEXTYPE eIndex;

    GST_DEBUG_OBJECT (self, "Setting encoder to use Nvmm buffer");

    err = gst_omx_component_get_index (self->enc,
        (char *) NVX_INDEX_CONFIG_USENVBUFFER, &eIndex);

    if (err == OMX_ErrorNone) {
      GST_OMX_INIT_STRUCT (&param);
      param.nPortIndex = self->enc_in_port->index;
      param.bUseNvBuffer = OMX_TRUE;
      err = gst_omx_component_set_parameter (self->enc, eIndex, &param);
    } else {
      GST_WARNING_OBJECT (self, "Coudn't get extension index for %s",
          (char *) NVX_INDEX_CONFIG_USENVBUFFER);
    }

    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't use HW Accelerated path");
    } else {
      self->hw_path = TRUE;
    }
  }
}

static OMX_ERRORTYPE
gstomx_set_rc_mode (GstOMXVideoEnc * self)
{
  OMX_INDEXTYPE eIndex;
  OMX_ERRORTYPE eError = OMX_ErrorNone;
  NVX_PARAM_RATECONTROLMODE oRCMode;

  GST_OMX_INIT_STRUCT (&oRCMode);
  oRCMode.nPortIndex = 0;
  eError =
      gst_omx_component_get_index (self->enc, (gpointer) NVX_INDEX_PARAM_RATECONTROLMODE,
      &eIndex);
  if (eError == OMX_ErrorNone) {
    switch (self->rc_mode) {
      case NVX_VIDEO_RateControlMode_CBR:
        oRCMode.eRateCtrlMode = NVX_VIDEO_RateControlMode_CBR;
        break;
      case NVX_VIDEO_RateControlMode_VBR:
        oRCMode.eRateCtrlMode = NVX_VIDEO_RateControlMode_VBR;
        break;
      case NVX_VIDEO_RateControlMode_VBR2:
        oRCMode.eRateCtrlMode = NVX_VIDEO_RateControlMode_VBR2;
        break;
      default:
        oRCMode.eRateCtrlMode = NVX_VIDEO_RateControlMode_VBR2;
        break;
    }

    eError = gst_omx_component_set_parameter (self->enc, eIndex, &oRCMode);
    if (eError != OMX_ErrorNone)
      GST_ERROR_OBJECT (self,
          "Failed to set rc_mode parameter: %s (0x%08x)",
          gst_omx_error_to_string (eError), eError);
  }
  return eError;
}

static OMX_ERRORTYPE
gstomx_set_bitrate (GstOMXVideoEnc * self)
{
  OMX_VIDEO_CONFIG_BITRATETYPE oBitrate;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  GST_OMX_INIT_STRUCT (&oBitrate);

  err =
      gst_omx_component_get_config (self->enc,
      OMX_IndexConfigVideoBitrate, &oBitrate);

  oBitrate.nEncodeBitrate = self->bitrate;

  err =
      gst_omx_component_set_config (self->enc,
      OMX_IndexConfigVideoBitrate, &oBitrate);
  if (err != OMX_ErrorNone)
    GST_ERROR_OBJECT (self,
        "Failed to set bitrate parameter: %s (0x%08x)",
        gst_omx_error_to_string (err), err);

  return err;
}

static gboolean
gst_omx_video_enc_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstOMXVideoEnc *self;
  GstOMXVideoEncClass *klass;
  OMX_ERRORTYPE err;
  gboolean needs_disable = FALSE;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  GstVideoInfo *info = &state->info;
  GList *negotiation_map = NULL, *l;

  self = GST_OMX_VIDEO_ENC (encoder);
  klass = GST_OMX_VIDEO_ENC_GET_CLASS (encoder);

  GST_DEBUG_OBJECT (self, "Setting new format %s",
      gst_video_format_to_string (info->finfo->format));

  gst_omx_port_get_port_definition (self->enc_in_port, &port_def);
  gst_omx_video_enc_check_nvfeatures (self, state);

  needs_disable =
      gst_omx_component_get_state (self->enc,
      GST_CLOCK_TIME_NONE) != OMX_StateLoaded;
  /* If the component is not in Loaded state and a real format change happens
   * we have to disable the port and re-allocate all buffers. If no real
   * format change happened we can just exit here.
   */
  if (needs_disable) {
    GST_DEBUG_OBJECT (self, "Need to disable and drain encoder");
    gst_omx_video_enc_drain (self, FALSE);
    gst_omx_port_set_flushing (self->enc_out_port, 5 * GST_SECOND, TRUE);

    /* Wait until the srcpad loop is finished,
     * unlock GST_VIDEO_ENCODER_STREAM_LOCK to prevent deadlocks
     * caused by using this lock from inside the loop function */
    GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
    gst_pad_stop_task (GST_VIDEO_ENCODER_SRC_PAD (encoder));
    GST_VIDEO_ENCODER_STREAM_LOCK (self);

    if (gst_omx_port_set_enabled (self->enc_in_port, FALSE) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_set_enabled (self->enc_out_port, FALSE) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_buffers_released (self->enc_in_port,
            5 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_buffers_released (self->enc_out_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_deallocate_buffers (self->enc_in_port) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_deallocate_buffers (self->enc_out_port) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_enabled (self->enc_in_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_enabled (self->enc_out_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    GST_DEBUG_OBJECT (self, "Encoder drained and disabled");
  }

  negotiation_map = gst_omx_video_enc_get_supported_colorformats (self);
  if (!negotiation_map) {
    /* Fallback */
    switch (info->finfo->format) {
      case GST_VIDEO_FORMAT_I420:
        port_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420Planar;
        break;
      case GST_VIDEO_FORMAT_NV12:
        port_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
        break;
      default:
        GST_ERROR_OBJECT (self, "Unsupported format %s",
            gst_video_format_to_string (info->finfo->format));
        return FALSE;
        break;
    }
  } else {
    for (l = negotiation_map; l; l = l->next) {
      VideoNegotiationMap *m = l->data;

      if (m->format == info->finfo->format) {
        port_def.format.video.eColorFormat = m->type;
        break;
      }
    }
    g_list_free_full (negotiation_map,
        (GDestroyNotify) video_negotiation_map_free);
  }

  port_def.format.video.nFrameWidth = info->width;
  if (port_def.nBufferAlignment)
    port_def.format.video.nStride =
        (info->width + port_def.nBufferAlignment - 1) &
        (~(port_def.nBufferAlignment - 1));
  else
    port_def.format.video.nStride = GST_ROUND_UP_4 (info->width);       /* safe (?) default */

  port_def.format.video.nFrameHeight = info->height;
  port_def.format.video.nSliceHeight = info->height;

  switch (port_def.format.video.eColorFormat) {
    case OMX_COLOR_FormatYUV420Planar:
    case OMX_COLOR_FormatYUV420PackedPlanar:
      port_def.nBufferSize =
          (port_def.format.video.nStride * port_def.format.video.nFrameHeight) +
          2 * ((port_def.format.video.nStride / 2) *
          ((port_def.format.video.nFrameHeight + 1) / 2));
      break;

    case OMX_COLOR_FormatYUV420SemiPlanar:
      port_def.nBufferSize =
          (port_def.format.video.nStride * port_def.format.video.nFrameHeight) +
          (port_def.format.video.nStride *
          ((port_def.format.video.nFrameHeight + 1) / 2));
      break;

    default:
      g_assert_not_reached ();
  }

  if (info->fps_n == 0) {
    port_def.format.video.xFramerate = 0;
  } else {
    if (!(klass->cdata.hacks & GST_OMX_HACK_VIDEO_FRAMERATE_INTEGER))
      port_def.format.video.xFramerate = (info->fps_n << 16) / (info->fps_d);
    else
      port_def.format.video.xFramerate = (info->fps_n) / (info->fps_d);
  }

  GST_DEBUG_OBJECT (self, "Setting inport port definition");
  if (gst_omx_port_update_port_definition (self->enc_in_port,
          &port_def) != OMX_ErrorNone)
    return FALSE;

  if (klass->set_format) {
    if (!klass->set_format (self, self->enc_in_port, state)) {
      GST_ERROR_OBJECT (self, "Subclass failed to set the new format");
      return FALSE;
    }
  }

  err = gstomx_set_rc_mode (self);
  if (err != OMX_ErrorNone) {
    GST_WARNING_OBJECT (self,
        "Error setting rc_mode %u : %s (0x%08x)",
        (guint) self->rc_mode, gst_omx_error_to_string (err), err);
    return FALSE;
  }

  err = gstomx_set_bitrate (self);
  if (err != OMX_ErrorNone) {
    GST_WARNING_OBJECT (self,
        "Error setting bitrate %u : %s (0x%08x)",
        (guint) self->bitrate, gst_omx_error_to_string (err), err);
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Updating outport port definition");
  if (gst_omx_port_update_port_definition (self->enc_out_port,
          NULL) != OMX_ErrorNone)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Enabling component");
  if (needs_disable) {
    if (gst_omx_port_set_enabled (self->enc_in_port, TRUE) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_allocate_buffers (self->enc_in_port) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_enabled (self->enc_in_port,
            5 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_mark_reconfigured (self->enc_in_port) != OMX_ErrorNone)
      return FALSE;
  } else {
    /* Disable output port */
    if (gst_omx_port_set_enabled (self->enc_out_port, FALSE) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_enabled (self->enc_out_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_set_state (self->enc, OMX_StateIdle) != OMX_ErrorNone)
      return FALSE;

    /* Need to allocate buffers to reach Idle state */
    if (gst_omx_port_allocate_buffers (self->enc_in_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->enc,
            GST_CLOCK_TIME_NONE) != OMX_StateIdle)
      return FALSE;

    if (gst_omx_component_set_state (self->enc,
            OMX_StateExecuting) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->enc,
            GST_CLOCK_TIME_NONE) != OMX_StateExecuting)
      return FALSE;
  }

#ifdef USE_OMX_TARGET_TEGRA
  /* PortSetting change event comes only if output port has buffers allocated */
  if (gst_omx_video_enc_reconfigure_output_port (self) != OMX_ErrorNone)
    return FALSE;
#endif

  /* Unset flushing to allow ports to accept data again */
  gst_omx_port_set_flushing (self->enc_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->enc_out_port, 5 * GST_SECOND, FALSE);

  if (gst_omx_component_get_last_error (self->enc) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Component in error state: %s (0x%08x)",
        gst_omx_component_get_last_error_string (self->enc),
        gst_omx_component_get_last_error (self->enc));
    return FALSE;
  }

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);

  /* Start the srcpad loop again */
  GST_DEBUG_OBJECT (self, "Starting task again");
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_ENCODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_video_enc_loop, encoder, NULL);

  return TRUE;
}

static gboolean
gst_omx_video_enc_reset (GstVideoEncoder * encoder, gboolean hard)
{
  GstOMXVideoEnc *self;

  self = GST_OMX_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Resetting encoder");

  gst_omx_port_set_flushing (self->enc_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->enc_out_port, 5 * GST_SECOND, TRUE);

  /* Wait until the srcpad loop is finished,
   * unlock GST_VIDEO_ENCODER_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_VIDEO_ENCODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_ENCODER_SRC_PAD (self));
  GST_VIDEO_ENCODER_STREAM_LOCK (self);

  gst_omx_port_set_flushing (self->enc_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->enc_out_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_populate (self->enc_out_port);

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_ENCODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_video_enc_loop, encoder, NULL);

  return TRUE;
}

static gboolean
gst_omx_video_enc_fill_buffer (GstOMXVideoEnc * self, GstBuffer * inbuf,
    GstOMXBuffer * outbuf)
{
  GstVideoCodecState *state = gst_video_codec_state_ref (self->input_state);
  GstVideoInfo *info = &state->info;
  OMX_PARAM_PORTDEFINITIONTYPE *port_def = &self->enc_in_port->port_def;
  gboolean ret = FALSE;
  GstVideoFrame frame;

  if (info->width != port_def->format.video.nFrameWidth ||
      info->height != port_def->format.video.nFrameHeight) {
    GST_ERROR_OBJECT (self, "Width or height do not match");
    goto done;
  }

  /* Same strides and everything */
  /*
   * If component is using HW acceleration path, No need for this check.
   * Because contents are not actual data but structures.
   * We can copy content of buffer directly.
   */
  if (self->hw_path || gst_buffer_get_size (inbuf) ==
      outbuf->omx_buf->nAllocLen - outbuf->omx_buf->nOffset) {
    outbuf->omx_buf->nFilledLen = gst_buffer_get_size (inbuf);

    gst_buffer_extract (inbuf, 0,
        outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset,
        outbuf->omx_buf->nFilledLen);
    ret = TRUE;
    goto done;
  }

  /* Different strides */

  switch (info->finfo->format) {
    case GST_VIDEO_FORMAT_I420:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      outbuf->omx_buf->nFilledLen = 0;

      if (!gst_video_frame_map (&frame, info, inbuf, GST_MAP_READ)) {
        GST_ERROR_OBJECT (self, "Invalid input buffer size");
        ret = FALSE;
        break;
      }

      for (i = 0; i < 3; i++) {
        if (i == 0) {
          dest_stride = port_def->format.video.nStride;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 0);

          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        } else {
          dest_stride = port_def->format.video.nStride / 2;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 1);

          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        }

        dest = outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset;
        if (i > 0)
          dest +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;
        if (i == 2)
          dest +=
              (port_def->format.video.nSliceHeight / 2) *
              (port_def->format.video.nStride / 2);

        src = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        width = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i);

        if (dest + dest_stride * height >
            outbuf->omx_buf->pBuffer + outbuf->omx_buf->nAllocLen) {
          gst_video_frame_unmap (&frame);
          GST_ERROR_OBJECT (self, "Invalid output buffer size");
          ret = FALSE;
          break;
        }

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          outbuf->omx_buf->nFilledLen += dest_stride;
          src += src_stride;
          dest += dest_stride;
        }
      }
      gst_video_frame_unmap (&frame);
      ret = TRUE;
      break;
    }
    case GST_VIDEO_FORMAT_NV12:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      outbuf->omx_buf->nFilledLen = 0;

      if (!gst_video_frame_map (&frame, info, inbuf, GST_MAP_READ)) {
        GST_ERROR_OBJECT (self, "Invalid input buffer size");
        ret = FALSE;
        break;
      }

      for (i = 0; i < 2; i++) {
        if (i == 0) {
          dest_stride = port_def->format.video.nStride;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 0);
          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        } else {
          dest_stride = port_def->format.video.nStride;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 1);

          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        }

        dest = outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset;
        if (i == 1)
          dest +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;

        src = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        width = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i) * (i == 0 ? 1 : 2);

        if (dest + dest_stride * height >
            outbuf->omx_buf->pBuffer + outbuf->omx_buf->nAllocLen) {
          gst_video_frame_unmap (&frame);
          GST_ERROR_OBJECT (self, "Invalid output buffer size");
          ret = FALSE;
          break;
        }

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          outbuf->omx_buf->nFilledLen += dest_stride;
          src += src_stride;
          dest += dest_stride;
        }

      }
      gst_video_frame_unmap (&frame);
      ret = TRUE;
      break;
    }
    default:
      GST_ERROR_OBJECT (self, "Unsupported format");
      goto done;
      break;
  }

done:

  gst_video_codec_state_unref (state);

  return ret;
}

static GstFlowReturn
gst_omx_video_enc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstOMXAcquireBufferReturn acq_ret = GST_OMX_ACQUIRE_BUFFER_ERROR;
  GstOMXVideoEnc *self;
  GstOMXPort *port;
  GstOMXBuffer *buf;
  OMX_ERRORTYPE err;

  self = GST_OMX_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Handling frame");

  if (self->eos) {
    GST_WARNING_OBJECT (self, "Got frame after EOS");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_EOS;
  }

  if (self->downstream_flow_ret != GST_FLOW_OK) {
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }

  port = self->enc_in_port;

  while (acq_ret != GST_OMX_ACQUIRE_BUFFER_OK) {
    BufferIdentification *id;
    GstClockTime timestamp, duration;

    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_VIDEO_ENCODER_STREAM_UNLOCK (self);
    acq_ret = gst_omx_port_acquire_buffer (port, &buf);

    if (acq_ret == GST_OMX_ACQUIRE_BUFFER_ERROR) {
      GST_VIDEO_ENCODER_STREAM_LOCK (self);
      goto component_error;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
      GST_VIDEO_ENCODER_STREAM_LOCK (self);
      goto flushing;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      /* Reallocate all buffers */
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_deallocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_allocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_ENCODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      /* Now get a new buffer and fill it */
      GST_VIDEO_ENCODER_STREAM_LOCK (self);
      continue;
    }
    GST_VIDEO_ENCODER_STREAM_LOCK (self);

    g_assert (acq_ret == GST_OMX_ACQUIRE_BUFFER_OK && buf != NULL);

    if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <= 0) {
      gst_omx_port_release_buffer (port, buf);
      goto full_buffer;
    }

    if (self->downstream_flow_ret != GST_FLOW_OK) {
      gst_omx_port_release_buffer (port, buf);
      goto flow_error;
    }

    /* Now handle the frame */
    GST_DEBUG_OBJECT (self, "Handling frame");

    if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame)) {
      OMX_CONFIG_INTRAREFRESHVOPTYPE config;

      GST_OMX_INIT_STRUCT (&config);
      config.nPortIndex = port->index;
      config.IntraRefreshVOP = OMX_TRUE;

      GST_DEBUG_OBJECT (self, "Forcing a keyframe");
      err =
          gst_omx_component_set_config (self->enc,
          OMX_IndexConfigVideoIntraVOPRefresh, &config);
      if (err != OMX_ErrorNone)
        GST_ERROR_OBJECT (self, "Failed to force a keyframe: %s (0x%08x)",
            gst_omx_error_to_string (err), err);
    }

    /* Copy the buffer content in chunks of size as requested
     * by the port */
    if (!gst_omx_video_enc_fill_buffer (self, frame->input_buffer, buf)) {
      gst_omx_port_release_buffer (port, buf);
      goto buffer_fill_error;
    }

    timestamp = frame->pts;
    if (timestamp != GST_CLOCK_TIME_NONE) {
      buf->omx_buf->nTimeStamp =
          gst_util_uint64_scale (timestamp, OMX_TICKS_PER_SECOND, GST_SECOND);
      self->last_upstream_ts = timestamp;
    }

    duration = frame->duration;
    if (duration != GST_CLOCK_TIME_NONE) {
      buf->omx_buf->nTickCount =
          gst_util_uint64_scale (buf->omx_buf->nFilledLen, duration,
          gst_buffer_get_size (frame->input_buffer));
      self->last_upstream_ts += duration;
    }
#ifdef USE_OMX_TARGET_TEGRA
    if (self->hw_path)
      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_RETAIN_OMX_TS;
#endif

    id = g_slice_new0 (BufferIdentification);
    id->timestamp = buf->omx_buf->nTimeStamp;
    gst_video_codec_frame_set_user_data (frame, id,
        (GDestroyNotify) buffer_identification_free);

    self->started = TRUE;
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;

    GST_DEBUG_OBJECT (self, "Passed frame to component");
  }

  gst_video_codec_frame_unref (frame);

  return self->downstream_flow_ret;

full_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Got OpenMAX buffer with no free space (%p, %u/%u)", buf,
            (guint) buf->omx_buf->nOffset, (guint) buf->omx_buf->nAllocLen));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

flow_error:
  {
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->enc),
            gst_omx_component_get_last_error (self->enc)));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- returning FLUSHING");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_FLUSHING;
  }
reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure input port"));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
buffer_fill_error:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
        ("Failed to write input into the OpenMAX buffer"));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
release_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase input buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_omx_video_enc_finish (GstVideoEncoder * encoder)
{
  GstOMXVideoEnc *self;

  self = GST_OMX_VIDEO_ENC (encoder);

  return gst_omx_video_enc_drain (self, TRUE);
}

static GstFlowReturn
gst_omx_video_enc_drain (GstOMXVideoEnc * self, gboolean at_eos)
{
  GstOMXVideoEncClass *klass;
  GstOMXBuffer *buf;
  GstOMXAcquireBufferReturn acq_ret;
  OMX_ERRORTYPE err;

  GST_DEBUG_OBJECT (self, "Draining component");

  klass = GST_OMX_VIDEO_ENC_GET_CLASS (self);

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Component not started yet");
    return GST_FLOW_OK;
  }
  self->started = FALSE;

  /* Don't send EOS buffer twice, this doesn't work */
  if (self->eos) {
    GST_DEBUG_OBJECT (self, "Component is EOS already");
    return GST_FLOW_OK;
  }
  if (at_eos)
    self->eos = TRUE;

  if ((klass->cdata.hacks & GST_OMX_HACK_NO_EMPTY_EOS_BUFFER)) {
    GST_WARNING_OBJECT (self, "Component does not support empty EOS buffers");
    return GST_FLOW_OK;
  }

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_VIDEO_ENCODER_STREAM_UNLOCK (self);

  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port. */
  acq_ret = gst_omx_port_acquire_buffer (self->enc_in_port, &buf);
  if (acq_ret != GST_OMX_ACQUIRE_BUFFER_OK) {
    GST_VIDEO_ENCODER_STREAM_LOCK (self);
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for draining: %d",
        acq_ret);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (&self->drain_lock);
  self->draining = TRUE;
  buf->omx_buf->nFilledLen = 0;
  buf->omx_buf->nTimeStamp =
      gst_util_uint64_scale (self->last_upstream_ts, OMX_TICKS_PER_SECOND,
      GST_SECOND);
  buf->omx_buf->nTickCount = 0;
  buf->omx_buf->nFlags |= OMX_BUFFERFLAG_EOS;
  err = gst_omx_port_release_buffer (self->enc_in_port, buf);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to drain component: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    GST_VIDEO_ENCODER_STREAM_LOCK (self);
    return GST_FLOW_ERROR;
  }
  GST_DEBUG_OBJECT (self, "Waiting until component is drained");
  g_cond_wait (&self->drain_cond, &self->drain_lock);
  GST_DEBUG_OBJECT (self, "Drained component");
  g_mutex_unlock (&self->drain_lock);
  GST_VIDEO_ENCODER_STREAM_LOCK (self);

  self->started = FALSE;

  return GST_FLOW_OK;
}

static gboolean
gst_omx_video_enc_propose_allocation (GstVideoEncoder * encoder,
    GstQuery * query)
{
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  return
      GST_VIDEO_ENCODER_CLASS
      (gst_omx_video_enc_parent_class)->propose_allocation (encoder, query);
}

GstCaps *
gst_omx_video_enc_negotiate_caps (GstVideoEncoder * encoder, GstCaps * caps,
    GstCaps * filter)
{
  GstCaps *templ_caps;
  GstCaps *allowed;
  GstCaps *fcaps, *filter_caps;
  GstCapsFeatures *feature;
  gint i, j;

  /* Allow downstream to specify width/height/framerate/PAR constraints
   * and forward them upstream for video converters to handle
   */
  templ_caps =
      caps ? gst_caps_ref (caps) :
      gst_pad_get_pad_template_caps (encoder->sinkpad);
  allowed = gst_pad_get_allowed_caps (encoder->srcpad);

  if (!allowed || gst_caps_is_empty (allowed) || gst_caps_is_any (allowed)) {
    fcaps = templ_caps;
    goto done;
  }

  GST_LOG_OBJECT (encoder, "template caps %" GST_PTR_FORMAT, templ_caps);
  GST_LOG_OBJECT (encoder, "allowed caps %" GST_PTR_FORMAT, allowed);

  filter_caps = gst_caps_new_empty ();

  for (i = 0; i < gst_caps_get_size (templ_caps); i++) {
    GQuark q_name =
        gst_structure_get_name_id (gst_caps_get_structure (templ_caps, i));
    gchar *f_name =
        gst_caps_features_to_string (gst_caps_get_features (templ_caps, i));

    for (j = 0; j < gst_caps_get_size (allowed); j++) {
      const GstStructure *allowed_s = gst_caps_get_structure (allowed, j);
      const GValue *val;
      GstStructure *s;

      s = gst_structure_new_id_empty (q_name);
      feature = gst_caps_features_new (f_name, NULL);
      if ((val = gst_structure_get_value (allowed_s, "width")))
        gst_structure_set_value (s, "width", val);
      if ((val = gst_structure_get_value (allowed_s, "height")))
        gst_structure_set_value (s, "height", val);
      if ((val = gst_structure_get_value (allowed_s, "framerate")))
        gst_structure_set_value (s, "framerate", val);
      if ((val = gst_structure_get_value (allowed_s, "pixel-aspect-ratio")))
        gst_structure_set_value (s, "pixel-aspect-ratio", val);

      filter_caps = gst_caps_merge_structure_full (filter_caps, s, feature);
    }
  }

  fcaps = gst_caps_intersect (filter_caps, templ_caps);
  gst_caps_unref (filter_caps);
  gst_caps_unref (templ_caps);

  if (filter) {
    GST_LOG_OBJECT (encoder, "intersecting with %" GST_PTR_FORMAT, filter);
    filter_caps = gst_caps_intersect (fcaps, filter);
    gst_caps_unref (fcaps);
    fcaps = filter_caps;
  }

done:
  gst_caps_replace (&allowed, NULL);

  GST_LOG_OBJECT (encoder, "proxy caps %" GST_PTR_FORMAT, fcaps);

  return fcaps;
}

static GstCaps *
gst_omx_video_enc_getcaps (GstVideoEncoder * encoder, GstCaps * filter)
{
  GstOMXVideoEnc *self = GST_OMX_VIDEO_ENC (encoder);
  GList *negotiation_map = NULL, *l;
  GstCaps *comp_supported_caps;
  GstCaps *ret;
  GstStructure *str;
  gint n;
  GValue list = G_VALUE_INIT;
  GValue val = G_VALUE_INIT;

  if (!self->enc)
    return gst_omx_video_enc_negotiate_caps (encoder, NULL, filter);

  negotiation_map = gst_omx_video_enc_get_supported_colorformats (self);
  comp_supported_caps = gst_pad_get_pad_template_caps (encoder->sinkpad);
  comp_supported_caps = gst_caps_make_writable (comp_supported_caps);

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_STRING);

  for (l = negotiation_map; l; l = l->next) {
    VideoNegotiationMap *map = l->data;

    g_value_set_static_string (&val, gst_video_format_to_string (map->format));
    gst_value_list_append_value (&list, &val);
  }

  if (!gst_caps_is_empty (comp_supported_caps)) {
    for (n = 0; n < gst_caps_get_size (comp_supported_caps); n++) {
      str = gst_caps_get_structure (comp_supported_caps, n);
      gst_structure_set_value (str, "format", &list);
    }
    ret =
        gst_omx_video_enc_negotiate_caps (encoder, comp_supported_caps, filter);
    gst_caps_unref (comp_supported_caps);
  } else {
    gst_caps_unref (comp_supported_caps);
    ret = gst_omx_video_enc_negotiate_caps (encoder, NULL, filter);
  }

  g_value_unset (&val);
  g_value_unset (&list);
  return ret;
}
