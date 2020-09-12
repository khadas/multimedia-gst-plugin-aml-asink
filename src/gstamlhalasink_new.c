/* GStreamer
 * Copyright (C) 2020 <song.zhao@amlogic.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstamlhalasink
 *
 * The amlhalasink element connects to Amlogic audio HAL service
 * and provide android like audio features.
 *
 */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <gst/audio/audio.h>
#include <audio_if_client.h>
#include <speex/speex_resampler.h>
#include "gstamlhalasink_new.h"
#include "ac4_frame_parse.h"

GST_DEBUG_CATEGORY_STATIC (gst_aml_hal_asink_debug_category);
#define GST_CAT_DEFAULT gst_aml_hal_asink_debug_category

#define GST_AML_HAL_ASINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_AML_HAL_ASINK, GstAmlHalAsinkPrivate))

//#define DUMP_TO_FILE
#define DEFAULT_VOLUME          1.0
#define MAX_VOLUME              1.0

#define MAX_TRANS_BUF_SIZE     0x8040
#define TRANS_DATA_OFFSET      0x40
//32KB
#define TRANS_DATA_SIZE        (MAX_TRANS_BUF_SIZE - TRANS_DATA_OFFSET)

#define is_raw_type(type) (type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW)
#define EXTEND_BUF_SIZE (4096*2*2)
#define DEFAULT_BUFFER_TIME     ((200 * GST_MSECOND) / GST_USECOND)
#define DEFAULT_LATENCY_TIME    ((10 * GST_MSECOND) / GST_USECOND)

#define GST_AUDIO_FORMAT_TYPE_AC4 100

#define TSYNC_ENABLE "/sys/class/tsync/enable"
#define TSYNC_EVENT  "/sys/class/tsync/event"
#define TSYNC_MODE   "/sys/class/tsync/mode"
#define TSYNC_PCRSCR "/sys/class/tsync/pts_pcrscr"
#define AUDIO_PAUSE_EVENT "AUDIO_PAUSE"
#define AUDIO_RESUME_EVENT "AUDIO_RESUME"
#define PTS_90K 90000

#ifdef DUMP_TO_FILE
static guint file_index;
#endif

struct _GstAmlHalAsinkPrivate
{
  audio_hw_device_t *hw_dev_;
  uint32_t output_port_;
  uint32_t direct_mode_;
  /* current cap */
  GstAudioRingBufferSpec spec;

  /* patch for vol control */
  struct audio_port_config source_;
  struct audio_port_config sink_;
  audio_patch_handle_t patch_h_;
  gdouble volume_;
  gboolean mute_;

  GCond   run_ready;
  GMutex  feed_lock;

  /* output stream */
  struct audio_stream_out *stream_;
  audio_format_t format_;
  uint32_t sr_;
  audio_channel_mask_t channel_mask_;

  gboolean paused_;
  gboolean flushing_;
  //gboolean flushed_;
  gboolean tsync_enable_;
  gboolean pcr_master_;
  gboolean received_eos;
  gboolean eos;
  guint32 seqnum; /* for eos */

  /* for bit stream */
  guint encoded_size;
  guint sample_per_frame;
  guint frame_sent;
  gboolean sync_frame;

  /* for header attaching */
  uint8_t *trans_buf;

  gboolean quit_clock_wait;
  GstClockTime eos_time;

  GstClockTime last_ts;
  guint64 render_samples;

  /* for position */
  guint wrapping_time;
  uint32_t last_pcr;
  uint32_t first_pts;
  gboolean first_pts_set;

  GstSegment segment;
  /* curent stream group */
  guint group_id;
  gboolean group_done;

  /* resmapler for trick play*/
  SpeexResamplerState *src;
  uint8_t *buf_src;
  guint32 buf_src_len;
  guint32 src_target_rate;
};

enum
{
  PROP_0,
  PROP_DIRECT_MODE,
  PROP_OUTPUT_PORT,
  PROP_VOLUME,
  PROP_MUTE,
  PROP_PCR_MASTER,
  PROP_LAST
};

#define COMMON_AUDIO_CAPS \
  "channels = (int) [ 1, MAX ], " \
  "rate = (int) [ 1, MAX ]"

/* pad templates */
static GstStaticPadTemplate gst_aml_hal_asink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
      "audio/x-raw,format=S16LE,rate=48000,"
      "channels=2,layout=interleaved; "
      "audio/x-ac3, "
      COMMON_AUDIO_CAPS "; "
      "audio/x-eac3, "
      COMMON_AUDIO_CAPS "; "
      "audio/x-ac4; "
      )
    );

#define GST_TYPE_AHAL_OUTPUT_PORT \
  (gst_ahal_output_port_get_type ())

static GType
gst_ahal_output_port_get_type (void)
{
  static GType ahal_output_port_type = 0;

  if (!ahal_output_port_type) {
    static const GEnumValue ahal_output_port[] = {
      {0, "Speaker", "speaker"},
      {1, "HDMI-Tx", "hdmitx"},
      {2, "HDMI ARC", "hdmi-arc"},
      {3, "SPDIF", "spdif"},
      {0, NULL, NULL},
    };

    ahal_output_port_type =
        g_enum_register_static ("AmlAsinkOutputPort", ahal_output_port);
  }

  return ahal_output_port_type;
}

/* class initialization */
#define gst_aml_hal_asink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstAmlHalAsink, gst_aml_hal_asink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_aml_hal_asink_debug_category, "amlhalasink", 0,
  "debug category for amlhalasink element");
  G_IMPLEMENT_INTERFACE (GST_TYPE_STREAM_VOLUME, NULL)
  );

static gboolean gst_aml_hal_asink_open (GstAmlHalAsink* sink);
static gboolean gst_aml_hal_asink_close (GstAmlHalAsink* asink);

static void gst_aml_hal_asink_dispose(GObject * object);

static void gst_aml_hal_asink_set_property(GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_aml_hal_asink_get_property(GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_aml_hal_asink_change_state(GstElement *
    element, GstStateChange transition);
static gboolean gst_aml_hal_asink_query(GstElement * element, GstQuery *
    query);

static GstClock *gst_aml_hal_asink_provide_clock(GstElement * elem);
static inline void gst_aml_hal_asink_reset_sync (GstAmlHalAsink * sink);
static GstClockTime gst_aml_hal_asink_get_time(GstClock * clock, GstAmlHalAsink * sink);

static GstFlowReturn gst_aml_hal_asink_render (GstAmlHalAsink * sink, GstBuffer * buffer);
static GstFlowReturn gst_aml_hal_asink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean gst_aml_hal_asink_event(GstAmlHalAsink *sink, GstEvent * event);
static gboolean gst_aml_hal_asink_pad_event (GstPad * pad, GstObject * parent, GstEvent * event);
static GstFlowReturn gst_aml_hal_asink_wait_event(GstBaseSink * bsink,
    GstEvent * event);
static void gst_aml_hal_asink_get_times(GstBaseSink * bsink,
    GstBuffer * buffer, GstClockTime * start, GstClockTime * end);
static gboolean gst_aml_hal_asink_setcaps (GstBaseSink * bsink,
    GstCaps * caps);

static gboolean hal_open_device (GstAmlHalAsink * sink);
static gboolean hal_close_device (GstAmlHalAsink* sink);
static gboolean hal_acquire (GstAmlHalAsink * sink, GstAudioRingBufferSpec * spec);
static gboolean hal_release (GstAmlHalAsink * sink);
static gboolean hal_start (GstAmlHalAsink * sink);
static gboolean hal_pause (GstAmlHalAsink * sink);
static gboolean hal_stop (GstAmlHalAsink * sink);
static guint hal_commit (GstAmlHalAsink * sink, guchar * data, gint size, guint64 pts_64);
static uint32_t hal_get_latency (GstAmlHalAsink * sink);
static int config_sys_node(const char* path, const char* value);
static int get_sysfs_uint32(const char *path, uint32_t *value);
static int tsync_enable (GstAmlHalAsink * sink, gboolean enable);
static void dump(const char* path, const uint8_t *data, int size);
static int tsync_send_audio_event(const char* event);

static void
gst_aml_hal_asink_class_init (GstAmlHalAsinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  g_type_class_add_private (klass, sizeof (GstAmlHalAsinkPrivate));

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS(klass),
      &gst_aml_hal_asink_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Amlogic audio HAL sink", "Sink/Audio", "gstream plugin to connect AML audio HAL",
      "song.zhao@amlogic.com");

  gobject_class->set_property = gst_aml_hal_asink_set_property;
  gobject_class->get_property = gst_aml_hal_asink_get_property;
  gobject_class->dispose = gst_aml_hal_asink_dispose;

  g_object_class_install_property (gobject_class, PROP_OUTPUT_PORT,
      g_param_spec_enum ("output-port", "Output Port",
          "select active output port for audio",
          GST_TYPE_AHAL_OUTPUT_PORT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DIRECT_MODE,
      g_param_spec_boolean ("direct-mode", "Direct Mode",
          "Select this mode for main mixing port, unselect it for system sound mixing port",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_VOLUME,
      g_param_spec_double ("volume", "Volume",
          "Linear volume of this stream, 1.0=100%", 0.0, MAX_VOLUME,
          DEFAULT_VOLUME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_MUTE,
      g_param_spec_boolean ("mute", "Mute",
          "Mute state of this stream", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_PCR_MASTER,
      g_param_spec_boolean ("pcr-master", "Enable PRC master avsync mode",
          "Enable this mode for DVB/ATSC playback", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_change_state);
  gstelement_class->provide_clock =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_provide_clock);
  gstelement_class->query = GST_DEBUG_FUNCPTR (gst_aml_hal_asink_query);

  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_aml_hal_asink_setcaps);
  gstbasesink_class->wait_event =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_wait_event);
  gstbasesink_class->get_times =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_get_times);

  /* ref class from a thread-safe context to work around missing bit of
   * thread-safety in GObject */
  g_type_class_ref (GST_TYPE_AUDIO_CLOCK);
}

static void
gst_aml_hal_asink_init (GstAmlHalAsink* sink)
{
  GstBaseSink *basesink;
  GstAmlHalAsinkPrivate *priv = GST_AML_HAL_ASINK_GET_PRIVATE (sink);

  sink->priv = priv;
  sink->provided_clock = gst_audio_clock_new ("GstAmlSinkClock",
      (GstAudioClockGetTimeFunc) gst_aml_hal_asink_get_time, sink,
      NULL);

  basesink = GST_BASE_SINK_CAST (sink);
  /* bypass sync control of basesink */
  gst_base_sink_set_sync(basesink, FALSE);
  gst_pad_set_event_function (basesink->sinkpad, gst_aml_hal_asink_pad_event);
  gst_pad_set_chain_function (basesink->sinkpad, gst_aml_hal_asink_chain);

  GST_OBJECT_FLAG_SET (basesink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);


  if (!gst_aml_hal_asink_open (sink))
    GST_ERROR_OBJECT(sink, "asink open failure");

  priv->direct_mode_ = TRUE;
  priv->quit_clock_wait = FALSE;
  priv->received_eos = FALSE;
  priv->group_id = -1;
  g_mutex_init (&priv->feed_lock);
  g_cond_init (&priv->run_ready);
}

static void
gst_aml_hal_asink_dispose (GObject * object)
{
  GstAmlHalAsink * sink = GST_AML_HAL_ASINK(object);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (sink->provided_clock) {
    gst_audio_clock_invalidate (sink->provided_clock);
    gst_object_unref (sink->provided_clock);
    sink->provided_clock = NULL;
  }

  gst_aml_hal_asink_close (sink);
  g_mutex_clear (&priv->feed_lock);
  g_cond_clear (&priv->run_ready);
  G_OBJECT_CLASS (parent_class)->dispose (object);
}


static GstClock *
gst_aml_hal_asink_provide_clock (GstElement * elem)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (sink);
  clock = GST_CLOCK_CAST (gst_object_ref (sink->provided_clock));
  GST_OBJECT_UNLOCK (sink);

  return clock;
}

static gboolean
gst_aml_hal_asink_is_self_provided_clock (GstAmlHalAsink* sink)
{
  return (sink->provided_clock && GST_IS_AUDIO_CLOCK (sink->provided_clock) &&
      GST_AUDIO_CLOCK_CAST (sink->provided_clock)->func ==
      (GstAudioClockGetTimeFunc) gst_aml_hal_asink_get_time);
}

static gboolean
get_position (GstAmlHalAsink* sink, GstFormat format, gint64 * cur)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  uint32_t pcr = 0;
  gint64 timepassed_90k, timepassed;

  if (!priv->tsync_enable_ || !priv->render_samples) {
    *cur = 0;
    return TRUE;
  }

  if (!priv->direct_mode_) {
    GST_WARNING_OBJECT(sink, "not well implemented");
    *cur = gst_util_uint64_scale_int(priv->render_samples, GST_SECOND, priv->sr_);
    GST_LOG_OBJECT (sink, "POSITION: %" GST_TIME_FORMAT, GST_TIME_ARGS (*cur));
    return FALSE;
  }

  get_sysfs_uint32(TSYNC_PCRSCR, &pcr);
  if (priv->last_pcr > 0xF0000000 && pcr < 10*PTS_90K) {
    priv->wrapping_time++;
    GST_INFO_OBJECT (sink, "pts wrapping num: %d", priv->wrapping_time);
  }
  priv->last_pcr = pcr;

  if (priv->wrapping_time <= 1)
    timepassed_90k = (int)(pcr - priv->first_pts);
  else
    timepassed_90k = (int)(pcr - priv->first_pts) + (priv->wrapping_time-1)*0xFFFFFFFFLL;

  timepassed = gst_util_uint64_scale_int (timepassed_90k, GST_SECOND, PTS_90K);
  *cur += priv->segment.start + timepassed;

  GST_LOG_OBJECT (sink, "POSITION: %" GST_TIME_FORMAT, GST_TIME_ARGS (*cur));
  if (GST_FORMAT_TIME != format) {
    gboolean ret;

    /* convert to final format */
    ret = gst_audio_info_convert (&priv->spec.info, GST_FORMAT_TIME, *cur, format, cur);
    if (!ret)
      return FALSE;
  }

  return TRUE;
}

static gboolean
gst_aml_hal_asink_query (GstElement * element, GstQuery * query)
{
  gboolean res = FALSE;
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (element);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  sink = GST_AML_HAL_ASINK (element);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      gint64 cur = 0;
      GstFormat format;
      gst_query_parse_position (query, &format, NULL);

      /* first try to get the position based on the clock */
      if ((res = get_position (sink, format, &cur))) {
        gst_query_set_position (query, format, cur);
        GST_LOG_OBJECT (sink, "position %lld format %s", cur, gst_format_get_name (format));
      }
      break;
    }

    case GST_QUERY_LATENCY:
    {
      gboolean live, us_live;
      GstClockTime min_l, max_l;

      GST_TRACE_OBJECT (sink, "latency query");

      /* ask parent first, it will do an upstream query for us. */
      if ((res =
              gst_base_sink_query_latency (GST_BASE_SINK_CAST (sink), &live,
                  &us_live, &min_l, &max_l))) {
        GstClockTime base_latency, min_latency, max_latency;

        /* we and upstream are both live, adjust the min_latency */
        if (live && us_live) {
          uint32_t latency;

          GST_OBJECT_LOCK (sink);
          latency = hal_get_latency (sink);
          GST_OBJECT_UNLOCK (sink);

          base_latency =
              gst_util_uint64_scale_int (latency, GST_SECOND, 1000);

          /* we cannot go lower than the buffer size and the min peer latency */
          min_latency = base_latency + min_l;
          /* the max latency is the max of the peer, we can delay an infinite
           * amount of time. */
          max_latency = (max_l == -1) ? -1 : (base_latency + max_l);

          GST_DEBUG_OBJECT (sink,
              "peer min %" GST_TIME_FORMAT ", our min latency: %"
              GST_TIME_FORMAT, GST_TIME_ARGS (min_l),
              GST_TIME_ARGS (min_latency));
          GST_DEBUG_OBJECT (sink,
              "peer max %" GST_TIME_FORMAT ", our max latency: %"
              GST_TIME_FORMAT, GST_TIME_ARGS (max_l),
              GST_TIME_ARGS (max_latency));
        } else {
          GST_TRACE_OBJECT (sink, "peer or we are not live, don't care about latency");
          min_latency = min_l;
          max_latency = max_l;
        }
        gst_query_set_latency (query, live, min_latency, max_latency);
      }
      break;
    }
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      GST_LOG_OBJECT (sink, "query convert");

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, NULL);
      GST_OBJECT_LOCK (sink);
      res = gst_audio_info_convert (&priv->spec.info, src_fmt,
          src_val, dest_fmt, &dest_val);
      GST_OBJECT_UNLOCK (sink);
      if (res) {
        gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      }
      break;
    }
    default:
      res = GST_ELEMENT_CLASS (parent_class)->query (element, query);
      break;
  }

  return res;
}

static int get_sysfs_uint32(const char *path, uint32_t *value)
{
    int fd;
    char valstr[64];
    uint32_t val = 0;

    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        memset(valstr, 0, 64);
        read(fd, valstr, 64 - 1);
        valstr[strlen(valstr)] = '\0';
        close(fd);
    } else {
        GST_ERROR("unable to open file %s", path);
        return -1;
    }
    if (sscanf(valstr, "0x%x", &val) < 1) {
        GST_ERROR("unable to get pts from: %s", valstr);
        return -1;
    }
    *value = val;
    return 0;
}

/* we call this function without holding the lock on sink for performance
 * reasons. Try hard to not deal with and invalid ringbuffer and rate. */
static GstClockTime gst_aml_hal_asink_get_time (GstClock * clock, GstAmlHalAsink * sink)
{
  GstClockTime result = GST_CLOCK_TIME_NONE;
  GstAmlHalAsinkPrivate *priv = sink->priv;
  uint32_t pcr = 0;

  if (!priv->tsync_enable_) {
    GST_TRACE_OBJECT(sink, "tsync disabled");
    return result;
  }
  if (!priv->render_samples) {
    result = priv->segment.start;
  } else {
    get_sysfs_uint32(TSYNC_PCRSCR, &pcr);
    result = gst_util_uint64_scale_int (pcr, GST_SECOND, PTS_90K);
  }

  GST_LOG_OBJECT (sink, "time %" GST_TIME_FORMAT " 0x%x", GST_TIME_ARGS (result), pcr);
  return result;
}

static gdouble
gst_aml_hal_sink_get_volume (GstAmlHalAsink * sink)
{
  double result;

  result = gst_stream_volume_convert_volume(GST_STREAM_VOLUME_FORMAT_DB,
    GST_STREAM_VOLUME_FORMAT_LINEAR, sink->priv->volume_);
  GST_LOG_OBJECT(sink, "return vol %f", result);
  return result;
}

static void
gst_aml_hal_sink_set_volume (GstAmlHalAsink * sink, gdouble volume)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  struct audio_port_config config;
  int ret;
  GST_DEBUG_OBJECT (sink, "set vol:%f", volume);

  volume  = gst_stream_volume_convert_volume (GST_STREAM_VOLUME_FORMAT_LINEAR,
          GST_STREAM_VOLUME_FORMAT_DB, volume);

  memset(&config, 0, sizeof(config));
  config.id = 2;
  config.role = AUDIO_PORT_ROLE_SINK;
  config.type = AUDIO_PORT_TYPE_DEVICE;
  config.config_mask = AUDIO_PORT_CONFIG_GAIN;
  /* audio_hal use dB * 100 to keep the accuracy */
  config.gain.values[0] = volume * 100;

  ret = priv->hw_dev_->set_audio_port_config(priv->hw_dev_, &config);
  if (ret) {
    GST_ERROR_OBJECT(sink, "port_config faile:%d",ret);
  } else {
    GST_LOG_OBJECT(sink, "hal volume set to %f", volume);
    priv->volume_ = volume;
  }
}

static void
gst_aml_hal_sink_set_mute (GstAmlHalAsink * sink, gboolean mute)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int ret;

  GST_DEBUG_OBJECT (sink, "set mute:%d", mute);
  //TODO: implement in audio hal, mute the amplifier
  ret = priv->hw_dev_->set_master_mute( priv->hw_dev_, mute);
  if (ret)
    GST_ERROR_OBJECT(sink, "mute fail:%d", ret);
  else
    priv->mute_ = mute;
}


static void
gst_aml_hal_asink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (object);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  switch (property_id) {
    case PROP_DIRECT_MODE:
      priv->direct_mode_ = g_value_get_boolean(value);
      GST_DEBUG_OBJECT (sink, "set direct mode:%d", priv->direct_mode_);
      break;
    case PROP_OUTPUT_PORT:
      priv->output_port_ = g_value_get_enum (value);
      GST_DEBUG_OBJECT (sink, "set output port:%d", priv->output_port_);
      break;
    case PROP_VOLUME:
      gst_aml_hal_sink_set_volume (sink, g_value_get_double (value));
      break;
    case PROP_MUTE:
      gst_aml_hal_sink_set_mute (sink, g_value_get_boolean (value));
      break;
    case PROP_PCR_MASTER:
      priv->pcr_master_ = g_value_get_boolean(value);
      GST_DEBUG_OBJECT (sink, "pcr master mode:%d", priv->pcr_master_);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void gst_aml_hal_asink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (object);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  switch (property_id) {
    case PROP_DIRECT_MODE:
      g_value_set_boolean(value, priv->direct_mode_);
      GST_DEBUG_OBJECT (sink, "get direct mode:%d", priv->direct_mode_);
      break;
    case PROP_OUTPUT_PORT:
      g_value_set_enum(value, priv->output_port_);
      GST_DEBUG_OBJECT (sink, "get output port:%d", priv->output_port_);
      break;
    case PROP_VOLUME:
      g_value_set_double (value, gst_aml_hal_sink_get_volume(sink));
      break;
    case PROP_MUTE:
      g_value_set_boolean (value, priv->mute_);
      break;
    case PROP_PCR_MASTER:
      g_value_set_boolean (value, priv->pcr_master_);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static gboolean
parse_caps (GstAudioRingBufferSpec * spec, GstCaps * caps)
{
  const gchar *mimetype;
  GstStructure *structure;
  GstAudioInfo info;

  structure = gst_caps_get_structure (caps, 0);
  gst_audio_info_init (&info);

  /* we have to differentiate between int and float formats */
  mimetype = gst_structure_get_name (structure);

  if (g_str_equal (mimetype, "audio/x-raw")) {
    if (!gst_audio_info_from_caps (&info, caps))
      goto parse_error;

    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW;
  } else if (g_str_equal (mimetype, "audio/x-iec958")) {
    /* extract the needed information from the cap */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
      goto parse_error;

    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_IEC958;
    info.bpf = 4;
  } else if (g_str_equal (mimetype, "audio/x-ac3")) {
    /* extract the needed information from the cap */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
      goto parse_error;

    gst_structure_get_int (structure, "channels", &info.channels);
    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_AC3;
    info.bpf = 4;
  } else if (g_str_equal (mimetype, "audio/x-ac4")) {
    spec->type = GST_AUDIO_FORMAT_TYPE_AC4;
    /* to pass the sanity check in render() */
    info.bpf = 1;
    info.channels = 2;
    info.rate = 48000;
  } else if (g_str_equal (mimetype, "audio/x-eac3")) {
    /* extract the needed information from the cap */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
      goto parse_error;

    gst_structure_get_int (structure, "channels", &info.channels);
    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3;
    info.bpf = 16;
  } else if (g_str_equal (mimetype, "audio/x-dts")) {
    /* extract the needed information from the cap */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
      goto parse_error;

    gst_structure_get_int (structure, "channels", &info.channels);
    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_DTS;
    info.bpf = 4;
  } else {
    goto parse_error;
  }

  gst_caps_replace (&spec->caps, caps);
  spec->info = info;

  return TRUE;

  /* ERRORS */
parse_error:
  {
    GST_DEBUG ("could not parse caps");
    return FALSE;
  }
}

static gboolean gst_aml_hal_asink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (bsink);
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstAudioRingBufferSpec *spec;

  spec = &priv->spec;

  if (G_UNLIKELY (spec->caps && gst_caps_is_equal (spec->caps, caps))) {
    GST_DEBUG_OBJECT (sink,
        "caps haven't changed, skipping reconfiguration");
    return TRUE;
  }

  GST_DEBUG_OBJECT (sink, "release old hal");

  /* release old ringbuffer */
  GST_OBJECT_LOCK (sink);
  hal_pause (sink);
  hal_release (sink);
  GST_OBJECT_UNLOCK (sink);

  GST_DEBUG_OBJECT (sink, "parse caps");

  spec->buffer_time = DEFAULT_BUFFER_TIME;
  spec->latency_time = DEFAULT_LATENCY_TIME;

  /* parse new caps */
  if (!parse_caps (spec, caps))
    goto parse_error;

  GST_DEBUG_OBJECT (sink, "acquire");
  GST_OBJECT_LOCK (sink);
  if (!hal_acquire (sink, spec)) {
    GST_OBJECT_UNLOCK (sink);
    goto acquire_error;
  }
  GST_OBJECT_UNLOCK (sink);

  /* If we use our own clock, we need to adjust the offset since it will now
   * restart from zero */
  if (gst_aml_hal_asink_is_self_provided_clock (sink))
    gst_audio_clock_reset (GST_AUDIO_CLOCK (sink->provided_clock), 0);

  /* We need to resync since the ringbuffer restarted */
  gst_aml_hal_asink_reset_sync (sink);

  gst_element_post_message (GST_ELEMENT_CAST (bsink),
      gst_message_new_latency (GST_OBJECT (bsink)));

  return TRUE;

  /* ERRORS */
parse_error:
  {
    GST_DEBUG_OBJECT (sink, "could not parse caps");
    GST_ELEMENT_ERROR (sink, STREAM, FORMAT,
        (NULL), ("cannot parse audio format."));
    return FALSE;
  }
acquire_error:
  {
    GST_DEBUG_OBJECT (sink, "could not acquire ringbuffer");
    return FALSE;
  }
}

static inline void gst_aml_hal_asink_reset_sync (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  priv->eos_time = -1;
  priv->eos = FALSE;
  priv->last_ts = GST_CLOCK_TIME_NONE;
  priv->render_samples = 0;
  priv->flushing_ = FALSE;
  priv->first_pts_set = FALSE;
  priv->wrapping_time = 0;
  priv->last_pcr = 0;
}

static void gst_aml_hal_asink_get_times (GstBaseSink * bsink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  /* our clock sync is a bit too much for the base class to handle so
   * we implement it ourselves. */
  *start = GST_CLOCK_TIME_NONE;
  *end = GST_CLOCK_TIME_NONE;
}

static void sink_force_start (GstAmlHalAsink * sink)
{
  hal_start (sink);
}

static GstClockReturn sink_wait_clock (GstAmlHalAsink * sink, GstClockTime time)
{
  GstClockReturn ret;
  GstClock *clock;
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (time)))
    goto invalid_time;

  clock = sink->provided_clock;

  GST_INFO_OBJECT (sink,
      "time %" GST_TIME_FORMAT , GST_TIME_ARGS (time));

  GstClockTime now = gst_aml_hal_asink_get_time (clock, sink);
  if (now == GST_CLOCK_TIME_NONE) {
    ret = GST_CLOCK_UNSCHEDULED;
    goto exit;
  }

  if (now > time) {
    GST_DEBUG_OBJECT (sink, "now: %lld", now);
    ret = GST_CLOCK_EARLY;
    goto exit;
  }

  do {
    now = gst_aml_hal_asink_get_time (clock, sink);
    if (now == GST_CLOCK_TIME_NONE) {
      ret = GST_CLOCK_UNSCHEDULED;
      break;
    }

    if (now < time) {
      if (priv->quit_clock_wait) {
        ret = GST_CLOCK_UNSCHEDULED;
        break;
      }
      usleep (30000);
      GST_TRACE_OBJECT (sink, "now: %lld", now);
      continue;
    } else {
      ret = GST_CLOCK_OK;
      break;
    }
  } while (1);

exit:
  priv->quit_clock_wait = FALSE;
  return ret;

  /* no syncing needed */
invalid_time:
  {
    GST_DEBUG_OBJECT (sink, "time not valid, no sync needed");
    return GST_CLOCK_BADTIME;
  }
}

/* This waits for the drain to happen and can be canceled */
static GstFlowReturn sink_drain (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstFlowReturn ret = GST_FLOW_OK;

  if (!priv->stream_)
    return ret;
  if (!priv->spec.info.rate)
    return ret;

  GST_DEBUG_OBJECT (sink, "draining");

  if (priv->eos_time != -1 && !priv->group_done) {
    GstClockReturn cret;
    GST_DEBUG_OBJECT (sink,
        "last sample time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (priv->eos_time));

    hal_commit (sink, NULL, 0, -1);
    /* wait for the EOS time to be reached, this is the time when the last
     * sample is played. */
    cret = sink_wait_clock (sink, priv->eos_time);

    GST_INFO_OBJECT (sink, "drained ret: %d", cret);
    if (cret == GST_CLOCK_OK || cret == GST_CLOCK_EARLY)
      ret = GST_FLOW_OK;
    else
      ret = GST_FLOW_ERROR;
  }
  return ret;
}

static GstFlowReturn
gst_aml_hal_asink_wait_event (GstBaseSink * bsink, GstEvent * event)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (bsink);
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstFlowReturn ret = GST_FLOW_OK;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_GAP:
      /* We must have a negotiated format before starting */
      if (G_UNLIKELY (!priv->stream_)) {
        GST_ELEMENT_ERROR (sink, STREAM, FORMAT, (NULL),
            ("Sink not negotiated before %s event.",
                GST_EVENT_TYPE_NAME (event)));
        return GST_FLOW_ERROR;
      }

      GST_OBJECT_LOCK (sink);
      sink_force_start (sink);
      GST_OBJECT_UNLOCK (sink);
      break;
    default:
      break;
  }

  return ret;
}

static void update_pcr_speed(float rate)
{
    int value = 1000000 * rate;
    char value_str[20];

    snprintf(value_str, 20, "%d", value);
    config_sys_node("/sys/class/video/vsync_slow_factor", value_str);
}

static gboolean
gst_aml_hal_asink_event (GstAmlHalAsink *sink, GstEvent * event)
{
  gboolean result = TRUE;
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstBaseSink* bsink = GST_BASE_SINK_CAST (sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
    {
      GstMessage *message;
      GstFlowReturn ret;

      priv->received_eos = TRUE;
      ret = sink_drain (sink);
      if (G_UNLIKELY (ret != GST_FLOW_OK)) {
        result = FALSE;
        goto done;
      }
      GST_OBJECT_LOCK (sink);
      priv->eos = TRUE;
      GST_OBJECT_UNLOCK (sink);

      /* ok, now we can post the message */
      GST_DEBUG_OBJECT (sink, "Now posting EOS");
      priv->seqnum = gst_event_get_seqnum (event);
      GST_DEBUG_OBJECT (sink, "Got seqnum #%" G_GUINT32_FORMAT, priv->seqnum);

      message = gst_message_new_eos (GST_OBJECT_CAST (sink));
      gst_message_set_seqnum (message, priv->seqnum);
      gst_element_post_message (GST_ELEMENT_CAST (sink), message);
      break;
    }
    case GST_EVENT_FLUSH_START:
    {
      GST_DEBUG_OBJECT (sink, "flush start");

      GST_OBJECT_LOCK (sink);
      priv->received_eos = FALSE;
      priv->flushing_ = TRUE;
      /* unblock hal_commit() */
      g_mutex_lock(&priv->feed_lock);
      g_cond_signal (&priv->run_ready);
      g_mutex_unlock(&priv->feed_lock);
      GST_OBJECT_UNLOCK (sink);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      gboolean reset_time;

      gst_event_parse_flush_stop (event, &reset_time);
      GST_DEBUG_OBJECT (sink, "flush stop");
      GST_OBJECT_LOCK (sink);
      hal_stop (sink);
      tsync_enable (sink, TRUE);
      GST_OBJECT_UNLOCK (sink);

      gst_aml_hal_asink_reset_sync (sink);
      if (reset_time) {
        GST_DEBUG_OBJECT (sink, "posting reset-time message");
        gst_element_post_message (GST_ELEMENT_CAST (sink),
            gst_message_new_reset_time (GST_OBJECT_CAST (sink), 0));
      }
#ifdef DUMP_TO_FILE
    file_index++;
#endif
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      gst_event_copy_segment (event, &priv->segment);
      GST_DEBUG_OBJECT (sink, "configured segment %" GST_SEGMENT_FORMAT,
          &priv->segment);

      /* prepare for trick play */
      if (priv->segment.rate != 1 && priv->segment.rate != 0
              && priv->format_ == AUDIO_FORMAT_PCM_16_BIT) {
          int err;

          if (priv->src)
              speex_resampler_destroy(priv->src);

          priv->src_target_rate = (guint32)(48000/priv->segment.rate);
          priv->src = speex_resampler_init(2, 48000,
                  priv->src_target_rate,
                  SPEEX_RESAMPLER_QUALITY_DEFAULT, &err);
          if (!priv->src) {
              GST_ERROR_OBJECT(sink, "can not create src");
              result = FALSE;
              goto done;
          }
          GST_INFO_OBJECT (sink, "SRC created, rate:%f target_rate:%d",
              priv->segment.rate, priv->src_target_rate);
          if (priv->direct_mode_) {
              update_pcr_speed(priv->segment.rate);
              GST_INFO_OBJECT (sink, "pcr rate updated");
          }
      } else if (priv->segment.rate == 1) {
          if (priv->src) {
              speex_resampler_destroy(priv->src);
              priv->src = NULL;
              if (priv->direct_mode_) {
                  update_pcr_speed(1);
                  GST_INFO_OBJECT (sink, "pcr rate recovered");
              }
              GST_INFO_OBJECT (sink, "SRC destroyed");
          }
          if (priv->buf_src) {
              g_free (priv->buf_src);
              priv->buf_src = NULL;
              priv->buf_src_len = 0;
          }
      }
      break;
    }
    case GST_EVENT_STREAM_START:
    {
      guint group_id;

      gst_event_parse_group_id (event, &group_id);
      GST_DEBUG_OBJECT (sink, "group change from %d to %d",
          priv->group_id, group_id);
      priv->group_id = group_id;
      priv->group_done = FALSE;
      GST_DEBUG_OBJECT (sink, "stream start, gid %d", group_id);
      return GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);
    }
    case GST_EVENT_STREAM_GROUP_DONE:
    {
      guint group_id;

      gst_event_parse_stream_group_done (event, &group_id);
      if (priv->group_id != group_id) {
        GST_WARNING_OBJECT (sink, "group id not match: %d vs %d",
            priv->group_id, group_id);
      } else {
        GST_DEBUG_OBJECT (sink, "stream group done, gid %d", group_id);
      }
      GST_OBJECT_LOCK (sink);
      hal_stop (sink);
      tsync_enable (sink, TRUE);
      priv->group_done = TRUE;
      GST_OBJECT_UNLOCK (sink);
      gst_aml_hal_asink_reset_sync (sink);
    }
    default:
    {
      GST_DEBUG_OBJECT (sink, "pass to basesink");
      return GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);
    }
  }
done:
  gst_event_unref (event);
  return result;
}

static gboolean
gst_aml_hal_asink_pad_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (parent);
  GstAmlHalAsinkPrivate *priv = sink->priv;
  gboolean result = TRUE;

  GST_DEBUG_OBJECT (sink, "received event %p %" GST_PTR_FORMAT, event, event);

  if (GST_EVENT_IS_SERIALIZED (event)) {
    if (G_UNLIKELY (priv->flushing_) &&
        GST_EVENT_TYPE (event) != GST_EVENT_FLUSH_STOP)
      goto flushing;

    if (G_UNLIKELY (priv->received_eos))
      goto after_eos;
  }

  result = gst_aml_hal_asink_event (sink, event);
done:
  GST_DEBUG_OBJECT (sink, "done");
  return result;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (sink, "we are flushing");
    gst_event_unref (event);
    result = FALSE;
    goto done;
  }

after_eos:
  {
    GST_DEBUG_OBJECT (sink, "Event received after EOS, dropping");
    gst_event_unref (event);
    result = FALSE;
    goto done;
  }
}

static int trick_play (GstAmlHalAsink * sink, uint8_t *data, gint len)
{
    GstAmlHalAsinkPrivate *priv = sink->priv;
    uint32_t in_sam = len / 4, out_sam;
    int ret;
    int buf_size = len / priv->segment.rate + 10;

    if (priv->buf_src_len < buf_size) {
        uint8_t *tmp;
        tmp = g_realloc (priv->buf_src, buf_size);
        if (!tmp) {
            GST_ERROR_OBJECT (sink, "oom");
            g_free (priv->buf_src);
            return -1;
        } else {
            priv->buf_src_len = buf_size;
            priv->buf_src = tmp;
        }
    }
    out_sam = priv->buf_src_len/4;
    ret = speex_resampler_process_interleaved_int (priv->src,
            (spx_int16_t *)data, &in_sam,
            (spx_int16_t *)priv->buf_src, &out_sam);
    if (ret != RESAMPLER_ERR_SUCCESS) {
        GST_ERROR_OBJECT (sink, "speex fail %d", ret);
    }
    if (in_sam != len / 4) {
        GST_WARNING_OBJECT (sink, "not fully processed %d vs %d",
            in_sam, len / 4);
    }
    return out_sam * 4;
}

static GstFlowReturn
gst_aml_hal_asink_render (GstAmlHalAsink * sink, GstBuffer * buf)
{
  GstClockTime time, stop;
  GstAmlHalAsinkPrivate *priv = sink->priv;
  guint64 ctime, cstop;
  gsize size;
  guint samples;
  gint bpf, rate;
  GstFlowReturn ret = GST_FLOW_OK;
  GstSegment clip_seg;
  GstMapInfo info;
  guchar * data;

  if (priv->flushing_) {
    ret = GST_FLOW_FLUSHING;
    goto done;
  }

  if (G_UNLIKELY (!priv->stream_))
    goto wrong_state;

  if (G_UNLIKELY (priv->received_eos))
    goto was_eos;

  bpf = GST_AUDIO_INFO_BPF (&priv->spec.info);
  rate = GST_AUDIO_INFO_RATE (&priv->spec.info);

  size = gst_buffer_get_size (buf);
  if (G_UNLIKELY (size % bpf) != 0)
    goto wrong_size;

  if (is_raw_type(priv->spec.type))
    samples = size / bpf;
  else
    samples = priv->sample_per_frame;

  time = GST_BUFFER_TIMESTAMP (buf);

  if (!GST_CLOCK_TIME_IS_VALID (time) ||
      (priv->last_ts != GST_CLOCK_TIME_NONE && priv->last_ts == time)) {
    time = gst_util_uint64_scale_int(priv->render_samples, GST_SECOND, rate);
    GST_LOG_OBJECT (sink, "fake time %" GST_TIME_FORMAT, GST_TIME_ARGS (time));
  }

  GST_LOG_OBJECT (sink,
      "time %" GST_TIME_FORMAT ", start %"
      GST_TIME_FORMAT ", samples %u", GST_TIME_ARGS (time),
      GST_TIME_ARGS (priv->segment.start), samples);
  priv->last_ts = GST_BUFFER_TIMESTAMP (buf);

  /* let's calc stop based on the number of samples in the buffer instead
   * of trusting the DURATION */
  stop = time + gst_util_uint64_scale_int (samples, GST_SECOND, rate);

  clip_seg.format = GST_FORMAT_TIME;
  clip_seg.start = priv->segment.start;
  clip_seg.stop = priv->segment.stop;
  clip_seg.duration = -1;

  /* samples should be rendered based on their timestamp. All samples
   * arriving before the segment.start or after segment.stop are to be
   * thrown away. All samples should also be clipped to the segment
   * boundaries */
  if (G_UNLIKELY (!gst_segment_clip (&clip_seg, GST_FORMAT_TIME, time, stop,
              &ctime, &cstop)))
    goto out_of_segment;

  priv->eos_time = cstop;

  gst_buffer_map (buf, &info, GST_MAP_READ);
  data = info.data;
  size = info.size;

  //trick play
  if (priv->src) {
      int rc;
      rc = trick_play (sink, data, size);
      if (rc > 0) {
          data = priv->buf_src;
          size = rc;
      }
      GST_LOG_OBJECT (sink, "SRC in:%d out:%d", info.size, size);
  }

  g_mutex_lock(&priv->feed_lock);
  /* blocked on paused */
  while (priv->paused_ && !priv->flushing_)
    g_cond_wait (&priv->run_ready, &priv->feed_lock);

  if (priv->flushing_) {
    GST_DEBUG_OBJECT (sink, "interrupted by stop");
    gst_buffer_unmap (buf, &info);
    g_mutex_unlock(&priv->feed_lock);
    goto done;
  }

  hal_commit (sink, data, size, time);
  g_mutex_unlock(&priv->feed_lock);
  gst_buffer_unmap (buf, &info);
  priv->render_samples += samples;

  ret = GST_FLOW_OK;
done:
  gst_buffer_unref (buf);
  return ret;

was_eos:
  {
    GST_DEBUG_OBJECT (sink, "we are EOS, return EOS");
    ret = GST_FLOW_EOS;
    goto done;
  }
  /* SPECIAL cases */
out_of_segment:
  {
    GST_INFO_OBJECT (sink,
        "dropping sample out of segment time %" GST_TIME_FORMAT ", start %"
        GST_TIME_FORMAT, GST_TIME_ARGS (time),
        GST_TIME_ARGS (priv->segment.start));
    ret = GST_FLOW_OK;
    goto done;
  }
wrong_state:
  {
    GST_DEBUG_OBJECT (sink, "not negotiated");
    GST_ELEMENT_ERROR (sink, STREAM, FORMAT, (NULL), ("sink not negotiated."));
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto done;
  }
wrong_size:
  {
    GST_ERROR_OBJECT (sink, "wrong size %d/%d", size, bpf);
    GST_ELEMENT_ERROR (sink, STREAM, WRONG_TYPE,
        (NULL), ("sink received buffer of wrong size."));
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static GstFlowReturn
gst_aml_hal_asink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (parent);

  return gst_aml_hal_asink_render (sink, buf);
}

static GstStateChangeReturn
gst_aml_hal_asink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (element);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
    {
      GST_DEBUG_OBJECT(sink, "null to ready");
      gst_audio_clock_reset (GST_AUDIO_CLOCK (sink->provided_clock), 0);

      GST_OBJECT_LOCK (sink);
      if (!hal_open_device (sink)) {
        GST_OBJECT_UNLOCK (sink);
        goto open_failed;
      }
      GST_OBJECT_UNLOCK (sink);
      break;
    }
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_DEBUG_OBJECT(sink, "ready to paused");
      gst_base_sink_set_async_enabled (GST_BASE_SINK_CAST(sink), FALSE);
      gst_aml_hal_asink_reset_sync (sink);
      /* start in paused state until PLAYING */
      priv->paused_ = TRUE;

      /* Only post clock-provide messages if this is the clock that
       * we've created. If the subclass has overriden it the subclass
       * should post this messages whenever necessary */
      if (gst_aml_hal_asink_is_self_provided_clock (sink))
        gst_element_post_message (element,
            gst_message_new_clock_provide (GST_OBJECT_CAST (element),
              sink->provided_clock, TRUE));
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    {
      GST_DEBUG_OBJECT(sink, "paused to playing");

      GST_OBJECT_LOCK (sink);
      hal_start (sink);
      GST_OBJECT_UNLOCK (sink);

      if (priv->eos) {
        GstMessage *message;

        /* need to post EOS message here */
        GST_DEBUG_OBJECT (sink, "Now posting EOS");
        message = gst_message_new_eos (GST_OBJECT_CAST (sink));
        gst_message_set_seqnum (message, priv->seqnum);
        gst_element_post_message (GST_ELEMENT_CAST (sink), message);
      }
      break;
    }
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    {
      GST_DEBUG_OBJECT(sink, "playing to paused");
      GST_OBJECT_LOCK (sink);
      hal_pause (sink);
      GST_OBJECT_UNLOCK (sink);

      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_DEBUG_OBJECT(sink, "paused to ready");
      /* Only post clock-lost messages if this is the clock that
       * we've created. If the subclass has overriden it the subclass
       * should post this messages whenever necessary */
      if (gst_aml_hal_asink_is_self_provided_clock (sink))
        gst_element_post_message (element,
            gst_message_new_clock_lost (GST_OBJECT_CAST (element),
              sink->provided_clock));

      /* make sure we unblock before calling the parent state change
       * so it can grab the STREAM_LOCK */
      GST_OBJECT_LOCK (sink);
      hal_release (sink);
      priv->quit_clock_wait = TRUE;
      if (priv->src) {
          speex_resampler_destroy (priv->src);
          priv->src = NULL;
          if (priv->direct_mode_) {
              update_pcr_speed(1);
              GST_INFO_OBJECT (sink, "pcr rate recovered");
          }
      }
      if (priv->buf_src) {
          g_free (priv->buf_src);
          priv->buf_src = NULL;
          priv->buf_src_len = 0;
      }
      GST_OBJECT_UNLOCK (sink);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop slaving ourselves to the master, if any */
      gst_clock_set_master (sink->provided_clock, NULL);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_DEBUG_OBJECT(sink, "ready to null");
      GST_OBJECT_LOCK (sink);
      hal_close_device (sink);
      GST_OBJECT_UNLOCK (sink);
      break;
    default:
      break;
  }

  return ret;

open_failed:
  {
    /* subclass must post a meaningful error message */
    GST_DEBUG_OBJECT (sink, "open failed");
    return GST_STATE_CHANGE_FAILURE;
  }
}

/* open the device with given specs */
static gboolean gst_aml_hal_asink_open (GstAmlHalAsink* sink)
{
  int ret;
  GstAmlHalAsinkPrivate *priv = sink->priv;

  GST_DEBUG_OBJECT (sink, "open");
  ret = audio_hw_load_interface(&priv->hw_dev_);
  if (ret) {
    GST_ERROR_OBJECT(sink, "fail to load hw:%d", ret);
    return FALSE;
  }
  GST_DEBUG_OBJECT (sink, "load hw done");

  priv->source_.id = 1;
  priv->source_.role = AUDIO_PORT_ROLE_SOURCE;
  priv->source_.type = AUDIO_PORT_TYPE_MIX;
  priv->source_.config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE |
    AUDIO_PORT_CONFIG_FORMAT;
  priv->source_.sample_rate = 48000;
  priv->source_.format = AUDIO_FORMAT_PCM_16_BIT;

  priv->sink_.id = 2;
  priv->sink_.role = AUDIO_PORT_ROLE_SINK;
  priv->sink_.type = AUDIO_PORT_TYPE_DEVICE;
  priv->sink_.config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE |
    AUDIO_PORT_CONFIG_FORMAT;
  priv->sink_.sample_rate = 48000;
  priv->sink_.format = AUDIO_FORMAT_PCM_16_BIT;
  priv->sink_.ext.device.type = AUDIO_DEVICE_OUT_SPEAKER;

  GST_DEBUG_OBJECT(sink, "create mix --> speaker patch...");
  ret = priv->hw_dev_->create_audio_patch(priv->hw_dev_,
      1, &priv->source_,
      1, &priv->sink_,
      &priv->patch_h_);
  if (ret)
    GST_ERROR_OBJECT(sink, "patch fail ret:%d",ret);
  else
    GST_DEBUG_OBJECT(sink, "success");

  return TRUE;
}

static gboolean gst_aml_hal_asink_close (GstAmlHalAsink* sink)
{
  int ret;
  GstAmlHalAsinkPrivate *priv = sink->priv;

  GST_DEBUG_OBJECT(sink, "close");
  if (priv->patch_h_) {
    ret = priv->hw_dev_->release_audio_patch(priv->hw_dev_, priv->patch_h_);
    if (ret)
      GST_ERROR_OBJECT(sink, "destroy patch fail ret:%d",ret);
    priv->patch_h_ = 0;
    GST_DEBUG_OBJECT(sink, "patch destroyed");
  }
  audio_hw_unload_interface(priv->hw_dev_);
  GST_DEBUG_OBJECT(sink, "unload hw");
  return TRUE;
}


/* will be called when the device should be opened. In this case we will connect
 * to the server. We should not try to open any streams in this state. */
static gboolean hal_open_device (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (priv->direct_mode_) {
    priv->trans_buf = (uint8_t *)g_malloc(MAX_TRANS_BUF_SIZE);
    if (!priv->trans_buf) {
      GST_ERROR_OBJECT (sink, "OOM");
      return FALSE;
    }
  }

  return TRUE;
}

/* close the device */
static gboolean hal_close_device (GstAmlHalAsink* sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GST_LOG_OBJECT (sink, "closing device");

  if (priv->stream_)
    priv->hw_dev_->close_output_stream(priv->hw_dev_,
        priv->stream_);

  if (priv->trans_buf)
    g_free(priv->trans_buf);
  GST_LOG_OBJECT (sink, "closed device");
  return TRUE;
}

static gboolean
hal_parse_spec (GstAmlHalAsink * sink, GstAudioRingBufferSpec * spec)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  gint channels;

  switch (spec->type) {
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW:
      switch (GST_AUDIO_INFO_FORMAT (&spec->info)) {
        case GST_AUDIO_FORMAT_S16LE:
          priv->format_ = AUDIO_FORMAT_PCM_16_BIT;
          break;
        default:
          goto error;
      }
      break;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_AC3:
      priv->format_ = AUDIO_FORMAT_AC3;
      break;
    case GST_AUDIO_FORMAT_TYPE_AC4:
      priv->format_ = AUDIO_FORMAT_AC4;
      break;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3:
      priv->format_ = AUDIO_FORMAT_E_AC3;
      break;
    default:
      goto error;

  }
  priv->sr_ = GST_AUDIO_INFO_RATE (&spec->info);
  channels = GST_AUDIO_INFO_CHANNELS (&spec->info);

  if (!priv->direct_mode_ && channels != 2) {
    GST_ERROR_OBJECT (sink, "unsupported channel number:%d", channels);
    goto error;
  }

  if (channels == 2)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_STEREO;
  else if (channels == 6)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_5POINT1;
  else if (channels == 8)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_7POINT1;
  else {
    GST_ERROR_OBJECT (sink, "unsupported channel number:%d", channels);
    goto error;
  }
  GST_DEBUG_OBJECT (sink, "format:0x%x, sr:%d, ch:%d",
      priv->format_, priv->sr_, channels);
  GST_DEBUG_OBJECT (sink, "buffer_time:%lld, peroid_time:%lld",
      spec->buffer_time, spec->latency_time);

  return TRUE;

  /* ERRORS */
error:
  return FALSE;
}

/* prepare resources and state to operate with the given specs */
static gboolean
aml_open_output_stream (GstAmlHalAsink * sink, GstAudioRingBufferSpec * spec)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  struct audio_config config;
  int ret;
  audio_output_flags_t flag;
  audio_devices_t device;

  GST_DEBUG_OBJECT (sink, "prepare");

  if (!hal_parse_spec (sink, spec))
    return FALSE;

  memset(&config, 0, sizeof(config));
  config.sample_rate = priv->sr_;
  config.channel_mask = priv->channel_mask_;
  config.format = priv->format_;

  if (priv->direct_mode_)
    flag = AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_HW_AV_SYNC;
  else
    flag = AUDIO_OUTPUT_FLAG_PRIMARY;

  if (priv->output_port_ == 0)
    device = AUDIO_DEVICE_OUT_SPEAKER;
  else if (priv->output_port_ == 1)
    device = AUDIO_DEVICE_OUT_HDMI;
  else if (priv->output_port_ == 2)
    device = AUDIO_DEVICE_OUT_HDMI_ARC;
  else if (priv->output_port_ == 3)
    device = AUDIO_DEVICE_OUT_SPDIF;
  else {
    GST_ERROR_OBJECT(sink, "invalid port:%d", priv->output_port_);
    return FALSE;
  }

  ret = priv->hw_dev_->open_output_stream(priv->hw_dev_,
      0, device,
      flag, &config,
      &priv->stream_, NULL);
  if (ret) {
    GST_ERROR_OBJECT(sink, "can not open output stream:%d", ret);
    return FALSE;
  }
  GST_DEBUG_OBJECT (sink, "done");
  return TRUE;
}

/* This method should create a new stream of the given @spec. No playback should
 * start yet so we start in the corked state. */
static gboolean hal_acquire (GstAmlHalAsink * sink,
    GstAudioRingBufferSpec * spec)
{
  if (!aml_open_output_stream (sink, spec))
    return FALSE;

  /* TODO:: configure volume when we changed it, else we leave the default */
  GST_DEBUG_OBJECT(sink, "done");
  return TRUE;
}

/* free the stream that we acquired before */
static gboolean hal_release (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GST_INFO_OBJECT (sink, "enter");

  hal_stop(sink);
  if (priv->stream_) {
    priv->hw_dev_->close_output_stream(priv->hw_dev_, priv->stream_);
    priv->stream_ = NULL;
    priv->paused_ = FALSE;
    priv->render_samples = 0;
  }

  GST_INFO_OBJECT(sink, "done");
  return TRUE;
}

static int config_sys_node(const char* path, const char* value)
{
  int fd;
  fd = open(path, O_RDWR);
  if (fd < 0) {
    GST_ERROR("fail to open %s", path);
    return -1;
  }
  if (write(fd, value, strlen(value)) != strlen(value)) {
    GST_ERROR("fail to write %s to %s", value, path);
    close(fd);
    return -1;
  }
  close(fd);

  return 0;
}

static int tsync_send_audio_event(const char* event)
{
  char *val;
  val = "AUDIO_PAUSE";
  return config_sys_node(TSYNC_EVENT, val);
}

static int tsync_enable (GstAmlHalAsink * sink, gboolean enable)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (enable && priv->direct_mode_ && !priv->tsync_enable_) {
    config_sys_node(TSYNC_ENABLE, "1");
    priv->tsync_enable_ = TRUE;
    GST_DEBUG_OBJECT (sink, "tsync enable");
    if (!priv->pcr_master_)
      config_sys_node(TSYNC_MODE, "1");//audio master
    else
      config_sys_node(TSYNC_MODE, "2");//pcr master
    return 0;
  }

  if (!enable && priv->direct_mode_ && priv->tsync_enable_) {
    config_sys_node(TSYNC_ENABLE, "0");
    priv->tsync_enable_ = FALSE;
    GST_DEBUG_OBJECT (sink, "tsync disable");
  }
  return 0;
}

#define HAL_WRONG_STAT 3
static gboolean hal_start (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GST_DEBUG_OBJECT (sink, "enter");

  if (!priv->stream_) {
    GST_INFO_OBJECT (sink, "stream not created yet");
    priv->paused_ = FALSE;
  } else {
    tsync_send_audio_event(AUDIO_RESUME_EVENT);
    g_mutex_lock(&priv->feed_lock);
    if (priv->paused_) {
      int ret;

      ret = priv->stream_->resume(priv->stream_);
      if (ret)
        GST_WARNING_OBJECT (sink, "resume failure:%d", ret);

      GST_DEBUG_OBJECT (sink, "resume");
      priv->paused_ = FALSE;
      g_cond_signal (&priv->run_ready);
    }
    g_mutex_unlock(&priv->feed_lock);
  }

  tsync_enable (sink, TRUE);
  return TRUE;
}

/* pause/stop playback ASAP */
static gboolean hal_pause (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int ret;

  GST_INFO_OBJECT (sink, "enter");
  if (!priv->stream_) {
    return FALSE;
  }

  tsync_send_audio_event(AUDIO_PAUSE_EVENT);
  g_mutex_lock(&priv->feed_lock);
  if (priv->paused_) {
    g_mutex_unlock(&priv->feed_lock);
    GST_DEBUG_OBJECT (sink, "already in pause state");
    return TRUE;
  }

  ret = priv->stream_->pause(priv->stream_);
  if (ret)
    GST_WARNING_OBJECT (sink, "pause failure:%d", ret);

  priv->paused_ = TRUE;
  g_mutex_unlock(&priv->feed_lock);
  GST_INFO_OBJECT (sink, "done");
  return TRUE;
}

/* stop playback, we flush everything. */
static gboolean hal_stop (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int ret;

  GST_DEBUG_OBJECT (sink, "enter");
  if (!priv->stream_) {
    return FALSE;
  }

  priv->stream_->pause(priv->stream_);

  ret = priv->stream_->flush(priv->stream_);
  if (ret) {
    GST_ERROR_OBJECT (sink, "pause failure:%d", ret);
    return FALSE;
  }

  g_mutex_lock (&priv->feed_lock);
  priv->flushing_ = TRUE;
  g_cond_signal (&priv->run_ready);
  g_mutex_unlock (&priv->feed_lock);

  tsync_enable (sink, FALSE);
  GST_DEBUG_OBJECT (sink, "stop");
  return TRUE;
}

static guint table_5_13[38][4] = {
  {96, 69, 64, 32},
  {96, 70, 64, 32},
  {120, 87, 80, 40},
  {120, 88, 80, 40},
  {144, 104, 96, 48},
  {144, 105, 96, 48},
  {168, 121, 112, 56},
  {168, 122, 112, 56},
  {192, 139, 128, 64},
  {192, 140, 128, 64},
  {240, 174, 160, 80},
  {240, 175, 160, 80},
  {288, 208, 192, 96},
  {288, 209, 192, 96},
  {336, 243, 224, 112},
  {336, 244, 224, 112},
  {384, 278, 256, 128},
  {384, 279, 256, 128},
  {480, 348, 320, 160},
  {480, 349, 320, 160},
  {576, 417, 384, 192},
  {576, 418, 384, 192},
  {672, 487, 448, 224},
  {672, 488, 448, 224},
  {768, 557, 512, 256},
  {768, 558, 512, 256},
  {960, 696, 640, 320},
  {960, 697, 640, 320},
  {1152, 835, 768, 384},
  {1152, 836, 768, 384},
  {1344, 975, 896, 448},
  {1344, 976, 896, 448},
  {1536, 1114, 1024, 512},
  {1536, 1115, 1024, 512},
  {1728, 1253, 1152, 576},
  {1728, 1254, 1152, 576},
  {1920, 1393, 1280, 640},
  {1920, 1394, 1280, 640}
};

static int parse_bit_stream(GstAmlHalAsink *sink,
    guchar * data, gint size)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstAudioRingBufferSpec * spec = &priv->spec;

  if (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_AC3) {
    /* Digital Audio Compression Standard (AC-3 E-AC-3) 5.3 */
    guint8 frmsizecod;
    guint8 fscod;

    if (size < 5) {
      return -1;
    }

    /* check sync word */
    if (data[0] != 0x0b || data[1] != 0x77)
      return -1;

    fscod = (data[4] >> 6);
    frmsizecod = data[4]&0x3F;

    GST_LOG_OBJECT (sink, "fscod:%d frmsizecod:%d", fscod, frmsizecod);
    if (fscod > 2)
      return -1;
    if (frmsizecod > 37)
      return -1;

    priv->encoded_size = table_5_13[frmsizecod][2 - fscod] * 2;
    priv->sample_per_frame = 1536;
    GST_LOG_OBJECT (sink, "encoded_size:%d", priv->encoded_size);
    return 0;
  } else if (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3) {
    /* Digital Audio Compression Standard (AC-3 E-AC-3) Annex E */
    guint16 frmsizecod;
    guint8 fscod, fscod2;
    guint8 numblkscod;

    if (size < 5) {
      return -1;
    }

    /* check sync word */
    if (data[0] != 0x0b || data[1] != 0x77)
      return -1;

    fscod = (data[4] >> 6);
    frmsizecod = data[3] + ((data[2]&0x7) << 8) + 1;

    GST_LOG_OBJECT (sink, "fscod:%d frmsizecod:%d", fscod, frmsizecod);
    if (fscod > 3)
      return -1;
    if (frmsizecod > 2048)
      return -1;

    if (fscod == 3) {
      fscod2 = (data[4] >> 4) & 0x3;
      GST_LOG_OBJECT (sink, "fscod2:%d", fscod2);
      if (fscod2 == 0)
        priv->sr_ = 24000;
      else if (fscod2 == 1)
        priv->sr_ = 22050;
      else if (fscod2 == 2)
        priv->sr_ = 16000;
      else {
        return -1;
      }
      priv->sample_per_frame = 256*6;
    } else {
      numblkscod = (data[4] >> 4) & 0x3;
      GST_LOG_OBJECT (sink, "numblkscod:%d", numblkscod);
      if (numblkscod == 0)
        priv->sample_per_frame = 256;
      else if (numblkscod == 1)
        priv->sample_per_frame = 256 * 2;
      else if (numblkscod == 2)
        priv->sample_per_frame = 256 * 3;
      else if (numblkscod == 3)
        priv->sample_per_frame = 256 * 6;
    }
    priv->encoded_size = frmsizecod * 2;
    GST_LOG_OBJECT (sink, "encoded_size:%d spf:%d",
        priv->encoded_size, priv->sample_per_frame);
    return 0;

  } else if (spec->type == GST_AUDIO_FORMAT_TYPE_AC4) {
    struct ac4_info info;

    if (ac4_toc_parse(data, size, &info)) {
      GST_ERROR_OBJECT (sink, "parse ac4 fail");
      dump("/tmp/ac4.dat", data, size);
      return -1;
    }
    priv->sample_per_frame = info.samples_per_frame;
    priv->sr_ = info.frame_rate;
    priv->spec.info.rate = priv->sr_;
    priv->sync_frame = info.sync_frame;
    GST_INFO_OBJECT (sink, "sr:%d spf:%d sync:%d",
      priv->sr_, priv->sample_per_frame, priv->sync_frame);
    return 0;
  }
  return -1;
}

struct hw_sync_header_v2 {
  uint8_t version[4];
  uint8_t size[4]; /* big endian */
  uint8_t pts[8];  /* big endian */
  uint8_t offset[4]; /* big endian */
};

static void hw_sync_set_ver(struct hw_sync_header_v2* header)
{
  header->version[0] = 0x55;
  header->version[1] = 0x55;
  header->version[2] = 0;
  header->version[3] = 0x02;
}

static void hw_sync_set_size(struct hw_sync_header_v2* header, uint32_t size)
{
  header->size[0] = (size&0xFF000000) >> 24;
  header->size[1] = (size&0xFF0000) >> 16;
  header->size[2] = (size&0xFF00) >> 8;
  header->size[3] = (size&0xFF);
}

static void hw_sync_set_pts(struct hw_sync_header_v2* header, uint64_t pts_64)
{
  header->pts[0] = (pts_64&0xFF00000000000000ull) >> 56;
  header->pts[1] = (pts_64&0x00ff000000000000ull) >> 48;
  header->pts[2] = (pts_64&0x0000ff0000000000ull) >> 40;
  header->pts[3] = (pts_64&0x000000ff00000000ull) >> 32;
  header->pts[4] = (pts_64&0x00000000ff000000ull) >> 24;
  header->pts[5] = (pts_64&0x0000000000ff0000ull) >> 16;
  header->pts[6] = (pts_64&0x000000000000ff00ull) >> 8;
  header->pts[7] = (pts_64&0x00000000000000ffull);
}

static void hw_sync_set_offset(struct hw_sync_header_v2* header, uint32_t offset)
{
  header->offset[0] = (offset & 0xFF000000) >> 24;
  header->offset[1] = (offset & 0xFF0000) >> 16;
  header->offset[2] = (offset & 0xFF00) >> 8;
  header->offset[3] = (offset & 0xFF);
}

static void dump(const char* path, const uint8_t *data, int size) {
#ifdef DUMP_TO_FILE
    char name[50];
    FILE* fd;

    sprintf(name, "%s%d.dat", path, file_index);
    fd = fopen(name, "ab");

    if (!fd)
        return;
    fwrite(data, 1, size, fd);
    fclose(fd);
#endif
}

static guint hal_commit (GstAmlHalAsink * sink, guchar * data,
    gint size, guint64 pts_64)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  guint towrite;
  gboolean raw_data;
  guint offset = 0;
  guint parsed = 0;
  guint hw_header_s = sizeof (struct hw_sync_header_v2);

  raw_data = is_raw_type(priv->spec.type);
  towrite = size;

  if (towrite)
    dump ("/data/asink_", data, towrite);

  /* Frame aligned */
  /* AC4 has constant bit rate (CBR) and variable bit reate(VBR) streams
   * And VBR doesn't have to be encoded size aligned
   */
  if (towrite && !raw_data && priv->format_ != AUDIO_FORMAT_AC4) {
    while (!parse_bit_stream(sink, data + parsed, towrite - parsed)
            && parsed < towrite)
      parsed += priv->encoded_size;
    if (parsed != towrite) {
      GST_ERROR_OBJECT(sink, "not frame aligned %d %d", towrite, priv->encoded_size);
      //dump("/tmp/err.dat", data, towrite);
      return 0;
    }
  }

  /* notify EOS */
  if (!towrite && priv->direct_mode_) {
    struct hw_sync_header_v2 header;

    hw_sync_set_ver(&header);
    hw_sync_set_size(&header, 0);
    hw_sync_set_pts(&header, -1);
    hw_sync_set_offset(&header, 0);
    priv->stream_->write(priv->stream_, &header, hw_header_s);
    return 0;
  }

  while (towrite > 0) {
    gboolean trans = false;
    int header_size = 0;
    int written;
    int cur_size;
    uint64_t pts_inc = 0;
    guchar * trans_data = NULL;

    if (!raw_data && priv->format_ != AUDIO_FORMAT_AC4)
      cur_size = priv->encoded_size;
    else
      cur_size = towrite;

    if (priv->format_ == AUDIO_FORMAT_AC4 && !priv->sync_frame) {
        int32_t ac4_header_len;
        uint8_t *header;

        header = ac4_syncframe_header(towrite, &ac4_header_len);
        header_size += ac4_header_len;

        if (cur_size > TRANS_DATA_SIZE) {
          GST_ERROR_OBJECT(sink, "frame too big %d", cur_size);
          return offset;
        }
        memcpy(priv->trans_buf + TRANS_DATA_OFFSET,
                data, cur_size);
        trans_data = priv->trans_buf + TRANS_DATA_OFFSET - ac4_header_len;
        memcpy(trans_data, header, ac4_header_len);
        cur_size += ac4_header_len;
        trans = true;
    }
    if (priv->direct_mode_) {
      struct hw_sync_header_v2 *hw_sync;
      uint32_t pts_32 = gst_util_uint64_scale_int (pts_64, PTS_90K, GST_SECOND);

      //truncate to 32bit PTS
      pts_64 = gst_util_uint64_scale_int(pts_32, GST_SECOND, PTS_90K);

      if (!trans) {
        hw_sync = (struct hw_sync_header_v2 *)priv->trans_buf;

        if (cur_size > MAX_TRANS_BUF_SIZE - hw_header_s) {
          GST_ERROR_OBJECT(sink, "frame too big %d", cur_size);
          return offset;
        }
        memcpy(priv->trans_buf + sizeof (*hw_sync),
          data, cur_size);
        trans_data = priv->trans_buf;
        header_size += hw_header_s;
        trans = true;
      } else {
        header_size += hw_header_s;
        if (header_size > TRANS_DATA_OFFSET) {
          GST_ERROR_OBJECT(sink, "header too big %d", header_size);
          return offset;
        }
        hw_sync = (struct hw_sync_header_v2 *)(trans_data - hw_header_s);
        trans_data -= hw_header_s;
      }

      hw_sync_set_ver(hw_sync);
      hw_sync_set_size(hw_sync, cur_size);
      hw_sync_set_pts(hw_sync, pts_64);
      hw_sync_set_offset(hw_sync, 0);
      cur_size += hw_header_s;

      if (!priv->first_pts_set) {
         priv->first_pts_set = TRUE;
         priv->first_pts = pts_32;
         GST_INFO_OBJECT(sink, "update first PTS %x", pts_32);
      }
    }

    if (trans) {
      written = priv->stream_->write(priv->stream_, trans_data, cur_size);
      if (written ==  cur_size)
        written -= header_size;
      else {
        GST_ERROR_OBJECT (sink, "trans mode write fail %d/%d", written, cur_size);
        return written;
      }
    } else {
      /* should consume all the PCM data */
      written = priv->stream_->write(priv->stream_, data, cur_size);
    }

    towrite -= written;
    data += written;

    /* update PTS for next sample */
    if (priv->direct_mode_) {
      if (priv->sr_)
        pts_inc = gst_util_uint64_scale_int (priv->sample_per_frame, GST_SECOND, priv->sr_);
      else
        GST_WARNING_OBJECT (sink, "invalid sample rate %d",  priv->sr_);
      pts_64 += pts_inc;
    }

    GST_LOG_OBJECT (sink,
        "write %d/%d left %d ts: %llu", written, cur_size, towrite, pts_64);
  }

  if (!raw_data)
    priv->frame_sent++;

  return size;
}

static uint32_t hal_get_latency (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int latency = 0;

  GST_DEBUG_OBJECT (sink, "enter");
  if (!priv->stream_) {
    GST_ERROR_OBJECT (sink, "null pointer");
    return 0;
  }

  priv->stream_->pause(priv->stream_);

  latency = priv->stream_->get_latency(priv->stream_);

  GST_DEBUG_OBJECT (sink, "latency %u", latency);
  return latency;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "amlhalasink", GST_RANK_PRIMARY,
      GST_TYPE_AML_HAL_ASINK);
}

#ifndef VERSION
#define VERSION "0.1.0"
#endif
#ifndef PACKAGE
#define PACKAGE "aml_package"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "aml_media"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://amlogic.com"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    amlhalasink,
    "Amlogic plugin for audio rendering",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
