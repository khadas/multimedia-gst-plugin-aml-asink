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

#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <math.h>
#include <gst/audio/audio.h>
#include <stdlib.h>
#include <time.h>
#include <audio_if_client.h>
#include "gstamlhalasink_new.h"
#include "gstamlclock.h"
#include "ac4_frame_parse.h"
#include "scaletempo.h"
#include "aml_avsync.h"
#include "aml_avsync_log.h"

#ifdef ESSOS_RM
#include "essos-resmgr.h"
#endif

GST_DEBUG_CATEGORY (gst_aml_hal_asink_debug_category);
#define GST_CAT_DEFAULT gst_aml_hal_asink_debug_category

#define GST_AML_HAL_ASINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_AML_HAL_ASINK, GstAmlHalAsinkPrivate))

//#define DUMP_TO_FILE
#define DEFAULT_VOLUME          1.0
#define MAX_VOLUME              1.0

#define MAX_TRANS_BUF_SIZE     0x6000
#define TRANS_DATA_OFFSET      0x40
//32KB
#define TRANS_DATA_SIZE        (MAX_TRANS_BUF_SIZE - TRANS_DATA_OFFSET)

#define is_raw_type(type) (type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW)
#define EXTEND_BUF_SIZE (4096*2*2)
#define DEFAULT_BUFFER_TIME     ((200 * GST_MSECOND) / GST_USECOND)
#define DEFAULT_LATENCY_TIME    ((10 * GST_MSECOND) / GST_USECOND)

#define GST_AUDIO_FORMAT_TYPE_AC4 100
#define PTS_90K 90000

#ifdef DUMP_TO_FILE
static guint file_index;
#endif

struct _GstAmlHalAsinkPrivate
{
  audio_hw_device_t *hw_dev_;
  uint32_t output_port_;
  uint32_t direct_mode_;
  gboolean tts_mode_;
  /* current cap */
  GstAudioRingBufferSpec spec;

  /* patch for vol control */
  struct audio_port_config source_;
  struct audio_port_config sink_;
  audio_patch_handle_t patch_h_;
  gboolean mute_;

  /* condition lock for chain and other threads */
  GCond   run_ready;
  GMutex  feed_lock;

  /* output stream */
  struct audio_stream_out *stream_;
  audio_format_t format_;
  uint32_t sr_;
  audio_channel_mask_t channel_mask_;

  gboolean paused_;
  gboolean flushing_;

  gboolean sync_mode;
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
  GstClockTime eos_end_time;

  GstClockTime last_ts;
  guint64 render_samples;

  /* for position */
  guint wrapping_time;
  uint32_t last_pcr;
  uint32_t first_pts;
  guint64  first_pts_64;
  gboolean first_pts_set;

  GstSegment segment;
  /* curent stream group */
  guint group_id;
  gboolean group_done;

  /* tempo stretch */
  struct scale_tempo st;
  gboolean tempo_used;
  float rate;

  /* pause pts */
  uint32_t pause_pts;

  /* master volume */
  gboolean master_volume_pending;
  float master_volume;

  /* stream volume */
  gboolean stream_volume_pending;
  float stream_volume;

  /* underrun detection */
  GThread *xrun_thread;
  gboolean quit_xrun_thread;
  GTimer *xrun_timer;
  gboolean xrun_paused;
  gboolean disable_xrun;

#ifdef ESSOS_RM
  EssRMgr *rm;
  int resAssignedId;
#endif

  /* debugging */
  gboolean diag_log_enable;
  char *log_path;

  /* pts gap info, pts/duration in ms unit */
  int      gap_state;
  int64_t  gap_start_pts;
  int32_t  gap_duration;
  uint64_t gap_offset;

  /* avsync */
  void * avsync;
  int session_id;
  GstClock *provided_clock;
  gboolean wait_video;
  int aligned_timeout;
  GstBuffer *start_buf; /* pcr master mode only */
  gboolean start_buf_sent;
  gboolean seamless_switch;
  gboolean tempo_disable; /* disable tempo use */

#ifdef ENABLE_MS12
  /* ac4 config */
  int ac4_pres_group_idx;
  int ac4_pat;
  gchar *ac4_lang;
  gchar *ac4_lang2;
#endif
};

enum
{
  GAP_IDLE,
  GAP_MUTING_1,
  GAP_MUTING_2,
  GAP_RAMP_UP
};

enum
{
  RAMP_DOWN,
  RAMP_UP
};
enum
{
  PROP_0,
  PROP_DIRECT_MODE,
  PROP_TTS_MODE,
  PROP_OUTPUT_PORT,
  PROP_MASTER_VOLUME,
  PROP_STREAM_VOLUME,
  PROP_MUTE,
  PROP_AVSYNC_MODE,
  PROP_PAUSE_PTS,
  PROP_DISABLE_XRUN_TIMER,
  PROP_GAP_START_PTS,
  PROP_GAP_DURATION,
  PROP_AVSYNC_SESSION,
  PROP_WAIT_FOR_VIDEO,
  PROP_SEAMLESS_SWITCH,
  PROP_DISABLE_TEMPO_STRETCH,
#ifdef ENABLE_MS12
  /* AC4 config */
  PROP_AC4_P_GROUP_IDX,
  PROP_AC4_LANG_1,
  PROP_AC4_LANG_2,
  PROP_AC4_AUTO_S_PRI,
#endif
  PROP_A_WAIT_TIMEOUT,
  PROP_LAST
};

enum
{
  SIGNAL_PAUSEPTS,
  SIGNAL_XRUN,
  SIGNAL_AUDSWITCH,
  MAX_SIGNAL
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
#ifdef ENABLE_MS12
      "channels={2,3,4,5,6,7,8},layout=interleaved; "
#else
      "channels=2,layout=interleaved; "
#endif
      "audio/x-ac3, "
      COMMON_AUDIO_CAPS "; "
      "audio/x-eac3, "
      COMMON_AUDIO_CAPS "; "
      "audio/x-ac4; "
#ifdef ENABLE_MS12
      "audio/mpeg, mpegversion=4, stream-format=(string){loas}; "
#endif
#ifdef ENABLE_DTS
      "audio/x-dts; "
      "audio/x-gst-fourcc-dtse;"
      "audio/x-dtsl;"
#endif
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
#if GST_CHECK_VERSION(1,14,0)
G_DEFINE_TYPE_WITH_CODE (GstAmlHalAsink, gst_aml_hal_asink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_aml_hal_asink_debug_category, "amlhalasink", 0,
  "debug category for amlhalasink element");G_ADD_PRIVATE(GstAmlHalAsink));
#else
G_DEFINE_TYPE_WITH_CODE (GstAmlHalAsink, gst_aml_hal_asink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_aml_hal_asink_debug_category, "amlhalasink", 0,
  "debug category for amlhalasink element");
  G_IMPLEMENT_INTERFACE (GST_TYPE_STREAM_VOLUME, NULL)
  );
#endif

static guint g_signals[MAX_SIGNAL]= {0};

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
static inline void gst_aml_hal_asink_reset_sync (GstAmlHalAsink * sink, gboolean keep_position);
static GstClockTime gst_aml_hal_asink_get_time(GstClock * clock, GstAmlHalAsink * sink);

static GstFlowReturn gst_aml_hal_asink_render (GstAmlHalAsink * sink, GstBuffer * buffer);
static GstFlowReturn gst_aml_hal_asink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean gst_aml_hal_asink_event(GstAmlHalAsink *sink, GstEvent * event);
static gboolean gst_aml_hal_asink_pad_event (GstPad * pad, GstObject * parent, GstEvent * event);
static GstFlowReturn gst_aml_hal_asink_wait_event(GstBaseSink * bsink,
    GstEvent * event);
static void gst_aml_hal_asink_get_times(GstBaseSink * bsink,
    GstBuffer * buffer, GstClockTime * start, GstClockTime * end);
static gboolean gst_aml_hal_asink_setcaps (GstAmlHalAsink * sink, GstCaps * caps);

static gboolean hal_open_device (GstAmlHalAsink * sink);
static gboolean hal_close_device (GstAmlHalAsink* sink);
static gboolean hal_acquire (GstAmlHalAsink * sink, GstAudioRingBufferSpec * spec);
static gboolean hal_release (GstAmlHalAsink * sink);
static gboolean hal_start (GstAmlHalAsink * sink);
static gboolean hal_pause (GstAmlHalAsink * sink);
static gboolean hal_stop (GstAmlHalAsink * sink);
static guint hal_commit (GstAmlHalAsink * sink, guchar * data, gint size, guint64 pts_64);
static uint32_t hal_get_latency (GstAmlHalAsink * sink);
static void dump(const char* path, const uint8_t *data, int size);
#if 0
static int get_sysfs_uint32(const char *path, uint32_t *value);
static int config_sys_node(const char* path, const char* value);
#endif
static void check_pause_pts (GstAmlHalAsink *sink, GstClockTime ts);
static void vol_ramp(guchar * data, gint size, int dir);

static void
gst_aml_hal_asink_class_init (GstAmlHalAsinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

#if GST_CHECK_VERSION(1,14,0)
#else
  g_type_class_add_private (klass, sizeof (GstAmlHalAsinkPrivate));
#endif

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

  g_object_class_install_property (gobject_class, PROP_TTS_MODE,
      g_param_spec_boolean ("tts-mode", "TTS Mode",
          "Select this mode for text to speech, this mode doesnt have AV sync control",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_MASTER_VOLUME,
      g_param_spec_double ("volume", "Stream volume, the same as stream-volume",
          "Linear volume of current stream, 1.0=100%", 0.0, MAX_VOLUME,
          DEFAULT_VOLUME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_STREAM_VOLUME,
      g_param_spec_double ("stream-volume", "Stream volume",
          "Linear volume of curernt stream, 1.0=100%", 0.0, MAX_VOLUME,
          DEFAULT_VOLUME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_MUTE,
      g_param_spec_boolean ("mute", "Mute",
          "Mute state of system", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_AVSYNC_MODE,
      g_param_spec_uint ("avsync-mode", "Avsync mode",
          "Set avsync mode 0 (amaster) 1 (pcr master) 2 (iptv)",
          0, 2, 0, G_PARAM_READWRITE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_PAUSE_PTS,
      g_param_spec_uint ("pause-pts", "pause pts",
        "notify arrival of pts (90KHz), caller should pause the sink, set it in READY state",
        0, G_MAXUINT, 0, G_PARAM_WRITABLE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_GAP_START_PTS,
      g_param_spec_int64 ("gap-start-pts", "gap start pts",
        "notify arrival of pts discontinuity start point in ms",
        G_MININT64, G_MAXINT64, -1, G_PARAM_WRITABLE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_GAP_DURATION,
      g_param_spec_int ("gap-duration", "gap duration",
        "notify pts discontinuity length in ms",
        0, G_MAXINT, 0, G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class, PROP_DISABLE_XRUN_TIMER,
      g_param_spec_boolean ("disable-xrun", "Disable underrun timer",
          "If the audio stream is not stable like in Mircast case, set it to prevent unexpected pause",
          FALSE, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_AVSYNC_SESSION,
      g_param_spec_int ("avsync-session", "avsync session",
        "avsync session id to link video and audio. If set, this sink won't create clock",
        G_MININT, G_MAXINT, 0, G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class,
      PROP_WAIT_FOR_VIDEO,
      g_param_spec_boolean ("wait-video", "Wait for video to start",
          "Audio needs to align with video at starting point", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_SEAMLESS_SWITCH,
      g_param_spec_boolean ("seamless-switch", "Seamless switch audio",
          "Seamless switch audio channels on different formats", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_DISABLE_TEMPO_STRETCH,
      g_param_spec_boolean ("disable-tempo-stretch",
          "Disable tempo stretch", "Disable the tempo stretch process", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

#ifdef ENABLE_MS12
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_AC4_P_GROUP_IDX,
      g_param_spec_int ("ac4-presentation-group-index", "ac4 presentation group index",
        "Presentation group index to be decoded. Overrides the presentation selection by preferred language and associated type 0...%d: Presentation group index -1:  Switch back to automatic selection by language and associated type (default)",
        -1, G_MAXINT, -1, G_PARAM_WRITABLE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_AC4_AUTO_S_PRI,
      g_param_spec_string ("ac4-auto-selection-priority", "ac4 auto selection priority",
        "language: Prefer selection by language associated_type: Prefer selection by associated type (default)", "associated_type", G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class, PROP_AC4_LANG_1,
      g_param_spec_string  ("ac4-preferred-lang1", "ac4 preferred lang1",
        "1st Preferred Language code (3 Letter ISO 639)",
        "null", G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class, PROP_AC4_LANG_2,
      g_param_spec_string  ("ac4-preferred-lang2", "ac4 preferred lang2",
        "2nd Preferred Language code (3 Letter ISO 639)",
        "null", G_PARAM_WRITABLE));
#endif

  g_object_class_install_property (gobject_class,
      PROP_A_WAIT_TIMEOUT,
      g_param_spec_int ("a-wait-timeout", "audio timeout when video not come",
          "audio wait for video timeout if no video comes, effective when wait-video property is true.",
          -1, 10000, 0, G_PARAM_WRITABLE));

  g_signals[SIGNAL_PAUSEPTS]= g_signal_new( "pause-pts-callback",
      G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
      (GSignalFlags) (G_SIGNAL_RUN_LAST),
      0,    /* class offset */
      NULL, /* accumulator */
      NULL, /* accu data */
      g_cclosure_marshal_VOID__UINT_POINTER,
      G_TYPE_NONE,
      2,
      G_TYPE_UINT,
      G_TYPE_POINTER);

  g_signals[SIGNAL_XRUN]= g_signal_new( "underrun-callback",
      G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
      (GSignalFlags) (G_SIGNAL_RUN_LAST),
      0,    /* class offset */
      NULL, /* accumulator */
      NULL, /* accu data */
      g_cclosure_marshal_VOID__UINT_POINTER,
      G_TYPE_NONE,
      2,
      G_TYPE_UINT,
      G_TYPE_POINTER);

  g_signals[SIGNAL_AUDSWITCH]= g_signal_new( "seamless-switch-callback",
      G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
      (GSignalFlags) (G_SIGNAL_RUN_LAST),
      0,    /* class offset */
      NULL, /* accumulator */
      NULL, /* accu data */
      g_cclosure_marshal_VOID__UINT_POINTER,
      G_TYPE_NONE,
      2,
      G_TYPE_UINT,
      G_TYPE_POINTER);

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_change_state);
  gstelement_class->provide_clock =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_provide_clock);
  gstelement_class->query = GST_DEBUG_FUNCPTR (gst_aml_hal_asink_query);

  gstbasesink_class->wait_event =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_wait_event);
  gstbasesink_class->get_times =
      GST_DEBUG_FUNCPTR (gst_aml_hal_asink_get_times);

  gst_element_class_set_details_simple (gstelement_class, "AmlHalAsink",
      "Decoder/Sink/Audio",
      "Gstreamer sink plugin for audio HAL",
      "Amlogic");

  /* ref class from a thread-safe context to work around missing bit of
   * thread-safety in GObject */
  g_type_class_ref (GST_TYPE_AUDIO_CLOCK);
}

static void
gst_aml_hal_asink_init (GstAmlHalAsink* sink)
{
  GstBaseSink *basesink;
#if GST_CHECK_VERSION(1,14,0)
  GstAmlHalAsinkPrivate *priv = gst_aml_hal_asink_get_instance_private (sink);
#else
  GstAmlHalAsinkPrivate *priv = GST_AML_HAL_ASINK_GET_PRIVATE (sink);
#endif

  sink->priv = priv;
  basesink = GST_BASE_SINK_CAST (sink);
  /* bypass sync control of basesink */
  gst_base_sink_set_sync(basesink, FALSE);
  gst_pad_set_event_function (basesink->sinkpad, gst_aml_hal_asink_pad_event);
  gst_pad_set_chain_function (basesink->sinkpad, gst_aml_hal_asink_chain);

  priv->provided_clock = gst_aml_clock_new ("GstAmlSinkClock",
          (GstAmlClockGetTimeFunc) gst_aml_hal_asink_get_time, sink, NULL);
  GST_OBJECT_FLAG_SET (basesink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  priv->hw_dev_ = NULL;
  priv->direct_mode_ = TRUE;
  priv->received_eos = FALSE;
  priv->group_id = -1;
  priv->pause_pts = -1;
  priv->gap_state = GAP_IDLE;
  priv->gap_start_pts = -1;
  priv->gap_duration = 0;
  priv->gap_offset = 0;
  priv->format_ = AUDIO_FORMAT_PCM_16_BIT;
  priv->sync_mode = AV_SYNC_MODE_AMASTER;
  priv->session_id = -1;
  priv->stream_volume = 1.0;
#ifdef ENABLE_MS12
  priv->ac4_pat = -1;
  priv->ac4_pres_group_idx = -2;
  priv->ac4_lang = priv->ac4_lang2 = NULL;
#endif
  priv->aligned_timeout = -1;
  g_mutex_init (&priv->feed_lock);
  g_cond_init (&priv->run_ready);
  scaletempo_init (&priv->st);

  {
    char *path = getenv("AV_PROGRESSION");
    if (path) {
      priv->log_path = g_malloc(128);
      if (priv->log_path) {
        snprintf(priv->log_path, 128, "%s.gtoa", path);
        priv->diag_log_enable = TRUE;
        GST_WARNING ("enable AV Progression logging");
      }
    }
  }
#ifdef ESSOS_RM
  priv->rm = 0;
  priv->resAssignedId = -1;
#endif
  log_set_level(LOG_INFO);
}

static void
gst_aml_hal_asink_dispose (GObject * object)
{
  GstAmlHalAsink * sink = GST_AML_HAL_ASINK(object);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  GST_DEBUG_OBJECT (sink, "dispose");
  if (priv->provided_clock) {
    gst_object_unref (priv->provided_clock);
    priv->provided_clock = NULL;
  }

  g_mutex_clear (&priv->feed_lock);
  g_cond_clear (&priv->run_ready);
#ifdef ENABLE_MS12
  g_free (priv->ac4_lang);
  g_free (priv->ac4_lang2);
#endif
  g_free (priv->log_path);
  G_OBJECT_CLASS (parent_class)->dispose (object);
}


static GstClock *
gst_aml_hal_asink_provide_clock (GstElement * elem)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (elem);
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstClock *clock = NULL;

  GST_OBJECT_LOCK (sink);
  if (priv->provided_clock)
      clock = GST_CLOCK_CAST (gst_object_ref (priv->provided_clock));
  GST_OBJECT_UNLOCK (sink);

  return clock;
}

static gboolean
gst_aml_hal_asink_is_self_provided_clock (GstAmlHalAsink* sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  return (priv->provided_clock && GST_IS_AML_CLOCK (priv->provided_clock));
}

static int avsync_get_time(GstAmlHalAsink* sink, pts90K *pts)
{
    int rc = 0;
    GstAmlHalAsinkPrivate *priv = sink->priv;

    if (!priv->avsync) {
        rc = -1;
        goto exit;
    }

    rc = av_sync_get_clock(priv->avsync, pts);

exit:
    return rc;
}

static gboolean
get_position (GstAmlHalAsink* sink, GstFormat format, gint64 * cur)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  pts90K pcr = 0;
  gint64 timepassed, timepassed_90k;
  int rc;

  if (priv->group_done) {
    /* return a little bigger time for basesink of other module
     * to behavior correctly. basink assume clock keeps
     * going to exit wait loop */
    *cur = priv->eos_end_time + 100 * GST_MSECOND;
    return TRUE;
  }

  if (!priv->render_samples) {
    if (priv->segment.start != GST_CLOCK_TIME_NONE)
      *cur = priv->segment.start;
    else
      *cur = GST_CLOCK_TIME_NONE;
    return TRUE;
  }

  if (!priv->direct_mode_) {
    //TODO(song): get HAL position
    *cur = gst_util_uint64_scale_int(priv->render_samples, GST_SECOND, priv->sr_);
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

  rc = avsync_get_time(sink, &pcr);
  if (rc)
      return FALSE;

  if (pcr == -1) {
    if (priv->paused_) {
      pcr = priv->last_pcr;
      GST_LOG_OBJECT (sink, "paused, return last %u", pcr);
    } else {
      pcr = priv->first_pts;
      GST_LOG_OBJECT (sink, "render not start, set to first_pts %u", pcr);
    }
  } else if (priv->first_pts_set && (int)(priv->first_pts - pcr) > 0 &&
                priv->sync_mode == AV_SYNC_MODE_AMASTER) {
    pcr = priv->first_pts;
    GST_LOG_OBJECT (sink, "render start with delay, set to first_pts %u", pcr);
  }

  if (priv->sync_mode != AV_SYNC_MODE_PCR_MASTER) {
    /* for live streaming need to consider PTS wrapping */
    if (priv->last_pcr > 0xF0000000 && pcr < 10*PTS_90K) {
      priv->wrapping_time++;
      GST_INFO_OBJECT (sink, "pts wrapping num: %d", priv->wrapping_time);
    }
    priv->last_pcr = pcr;

    if (priv->wrapping_time <= 1)
      timepassed_90k = (int)(pcr - priv->first_pts);
    else
      timepassed_90k = (int)(pcr - priv->first_pts) +
              (priv->wrapping_time - 1) * 0xFFFFFFFFLL;

    timepassed = gst_util_uint64_scale_int (timepassed_90k, GST_SECOND, PTS_90K);
    *cur = priv->first_pts_64 + timepassed;
  } else {
    timepassed = gst_util_uint64_scale_int (pcr, GST_SECOND, PTS_90K);
    *cur = timepassed;
  }

  GST_LOG_OBJECT (sink, "POSITION: %" GST_TIME_FORMAT " pcr: %u",
                  GST_TIME_ARGS (*cur), pcr);
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
        check_pause_pts (sink, cur);
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

#if 0
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
#endif

static void check_pause_pts (GstAmlHalAsink *sink, GstClockTime ts)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  uint32_t pts_90k;

  if (priv->pause_pts == -1)
    return;

  pts_90k = gst_util_uint64_scale_int (ts, PTS_90K, GST_SECOND);
  if (pts_90k > priv->pause_pts) {
    GST_WARNING_OBJECT (sink, "emit pause pts signal %u", pts_90k);
    g_signal_emit (G_OBJECT (sink), g_signals[SIGNAL_PAUSEPTS], 0, pts_90k, NULL);
    priv->pause_pts = -1;
  }
}

/* we call this function without holding the lock on sink for performance
 * reasons. Try hard to not deal with and invalid ringbuffer and rate. */
static GstClockTime gst_aml_hal_asink_get_time (GstClock * clock, GstAmlHalAsink * sink)
{
  gint64 position = GST_CLOCK_TIME_NONE;

  get_position (sink, GST_FORMAT_TIME, &position);
  GST_LOG_OBJECT (sink, "time %" GST_TIME_FORMAT, GST_TIME_ARGS (position));
  return position;
}

static void
gst_aml_hal_sink_set_stream_volume (GstAmlHalAsink * sink, float vol, gboolean force)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int ret;

  if (!priv->stream_) {
    GST_WARNING_OBJECT(sink, "audio stream not open yet, delayed");
    priv->stream_volume_pending = TRUE;
    priv->stream_volume = vol;
    return;
  }

  if (!force && vol == priv->stream_volume)
    return;

  ret = priv->stream_->set_volume (priv->stream_, vol, vol);
  if (ret)
    GST_ERROR_OBJECT(sink, "set volume fail %d", ret);
  else {
    GST_LOG_OBJECT(sink, "stream volume set to %f", vol);
    priv->stream_volume = vol;
  }
}

static void
gst_aml_hal_sink_set_mute (GstAmlHalAsink * sink, gboolean mute)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int ret;

  GST_WARNING_OBJECT (sink, "set mute:%d", mute);
  ret = priv->hw_dev_->set_master_mute( priv->hw_dev_, mute);
  if (ret)
    GST_ERROR_OBJECT(sink, "mute fail:%d", ret);
  else
    priv->mute_ = mute;
}

static gboolean
gst_aml_hal_sink_get_mute (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  gboolean mute = 0;
  int ret;

  if (!priv->hw_dev_) {
    GST_ERROR_OBJECT(sink, "audio HAL not open yet");
    return mute;
  }

  ret = priv->hw_dev_->get_master_mute(priv->hw_dev_, &mute);
  if (ret) {
    GST_ERROR_OBJECT(sink, "get_master_mute fail: %d",ret);
    return mute;
  }
  GST_LOG_OBJECT (sink, "master mute:%d", mute);

  return mute;
}


static void
gst_aml_hal_asink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAmlHalAsink *sink = GST_AML_HAL_ASINK (object);
  GstAmlHalAsinkPrivate *priv = sink->priv;

  switch (property_id) {
    case PROP_DIRECT_MODE:
    {
      priv->direct_mode_ = g_value_get_boolean(value);
      GST_DEBUG_OBJECT (sink, "set direct mode:%d", priv->direct_mode_);
      GST_OBJECT_LOCK (sink);
      if (priv->tempo_used && !priv->direct_mode_) {
        GST_DEBUG_OBJECT (sink, "disable scaletempo for non-direct mode");
        scaletempo_stop (&priv->st);
        priv->tempo_used = FALSE;
      }
      if (!priv->direct_mode_ && priv->provided_clock) {
        GstAmlHalAsinkClass *class = GST_AML_HAL_ASINK_CLASS(object);
        GstBaseSink *basesink = GST_BASE_SINK_CAST (sink);
        GstElementClass *eclass = (GstElementClass *)class;

        gst_object_unref (priv->provided_clock);
        priv->provided_clock = NULL;
        eclass->provide_clock = NULL;
        GST_OBJECT_FLAG_UNSET (basesink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
      }
      GST_OBJECT_UNLOCK (sink);
      break;
    }
    case PROP_TTS_MODE:
      priv->tts_mode_ = g_value_get_boolean(value);
      if (priv->tts_mode_)
        priv->direct_mode_ = false;
      GST_WARNING_OBJECT (sink, "set tts mode:%d", priv->tts_mode_);
      break;
    case PROP_OUTPUT_PORT:
      priv->output_port_ = g_value_get_enum (value);
      GST_DEBUG_OBJECT (sink, "set output port:%d", priv->output_port_);
      break;
    case PROP_MASTER_VOLUME:
    case PROP_STREAM_VOLUME:
    {
      float vol = g_value_get_double (value);
      gst_aml_hal_sink_set_stream_volume (sink, vol, false);
      break;
    }
    case PROP_MUTE:
      gst_aml_hal_sink_set_mute (sink, g_value_get_boolean (value));
      break;
    case PROP_AVSYNC_MODE:
    {
      guint mode = g_value_get_uint (value);

      priv->sync_mode = g_value_get_uint (value);
      if (mode == 0) {
        priv->sync_mode = AV_SYNC_MODE_AMASTER;
      } else if (mode == 1) {
        priv->sync_mode = AV_SYNC_MODE_PCR_MASTER;
        priv->tempo_used = FALSE;
        priv->tempo_disable = TRUE;
      } else if (mode == 2) {
        priv->sync_mode = AV_SYNC_MODE_IPTV;
        priv->tempo_used = FALSE;
        priv->tempo_disable = TRUE;
      } else {
        GST_ERROR_OBJECT (sink, "invalid sync mode %d", mode);
      }

      GST_DEBUG_OBJECT (sink, "sync mode:%d", priv->sync_mode);
      break;
    }
    case PROP_PAUSE_PTS:
      priv->pause_pts = g_value_get_uint (value);
      GST_WARNING_OBJECT (sink, "pause PTS %u", priv->pause_pts);
      break;
    case PROP_DISABLE_XRUN_TIMER:
      priv->disable_xrun = g_value_get_boolean(value);
      GST_WARNING_OBJECT (sink, "disable xrun %d", priv->disable_xrun);
      break;
    case PROP_GAP_START_PTS:
      priv->gap_start_pts = g_value_get_int64(value);
      GST_WARNING_OBJECT (sink, "gap start PTS %" PRId64, priv->gap_start_pts);
      break;
    case PROP_GAP_DURATION:
      priv->gap_duration = g_value_get_int(value);
      GST_WARNING_OBJECT (sink, "gap duration %d", priv->gap_duration);
      break;
    case PROP_AVSYNC_SESSION:
    {
      int id = g_value_get_int (value);
      if (id >= 0) {
        GstAmlHalAsinkClass *class = GST_AML_HAL_ASINK_CLASS(object);
        GstElementClass *eclass = (GstElementClass *)class;
        GstBaseSink *basesink = GST_BASE_SINK_CAST (sink);

        /* will not provide GstAmlClock, application should take care of the
         * session */
        priv->session_id = id;
        eclass->provide_clock = NULL;

        gst_object_unref (priv->provided_clock);
        priv->provided_clock = NULL;
        GST_OBJECT_FLAG_UNSET (basesink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
        GST_WARNING_OBJECT (sink, "avsync session %u", priv->session_id);
      }
      break;
    }
    case PROP_WAIT_FOR_VIDEO:
      priv->wait_video = g_value_get_boolean(value);
      GST_WARNING_OBJECT (sink, "wait video:%d", priv->wait_video);
      break;
    case PROP_SEAMLESS_SWITCH:
      priv->seamless_switch = g_value_get_boolean(value);
      GST_WARNING_OBJECT (sink, "seamless switch audio:%d", priv->seamless_switch);
      if (priv->avsync) {
           GST_INFO_OBJECT (sink, "AV sync set seamless switch:%d",
                   priv->seamless_switch);
           av_sync_set_audio_switch(priv->avsync, priv->seamless_switch);
      }
      break;
    case PROP_DISABLE_TEMPO_STRETCH:
      priv->tempo_disable = g_value_get_boolean(value);
      GST_WARNING_OBJECT (sink, "disable tempo stretch:%d", priv->tempo_disable);
      if (priv->tempo_used && priv->tempo_disable) {
        GST_OBJECT_LOCK (sink);
        GST_DEBUG_OBJECT (sink, "disable scaletempo");
        scaletempo_stop (&priv->st);
        priv->tempo_used = FALSE;
        GST_OBJECT_UNLOCK (sink);
      }
      break;
#ifdef ENABLE_MS12
    case PROP_AC4_P_GROUP_IDX:
      priv->ac4_pres_group_idx = g_value_get_int(value);
      GST_WARNING_OBJECT (sink, "ac4_pres_group_idx:%d", priv->ac4_pres_group_idx);
      break;
    case PROP_AC4_AUTO_S_PRI:
    {
      const gchar *pri = g_value_get_string (value);
      if (!strncmp ("language", pri, sizeof ("language")))
        priv->ac4_pat = 0;
      else if (!strncmp ("associated_type", pri, sizeof ("associated_type")))
        priv->ac4_pat = 1;
      else
        GST_ERROR_OBJECT (sink, "invalid value:%s", pri);
      if (priv->ac4_pat != -1)
        GST_WARNING_OBJECT (sink, "ac4_pat:%d", priv->ac4_pat);
      break;
    }
    case PROP_AC4_LANG_1:
      if (priv->ac4_lang)
        g_free(priv->ac4_lang);
      priv->ac4_lang = g_value_dup_string (value);
      if (!priv->ac4_lang) {
        GST_ERROR_OBJECT (sink, "can not get string");
        break;
      }
      if (strlen(priv->ac4_lang) != 3) {
        GST_ERROR_OBJECT (sink, "wrong ac4_lang:%s", priv->ac4_lang);
        g_free(priv->ac4_lang);
        priv->ac4_lang = NULL;
        break;
      }
      GST_WARNING_OBJECT (sink, "ac4_lang:%s", priv->ac4_lang);
      break;
    case PROP_AC4_LANG_2:
      if (priv->ac4_lang2)
        g_free(priv->ac4_lang2);
      priv->ac4_lang2 = g_value_dup_string (value);
      if (!priv->ac4_lang2) {
        GST_ERROR_OBJECT (sink, "can not get string");
        break;
      }
      if (strlen(priv->ac4_lang2) != 3) {
        GST_ERROR_OBJECT (sink, "wrong ac4_lang2:%s", priv->ac4_lang2);
        g_free(priv->ac4_lang2);
        priv->ac4_lang2 = NULL;
        break;
      }
      GST_WARNING_OBJECT (sink, "ac4_lang2:%s", priv->ac4_lang2);
      break;
#endif
    case PROP_A_WAIT_TIMEOUT:
      priv->aligned_timeout = g_value_get_int(value);
      GST_WARNING_OBJECT (sink, "timeout:%d", priv->aligned_timeout);
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
    case PROP_TTS_MODE:
      g_value_set_boolean(value, priv->tts_mode_);
      GST_DEBUG_OBJECT (sink, "get tts mode:%d", priv->tts_mode_);
      break;
    case PROP_OUTPUT_PORT:
      g_value_set_enum(value, priv->output_port_);
      GST_DEBUG_OBJECT (sink, "get output port:%d", priv->output_port_);
      break;
    case PROP_MASTER_VOLUME:
    case PROP_STREAM_VOLUME:
      g_value_set_double (value, priv->stream_volume); 
      break;
    case PROP_MUTE:
      priv->mute_ = gst_aml_hal_sink_get_mute(sink);
      g_value_set_boolean (value, priv->mute_);
      break;
    case PROP_AVSYNC_MODE:
      g_value_set_uint (value, priv->sync_mode);
      break;
    case PROP_PAUSE_PTS:
      g_value_set_uint (value, priv->pause_pts);
      break;
    case PROP_AVSYNC_SESSION:
      g_value_set_int (value, priv->session_id);
      break;
    case PROP_WAIT_FOR_VIDEO:
      g_value_set_boolean (value, priv->wait_video);
      break;
    case PROP_SEAMLESS_SWITCH:
      g_value_set_boolean (value, priv->seamless_switch);
      break;
    case PROP_A_WAIT_TIMEOUT:
      g_value_set_int (value, priv->aligned_timeout);
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
    info.bpf = 1;
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
    info.bpf = 1;
  } else if (g_str_equal (mimetype, "audio/x-dts")) {
    /* extract the needed information from the cap */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
      goto parse_error;

    gst_structure_get_int (structure, "channels", &info.channels);
    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_DTS;
    info.bpf = 4;
  } else if (g_str_equal (mimetype, "audio/mpeg")) {
    /* if cannot extract the needed information from the cap, set default value */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
        info.rate = 48000;
    if (!gst_structure_get_int (structure, "channels", &info.channels))
        info.channels = 2;
    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_MPEG4_AAC;
    info.bpf = 1;
  } else if (g_str_equal (mimetype, "audio/x-dts")
    || g_str_equal (mimetype, "audio/x-gst-fourcc-dtse")
    || g_str_equal (mimetype, "audio/x-dtsl")) {
    /* if cannot extract the needed information from the cap, set default value */
    if (!(gst_structure_get_int (structure, "rate", &info.rate)))
        info.rate = 48000;
    if (!gst_structure_get_int (structure, "channels", &info.channels))
        info.channels = 2;
    spec->type = GST_AUDIO_RING_BUFFER_FORMAT_TYPE_DTS;
    info.bpf = 1;
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

static gboolean caps_not_changed(GstAmlHalAsink *sink, GstCaps * cur, GstCaps * new)
{
  gboolean ret = FALSE;
  GstAudioRingBufferSpec new_spec;
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstAudioRingBufferSpec *cur_spec = &priv->spec;

  if (gst_caps_is_equal (cur, new))
    return TRUE;

  memset(&new_spec, 0, sizeof(new_spec));
  if (!parse_caps (&new_spec, new))
    return FALSE;

  /* audio hal can handle the change for eac3 */
  if (cur_spec->type == new_spec.type &&
          cur_spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3) {
    GST_DEBUG_OBJECT (sink, "ignore eac3 format change");
    ret = TRUE;
  }

  gst_caps_replace (&new_spec.caps, NULL);
  return ret;
}

static gboolean gst_aml_hal_asink_setcaps (GstAmlHalAsink* sink, GstCaps * caps)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GstAudioRingBufferSpec *spec;

  spec = &priv->spec;

  if (G_UNLIKELY (spec->caps && caps_not_changed (sink, spec->caps, caps))) {
    GST_DEBUG_OBJECT (sink,
        "caps haven't changed, skipping reconfiguration");
    return TRUE;
  }

  GST_DEBUG_OBJECT (sink, "release old hal");

  /* release old ringbuffer */
  GST_OBJECT_LOCK (sink);
  hal_pause (sink);
  hal_release (sink);
  priv->flushing_ = FALSE;
  GST_OBJECT_UNLOCK (sink);

  GST_DEBUG_OBJECT (sink, "parse caps: %" GST_PTR_FORMAT, caps);

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

  if (priv->sync_mode == AV_SYNC_MODE_PCR_MASTER) {
    char setting[20];
    priv->avsync = av_sync_create (priv->session_id, priv->sync_mode, AV_SYNC_TYPE_AUDIO, 0);
    if (!priv->avsync) {
      GST_ERROR_OBJECT (sink, "create av sync fail");
      return FALSE;
    }
    /* set session into hwsync id */
    snprintf(setting, sizeof(setting), "hw_av_sync=%d", priv->session_id);
    priv->stream_->common.set_parameters (&priv->stream_->common, setting);
  }

  if (priv->sync_mode == AV_SYNC_MODE_PCR_MASTER) {
    char setting[20];
    priv->avsync = av_sync_create (priv->session_id, priv->sync_mode, AV_SYNC_TYPE_AUDIO, 0);
    if (!priv->avsync) {
      GST_ERROR_OBJECT (sink, "create av sync fail");
      return FALSE;
    }
    /* set session into hwsync id */
    snprintf(setting, sizeof(setting), "hw_av_sync=%d", priv->session_id);
    priv->stream_->common.set_parameters (&priv->stream_->common, setting);
  }

  if (priv->stream_volume_pending) {
    priv->stream_volume_pending = FALSE;
    gst_aml_hal_sink_set_stream_volume (sink, priv->stream_volume, true);
  }

  if (is_raw_type(spec->type) && priv->direct_mode_ && !priv->tempo_disable) {
    priv->tempo_used = TRUE;
    scaletempo_start (&priv->st);
    scaletempo_set_info (&priv->st, &spec->info);
  } else if (priv->tempo_disable)
    priv->tempo_used = FALSE;

  gst_element_post_message (GST_ELEMENT_CAST (sink),
      gst_message_new_latency (GST_OBJECT (sink)));

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

static inline void gst_aml_hal_asink_reset_sync (GstAmlHalAsink * sink, gboolean keep_position)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  priv->eos_time = -1;
  priv->received_eos = FALSE;
  priv->eos = FALSE;
  priv->last_ts = GST_CLOCK_TIME_NONE;
  priv->flushing_ = FALSE;
  priv->first_pts_set = FALSE;
  priv->wrapping_time = 0;
  priv->last_pcr = 0;
  gst_caps_replace (&priv->spec.caps, NULL);
  priv->segment.rate = 1.0f;
  priv->gap_state = GAP_IDLE;
  priv->gap_start_pts = -1;
  priv->gap_duration = 0;
  priv->gap_offset = 0;
  priv->quit_clock_wait = FALSE;
  priv->group_done = FALSE;
  if (priv->start_buf) {
    gst_buffer_unref (priv->start_buf);
    priv->start_buf = NULL;
  }
  priv->start_buf_sent = FALSE;

  if (!keep_position) {
    priv->render_samples = 0;
    priv->segment.start = GST_CLOCK_TIME_NONE;
  }
}

static void gst_aml_hal_asink_get_times (GstBaseSink * bsink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  /* our clock sync is a bit too much for the base class to handle so
   * we implement it ourselves. */
  *start = GST_CLOCK_TIME_NONE;
  *end = GST_CLOCK_TIME_NONE;
}

static GstClockReturn sink_wait_clock (GstAmlHalAsink * sink,
    GstClockTime time, GstClockTime duration)
{
  GstClockReturn ret;
  GstClock *clock;
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (time)))
    goto invalid_time;

  clock = priv->provided_clock;

  GST_INFO_OBJECT (sink,
      "time %" GST_TIME_FORMAT , GST_TIME_ARGS (time));

  GstClockTime now = gst_aml_hal_asink_get_time (clock, sink);
  if (now == GST_CLOCK_TIME_NONE) {
    ret = GST_CLOCK_UNSCHEDULED;
    goto exit;
  }

  if (!priv->render_samples &&
      priv->sync_mode == AV_SYNC_MODE_AMASTER) {
    /* clock not started yet */
    usleep (GST_TIME_AS_USECONDS(duration));
    ret = GST_CLOCK_OK;
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
    if (priv->eos) {
      GST_INFO_OBJECT (sink, "Already reach EOS no wait at time %lld now %lld",
                       time, now);
      ret = GST_CLOCK_OK;
      break;
    }
    if (now < time - GST_MSECOND) {
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

  if (priv->segment.stop != -1 &&
      priv->eos_time != -1 &&
      priv->segment.stop - priv->eos_time > 2 * GST_SECOND) {
    GST_DEBUG_OBJECT (sink, "refine eos from %llu to %llu",
        priv->eos_time, priv->segment.stop);
    priv->eos_time = priv->segment.stop;
  }

  if (priv->eos_time != -1 && !priv->group_done) {
    GstClockReturn cret;
    GST_DEBUG_OBJECT (sink,
        "last sample time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (priv->eos_time));

    hal_commit (sink, NULL, 0, -1);
    /* wait for the EOS time to be reached, this is the time when the last
     * sample is played. */
    cret = sink_wait_clock (sink, priv->eos_time, 0);

    GST_INFO_OBJECT (sink, "drained ret: %d", cret);
    if (cret == GST_CLOCK_OK || cret == GST_CLOCK_EARLY)
      ret = GST_FLOW_OK;
    else
      ret = GST_FLOW_ERROR;

    if (priv->seamless_switch && priv->avsync) {
        GST_INFO_OBJECT (sink, "EOS reached clean audio seamless switch");
        av_sync_set_audio_switch(priv->avsync, false);
        priv->seamless_switch = false;
    }
  }
  return ret;
}

static GstFlowReturn
gst_aml_hal_asink_wait_event (GstBaseSink * bsink, GstEvent * event)
{
  //GstAmlHalAsink *sink = GST_AML_HAL_ASINK (bsink);
  //GstAmlHalAsinkPrivate *priv = sink->priv;
  GstFlowReturn ret = GST_FLOW_OK;

  switch (GST_EVENT_TYPE (event)) {
#if 0
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
#endif
    default:
      break;
  }

  return ret;
}

static int update_avsync_speed(GstAmlHalAsink *sink, float rate)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  int rc = 0;

  if (rate == priv->rate)
    return 0;

  if (priv->avsync)
    rc = av_sync_set_speed(priv->avsync, rate);

  if (!rc)
    priv->rate = rate;
  else
    GST_ERROR_OBJECT (sink, "rate %f fail", rate);
  return rc;
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
      GST_DEBUG_OBJECT (sink, "receive eos");
      priv->received_eos = TRUE;
      ret = sink_drain (sink);
      if (G_UNLIKELY (ret != GST_FLOW_OK)) {
        result = FALSE;
        goto done;
      }
      GST_OBJECT_LOCK (sink);
      priv->eos = TRUE;
      if (priv->xrun_timer) {
        g_timer_stop (priv->xrun_timer);
        priv->xrun_paused = false;
      }
      GST_OBJECT_UNLOCK (sink);

      /* ok, now we can post the message */
      GST_WARNING_OBJECT (sink, "Now posting EOS");
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
      priv->quit_clock_wait = TRUE;
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
      if (priv->xrun_timer) {
        g_timer_stop(priv->xrun_timer);
        priv->xrun_paused = false;
      }
      GST_OBJECT_UNLOCK (sink);

      gst_aml_hal_asink_reset_sync (sink, TRUE);
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
      GstSegment segment;
      gst_event_copy_segment (event, &segment);
      GST_DEBUG_OBJECT (sink, "configured segment %" GST_SEGMENT_FORMAT,
              &segment);

      if (segment.start != GST_CLOCK_TIME_NONE) {
        GstMessage *seg_rec;

        if (priv->segment.rate != 0.0 && priv->segment.rate != segment.rate) {
          GST_INFO_OBJECT (sink, "ignore rate %f keep %f", segment.rate, priv->segment.rate);
          segment.rate = priv->segment.rate;
        }
        priv->segment = segment;
        priv->render_samples = 0;
        seg_rec = gst_message_new_info_with_details (GST_OBJECT_CAST (sink),
            NULL, "segment-received", gst_structure_new_empty("segment-received"));
        if (seg_rec)
          gst_element_post_message (GST_ELEMENT_CAST (sink), seg_rec);
      } else {
        GST_WARNING_OBJECT (sink, "rate update %f -> %f", priv->segment.rate, segment.rate);
        if (priv->segment.rate == segment.rate) {
          GST_WARNING_OBJECT (sink, "keep current rate");
          break;
        }
        priv->segment.rate = segment.rate;
      }

      /* create avsync before rate change */
      if (!priv->avsync && priv->direct_mode_) {
        char setting[20];
        struct start_policy policy;

         if (priv->seamless_switch) {
           priv->avsync = av_sync_attach (priv->session_id, AV_SYNC_TYPE_AUDIO);
         } else {
           priv->avsync = av_sync_create (priv->session_id, priv->sync_mode, AV_SYNC_TYPE_AUDIO, 0);
         }
        if (!priv->avsync) {
          GST_ERROR_OBJECT (sink, "create av sync fail");
          break;
        }
        if (priv->seamless_switch)
        {
           GST_INFO_OBJECT (sink, "SET AVSYNC audio switch to ALIGN mode");
           policy.policy = AV_SYNC_START_ALIGN;
           policy.timeout = -1;
           avs_sync_set_start_policy (priv->avsync, &policy);
        }

        if (priv->wait_video) {
          policy.policy = AV_SYNC_START_ALIGN;
          policy.timeout = priv->aligned_timeout;
          GST_INFO_OBJECT (sink, "set policy=align,  timeout=%d", policy.timeout);
          avs_sync_set_start_policy (priv->avsync, &policy);
        }
        /* set session into hwsync id */
        snprintf(setting, sizeof(setting), "hw_av_sync=%d", priv->session_id);
        priv->stream_->common.set_parameters (&priv->stream_->common, setting);
      }

      if (priv->tempo_used)
        scaletempo_update_segment (&priv->st, &priv->segment);

      if (priv->direct_mode_) {
        update_avsync_speed(sink, segment.rate);
        GST_INFO_OBJECT (sink, "rate to %f", segment.rate);
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
      priv->eos_end_time = GST_CLOCK_TIME_NONE;
      priv->gap_state = GAP_IDLE;
      priv->gap_start_pts = -1;
      priv->gap_duration = 0;
      priv->gap_offset = 0;
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

      /* if audio stop in the middle, do not touch clock and
       * let video finish. Event GAP will update eos_end_time
       */
      if (GST_CLOCK_TIME_IS_VALID(priv->eos_end_time)
              && (priv->eos_end_time - priv->eos_time) > GST_SECOND) {
        GST_DEBUG_OBJECT (sink, "group-done ignore");
        break;
      }

      GST_OBJECT_LOCK (sink);
      if (priv->xrun_timer) {
        g_timer_stop (priv->xrun_timer);
        priv->xrun_paused = false;
      }
      GST_OBJECT_UNLOCK (sink);

      sink_drain (sink);
      GST_OBJECT_LOCK (sink);
      hal_stop (sink);
      priv->group_done = TRUE;
      priv->eos_end_time = priv->eos_time;
      GST_OBJECT_UNLOCK (sink);
      gst_aml_hal_asink_reset_sync (sink, FALSE);
      break;
    }
    case GST_EVENT_GAP:
    {
      GstClockTime timestamp, duration, wait_end;
      gint64 end_time = GST_CLOCK_TIME_NONE;

      gst_event_parse_gap (event, &timestamp, &duration);
      wait_end = timestamp + duration;

      if (GST_CLOCK_TIME_IS_VALID(priv->segment.stop))
        end_time = priv->segment.stop;
      else if (GST_CLOCK_TIME_IS_VALID(priv->segment.duration))
        end_time = priv->segment.start + priv->segment.duration;

      if (!GST_CLOCK_TIME_IS_VALID(duration) ||
              !GST_CLOCK_TIME_IS_VALID(timestamp) ||
              !GST_CLOCK_TIME_IS_VALID(end_time) ||
              wait_end > end_time) {
        GST_DEBUG_OBJECT (sink, "event-gap ignore");
        break;
      }

      if (!priv->group_done) {
        GstClockReturn cret;

        cret = sink_wait_clock (sink, wait_end, duration);
        priv->eos_end_time = wait_end;
        GST_DEBUG_OBJECT (sink, "event-gap wait %d", cret);
      } else {
        if (GST_CLOCK_TIME_IS_VALID(priv->eos_end_time) &&
                wait_end < priv->eos_end_time) {
          GST_DEBUG_OBJECT (sink, "event-gap ignore %llu < %llu", wait_end, priv->eos_end_time);
        } else {
          GST_DEBUG_OBJECT (sink, "sleep %d", (gint)(duration / 1000));
          usleep(duration / 1000);
          priv->eos_end_time = wait_end;
        }
      }
      break;
    }
    case GST_EVENT_CAPS:
    {
      GstCaps *caps = NULL;

      gst_event_parse_caps (event, &caps);
      if (caps) {
        result = gst_aml_hal_asink_setcaps(sink, caps);
        GST_DEBUG_OBJECT (sink, "set caps ret %d", result);
      } else {
        result = FALSE;
      }
      break;
    }
    default:
    {
      GST_LOG_OBJECT (sink, "pass to basesink");
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

  if (GST_EVENT_TYPE (event) != GST_EVENT_TAG)
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
  if (GST_EVENT_TYPE (event) != GST_EVENT_TAG)
    GST_DEBUG_OBJECT (sink, "done");
  return result;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (sink, "we are flushing ignore %" GST_PTR_FORMAT, event);
    gst_event_unref (event);
    goto done;
  }

after_eos:
  {
    GST_DEBUG_OBJECT (sink, "Event received after EOS, dropping %" GST_PTR_FORMAT, event);
    gst_event_unref (event);
    goto done;
  }
}

static gpointer xrun_thread(gpointer para)
{
  GstAmlHalAsink *sink = (GstAmlHalAsink *)para;
  GstAmlHalAsinkPrivate *priv = sink->priv;

  GST_INFO_OBJECT (sink, "enter");
  while (!priv->quit_xrun_thread) {
    /* cobalt cert requires pause avsync to stop video rendering */
    if (priv->seamless_switch) {
        bool result;
        av_sync_get_audio_switch(priv->avsync,  &result);
        if (!result) {
           GST_INFO_OBJECT (sink, "audio seamless switch finish");
           priv->seamless_switch = false;
           // emit the event here if needed
           g_signal_emit (G_OBJECT (sink), g_signals[SIGNAL_AUDSWITCH], 0, 0, NULL);
        }
    }
    if (!priv->xrun_paused &&
           g_timer_elapsed(priv->xrun_timer, NULL) > 0.7) {
#ifdef ENABLE_MS12
      char *status = priv->hw_dev_->get_parameters (priv->hw_dev_,
              "main_input_underrun");
      int underrun = 0;

      if (status) {
        sscanf(status,"main_input_underrun=%d", &underrun);
        free (status);
      }

      if (!underrun) {
        usleep(10000);
        continue;
      }
      if (priv->received_eos) {
        GST_INFO_OBJECT (sink, "xrun timer reached EOS");
        GST_OBJECT_LOCK (sink);
        priv->eos = TRUE;
        GST_OBJECT_UNLOCK (sink);
      } else {
        GST_INFO_OBJECT (sink, "xrun timer triggered pause audio");
        hal_pause (sink);
        g_signal_emit (G_OBJECT (sink), g_signals[SIGNAL_XRUN], 0, 0, NULL);
      }
      g_timer_start(priv->xrun_timer);
      g_timer_stop(priv->xrun_timer);
#else
      GST_INFO_OBJECT (sink, "xrun timer triggered pause audio");
      hal_pause (sink);
      priv->xrun_paused = true;
      g_signal_emit (G_OBJECT (sink), g_signals[SIGNAL_XRUN], 0, 0, NULL);
#endif
    }
    usleep(50000);
  }
  GST_INFO_OBJECT (sink, "quit");
  return NULL;
}

static int start_xrun_thread (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  priv->xrun_timer = g_timer_new ();
  if (!priv->xrun_timer) {
    GST_ERROR_OBJECT (sink, "create timer fail");
    return -1;
  }

  priv->quit_xrun_thread = FALSE;
  priv->xrun_thread = g_thread_new ("axrun_render", xrun_thread, sink);
  if (!priv->xrun_thread) {
    GST_ERROR_OBJECT (sink, "create thread fail");
    g_timer_destroy (priv->xrun_timer);
    priv->xrun_timer = NULL;
    return -1;
  }
  return 0;
}

static void stop_xrun_thread (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (priv->xrun_thread) {
    priv->quit_xrun_thread = TRUE;
    g_thread_join (priv->xrun_thread);
    priv->xrun_thread = NULL;
    g_timer_destroy (priv->xrun_timer);
    priv->xrun_timer = NULL;
    priv->xrun_paused = false;
  }
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

  if ((!GST_CLOCK_TIME_IS_VALID (time) ||
      (priv->last_ts != GST_CLOCK_TIME_NONE && priv->last_ts == time)) &&
      is_raw_type(priv->spec.type)) {
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

  if (!priv->first_pts_set) {
    uint32_t pts_32 = gst_util_uint64_scale_int (time, PTS_90K, GST_SECOND);

    priv->first_pts_set = TRUE;
    priv->first_pts_64 = time;
    priv->first_pts = pts_32;
    GST_INFO_OBJECT(sink, "update first PTS %x", pts_32);
  }

  GST_OBJECT_LOCK (sink);
  if (priv->tempo_used) {
    GstBuffer *outbuffer = NULL;
    gsize insize, outsize;

    insize = gst_buffer_get_size (buf);
    scaletempo_transform_size (&priv->st, insize, &outsize);
    GST_LOG_OBJECT (sink, "in:%d out:%d", insize, outsize);

    outbuffer = gst_buffer_new_allocate (NULL, outsize, NULL);
    if (!outbuffer) {
      GST_ERROR_OBJECT (sink, "out buffer fail %d", outsize);
      ret = GST_FLOW_ERROR;
      GST_OBJECT_UNLOCK (sink);
      goto done;
    }
    gst_buffer_copy_into (outbuffer, buf, GST_BUFFER_COPY_METADATA, 0, -1);

    ret = scaletempo_transform (&priv->st, buf, outbuffer);
    gst_buffer_unref (buf);
    buf = outbuffer;

    if (ret != GST_FLOW_OK) {
      GST_OBJECT_UNLOCK (sink);
      GST_LOG_OBJECT (sink, "transform fail");
      goto done;
    }

    if (!gst_buffer_get_size(buf)) {
      /* lenth 0 can not be commited */
      priv->render_samples += samples;
      GST_OBJECT_UNLOCK (sink);
      GST_LOG_OBJECT (sink, "skip length 0 buff");
      goto done;
    }
    /* note: do not update time to output buffer
       time = GST_BUFFER_TIMESTAMP (buf);
     * it is based on the scaled time axis. When 2x
     * playback, the time will be 1/2 of real time.
     */
  }
  GST_OBJECT_UNLOCK (sink);
  //for those no bit stream parsed format EAC3 DTS render samples as buffers
  // otherwise position always return the start position.
  if (samples == 0)
    samples = 1;

  priv->render_samples += samples;
  gst_buffer_map (buf, &info, GST_MAP_READ);
  data = info.data;
  size = info.size;
  time = GST_BUFFER_TIMESTAMP (buf);

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

  if (priv->sync_mode == AV_SYNC_MODE_PCR_MASTER) {
      /* ms12 2.4 needs 2 frames to decode immediately */
      if(!priv->start_buf_sent && !priv->start_buf) {
        priv->start_buf = gst_buffer_ref (buf);
        GST_DEBUG_OBJECT (sink, "cache start buf %llu", time);
        goto commit_done;
      } else if (!priv->start_buf_sent) {
        GstMapInfo info2;

        gst_buffer_map (priv->start_buf, &info2, GST_MAP_READ);
        hal_commit (sink, info2.data, info2.size,
                GST_BUFFER_TIMESTAMP(priv->start_buf));
        gst_buffer_unmap (priv->start_buf, &info2);
        gst_buffer_unref (priv->start_buf);
        priv->start_buf = NULL;
        priv->start_buf_sent = TRUE;
        GST_DEBUG_OBJECT (sink, "sent cache start buf");
      }
  }

  if (priv->format_ == AUDIO_FORMAT_PCM_16_BIT) {
      if ((priv->gap_state == GAP_IDLE) &&
          (priv->gap_start_pts != -1) &&
          (time >= (priv->gap_start_pts * GST_MSECOND))) {
        // PCM volume ramping down
        GST_DEBUG_OBJECT(sink, "PCM volume ramping down %" PRId64 "ms @%" PRId64 " size %d",
          priv->gap_start_pts, time, size);
        vol_ramp(data, size, RAMP_DOWN);
        hal_commit (sink, data, size, time);

        // insert silence
        if (priv->gap_duration > 0) {
          GST_DEBUG_OBJECT(sink, "PCM insert silence %d ms", priv->gap_duration);
          int32_t filled_ms = 0;
          int32_t bytes_per_ms = 48 * 4;
          uint8_t *silence = (uint8_t *)g_malloc(16 * bytes_per_ms);
          if (silence) {
            memset(silence, 0, 16 * bytes_per_ms);
            time += (int64_t)size * GST_MSECOND / bytes_per_ms;
            while ((filled_ms < priv->gap_duration) &&
              (!priv->flushing_)) {
              int insert_ms = priv->gap_duration - filled_ms;
              if (insert_ms > 16) insert_ms = 16;
              GST_DEBUG_OBJECT(sink, "PCM silence @%" PRId64, time + filled_ms * GST_MSECOND);
              hal_commit (sink, silence, insert_ms * bytes_per_ms, time + filled_ms * GST_MSECOND);
              filled_ms += insert_ms;
              priv->render_samples += filled_ms * 48;
            }
            g_free(silence);
          }
          priv->gap_duration = 0;
        }
        priv->gap_start_pts = -1;
        priv->gap_state = GAP_MUTING_1;
      } else if (priv->gap_state == GAP_MUTING_1) {
        GST_DEBUG_OBJECT(sink, "Muting 1 @%" PRId64 " size %d", time, size);
        memset(data, 0, size);
        hal_commit (sink, data, size, time);
        priv->gap_state = GAP_MUTING_2;
      } else if (priv->gap_state == GAP_MUTING_2) {
        GST_DEBUG_OBJECT(sink, "Muting 2 @%" PRId64 " size %d", time, size);
        memset(data, 0, size);
        hal_commit (sink, data, size, time);
        priv->gap_state = GAP_RAMP_UP;
      } else if (priv->gap_state == GAP_RAMP_UP) {
        // PCM volume ramping up
        GST_DEBUG_OBJECT(sink, "PCM volume ramping up @%" PRId64 " size %d", time, size);
        vol_ramp(data, size, RAMP_UP);
        hal_commit (sink, data, size, time);
        priv->gap_state = GAP_IDLE;
      } else {
        hal_commit (sink, data, size, time);
      }
  } else if (priv->format_ == AUDIO_FORMAT_E_AC3) {
      if ((priv->gap_start_pts != -1) &&
          (time >= (priv->gap_start_pts * GST_MSECOND))) {
        char cmd[32] = {0};
        snprintf(cmd, sizeof(cmd), "pts_gap=%llu,%d",
          (unsigned long long)priv->gap_offset, priv->gap_duration);
        priv->hw_dev_->set_parameters(priv->hw_dev_, cmd);
        GST_DEBUG_OBJECT(sink, "E-AC3 %s", cmd);
        priv->gap_start_pts = -1;
        priv->gap_duration = 0;
      }
      hal_commit (sink, data, size, time);
      priv->gap_offset += size;
  } else {
    hal_commit (sink, data, size, time);
  }

commit_done:
  g_mutex_unlock(&priv->feed_lock);
  gst_buffer_unmap (buf, &info);

#ifdef ENABLE_XRUN_DETECTION
  GST_OBJECT_LOCK (sink);
  if (priv->sync_mode == AV_SYNC_MODE_AMASTER &&
        priv->stream_ && !priv->xrun_thread &&
        !priv->disable_xrun && start_xrun_thread (sink)) {
    ret = GST_FLOW_ERROR;
    GST_OBJECT_UNLOCK (sink);
    goto done;
  }
  GST_OBJECT_UNLOCK (sink);
#endif

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
#ifdef DUMP_TO_FILE
    gst_buffer_map (buf, &info, GST_MAP_READ);
    data = info.data;
    size = info.size;
    dump ("/tmp/asink_input_", data, size);
    gst_buffer_unmap (buf, &info);
#endif
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

static void paused_to_ready(GstAmlHalAsink *sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  /* make sure we unblock before calling the parent state change
   * so it can grab the STREAM_LOCK */
  GST_OBJECT_LOCK (sink);
  hal_release (sink);
  priv->quit_clock_wait = TRUE;

  gst_aml_hal_asink_reset_sync (sink, FALSE);

  if (priv->tempo_used) {
    scaletempo_stop (&priv->st);
    priv->tempo_used = FALSE;
  }
  GST_OBJECT_UNLOCK (sink);
}

#ifdef ESSOS_RM
static void resMgrNotify(EssRMgr *rm, int event, int type, int id, void* userData)
{
  GstAmlHalAsink *sink = (GstAmlHalAsink*)userData;
  GstAmlHalAsinkPrivate *priv = sink->priv;

  GST_WARNING_OBJECT (sink, "resMgrNotify: enter");
  switch (type) {
    case EssRMgrResType_audioDecoder:
    {
      switch (event) {
        case EssRMgrEvent_revoked:
        {
          GST_WARNING_OBJECT (sink, "releasing audio decoder %d", id);
          paused_to_ready (sink);

          GST_OBJECT_LOCK (sink);
          EssRMgrReleaseResource (priv->rm, EssRMgrResType_audioDecoder, id);
          priv->resAssignedId = -1;
          GST_OBJECT_UNLOCK (sink);

          GST_DEBUG_OBJECT (sink, "done releasing audio decoder %d", id);
          break;
        }
        default:
        break;
      }
    }
    case EssRMgrEvent_granted:
    default:
      break;
  }
  GST_WARNING_OBJECT (sink, "resMgrNotify: exit");
}

static void essos_rm_init(GstAmlHalAsink *sink)
{
  bool result;
  EssRMgrRequest resReq;
  GstAmlHalAsinkPrivate *priv = sink->priv;
  const char *env = getenv("AMLASINK_USE_ESSRMGR");

  if (env && !atoi(env))
    return;

  priv->rm = EssRMgrCreate();
  if (!priv->rm) {
    GST_ERROR_OBJECT (sink, "fail");
    return;
  }

  resReq.type = EssRMgrResType_audioDecoder;
  resReq.usage = EssRMgrAudUse_none;
  resReq.priority = 0;
  resReq.asyncEnable = false;
  resReq.notifyCB = resMgrNotify;
  resReq.notifyUserData = sink;
  resReq.assignedId = -1;

  result = EssRMgrRequestResource (priv->rm, EssRMgrResType_audioDecoder, &resReq);
  if (result) {
    if (resReq.assignedId >= 0) {
      GST_DEBUG_OBJECT (sink, "assigned id %d caps %X", resReq.assignedId, resReq.assignedCaps);
      priv->resAssignedId = resReq.assignedId;
    }
  }
}

static void resMgrUpdateState(GstAmlHalAsinkPrivate *priv, EssRMgrResState state)
{
  if (priv->rm && priv->resAssignedId >= 0)
    EssRMgrResourceSetState(priv->rm, EssRMgrResType_audioDecoder, priv->resAssignedId, state);
}
#endif

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
      GST_INFO_OBJECT(sink, "null to ready");
#ifdef ESSOS_RM
      if (priv->direct_mode_)
        essos_rm_init (sink);
#endif
      if (priv->provided_clock)
        priv->session_id = gst_aml_clock_get_session_id(priv->provided_clock);
      GST_WARNING_OBJECT(sink, "avsync session %d", priv->session_id);

      if (!gst_aml_hal_asink_open (sink)) {
        GST_ERROR_OBJECT(sink, "asink open failure");
        goto open_failed;
      }

      GST_OBJECT_LOCK (sink);
      if (!hal_open_device (sink)) {
        GST_OBJECT_UNLOCK (sink);
        goto open_failed;
      }
      GST_OBJECT_UNLOCK (sink);
      break;
    }
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_INFO_OBJECT(sink, "ready to paused");
      gst_base_sink_set_async_enabled (GST_BASE_SINK_CAST(sink), FALSE);
      gst_aml_hal_asink_reset_sync (sink, FALSE);
      /* start in paused state until PLAYING */
      priv->paused_ = TRUE;
      priv->quit_clock_wait = FALSE;

      /* Only post clock-provide messages if this is the clock that
       * we've created. If the subclass has overriden it the subclass
       * should post this messages whenever necessary */
      if (gst_aml_hal_asink_is_self_provided_clock (sink))
        gst_element_post_message (element,
            gst_message_new_clock_provide (GST_OBJECT_CAST (element),
              priv->provided_clock, TRUE));
#ifdef ESSOS_RM
      resMgrUpdateState(priv, EssRMgrRes_paused);
#endif
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    {
      GST_INFO_OBJECT(sink, "paused to playing");

      GST_OBJECT_LOCK (sink);
      hal_start (sink);
      GST_OBJECT_UNLOCK (sink);
#ifdef ESSOS_RM
      resMgrUpdateState(priv, EssRMgrRes_active);
#endif
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
      GstBaseSink* bsink = GST_BASE_SINK_CAST (sink);
      GST_INFO_OBJECT(sink, "playing to paused");
      if (priv->sync_mode == AV_SYNC_MODE_PCR_MASTER) {
        GST_INFO_OBJECT(sink, "avsync to free run");
        av_sync_change_mode (priv->avsync, AV_SYNC_MODE_FREE_RUN);
      }
      GST_OBJECT_LOCK (sink);
      hal_pause (sink);
      /* To complete transition to paused state in async_enabled mode,
       * we need a preroll buffer pushed to the pad.
       * This is a workaround to avoid the need for preroll buffer. */
      if (priv->xrun_timer)
        g_timer_stop (priv->xrun_timer);
      GST_BASE_SINK_PREROLL_LOCK (bsink);
      bsink->have_preroll = 1;
      GST_BASE_SINK_PREROLL_UNLOCK (bsink);
      GST_OBJECT_UNLOCK (sink);
#ifdef ESSOS_RM
      resMgrUpdateState(priv, EssRMgrRes_paused);
#endif
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    {
      GST_INFO_OBJECT(sink, "paused to ready");
      paused_to_ready (sink);
#ifdef ESSOS_RM
      resMgrUpdateState(priv, EssRMgrRes_idle);
#endif
      break;
    }
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_INFO_OBJECT(sink, "ready to null");
      GST_OBJECT_LOCK (sink);
      hal_close_device (sink);

#ifdef ESSOS_RM
      if (priv->rm) {
         if (priv->resAssignedId >= 0) {
            EssRMgrReleaseResource (priv->rm,
                EssRMgrResType_audioDecoder, priv->resAssignedId);
            priv->resAssignedId = -1;
         }
         EssRMgrDestroy (priv->rm);
         priv->rm = 0;
      }
#endif
      gst_aml_hal_asink_close (sink);

      priv->gap_state = GAP_IDLE;
      priv->gap_start_pts = -1;
      priv->gap_duration = 0;
      priv->format_ = AUDIO_FORMAT_PCM_16_BIT;
      scaletempo_init (&priv->st);

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
  priv->hw_dev_ = NULL;
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

  if (priv->stream_) {
    priv->stream_->flush(priv->stream_);
    priv->hw_dev_->close_output_stream(priv->hw_dev_,
        priv->stream_);
  }

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
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_MPEG4_AAC:
      priv->format_ = AUDIO_FORMAT_HE_AAC_V2;
      break;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_DTS:
      priv->format_ = AUDIO_FORMAT_DTS;
      break;
    default:
      goto error;

  }
  priv->sr_ = GST_AUDIO_INFO_RATE (&spec->info);
  channels = GST_AUDIO_INFO_CHANNELS (&spec->info);

  if (!priv->direct_mode_ && channels != 2 && channels != 1) {
    GST_ERROR_OBJECT (sink, "unsupported channel number:%d", channels);
    goto error;
  }

  if (channels == 1)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_MONO;
  else if (channels == 2)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_STEREO;
  else if (channels == 3)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_2POINT1;
  else if (channels == 4)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_2POINT0POINT2;
  else if (channels == 5 || channels == 6)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_5POINT1;
  else if (channels == 7)
    priv->channel_mask_ = AUDIO_CHANNEL_OUT_6POINT1;
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

  if (priv->tts_mode_)
    flag = AUDIO_OUTPUT_FLAG_MMAP_NOIRQ | AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_PRIMARY;
  else if (priv->direct_mode_)
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

#ifdef ENABLE_MS12
  if (priv->format_ == AUDIO_FORMAT_AC4)
  {
    char setting[50];
    /*ac4 setting */
    if (priv->ac4_lang) {
      snprintf(setting, sizeof(setting), "ms12_runtime=-lang %s", priv->ac4_lang);
      priv->hw_dev_->set_parameters(priv->hw_dev_, setting);
    }
    if (priv->ac4_lang2) {
      snprintf(setting, sizeof(setting), "ms12_runtime=-lang2 %s", priv->ac4_lang2);
      priv->hw_dev_->set_parameters(priv->hw_dev_, setting);
    }
    if (priv->ac4_pat != -1) {
      snprintf(setting, sizeof(setting), "ms12_runtime=-pat %d", priv->ac4_pat);
      priv->hw_dev_->set_parameters(priv->hw_dev_, setting);
    }
    if (priv->ac4_pres_group_idx != -2) {
      snprintf(setting, sizeof(setting), "ms12_runtime=-ac4_pres_group_idx %d", priv->ac4_pres_group_idx);
      priv->hw_dev_->set_parameters(priv->hw_dev_, setting);
    }
  }
#endif

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

  stop_xrun_thread (sink);
  hal_stop(sink);
  g_mutex_lock(&priv->feed_lock);
  if (priv->stream_) {
    priv->hw_dev_->close_output_stream(priv->hw_dev_, priv->stream_);
    priv->stream_ = NULL;
    priv->paused_ = FALSE;
    priv->render_samples = 0;
  }
  g_mutex_unlock(&priv->feed_lock);

  GST_INFO_OBJECT(sink, "done");
  return TRUE;
}

#if 0
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

static int tsync_set_first_apts(uint32_t pts)
{
  char val[20];
  snprintf (val, sizeof(val), "%u", pts);
  return config_sys_node(TSYNC_APTS, val);
}

static int tsync_send_audio_event(const char* event)
{
  char *val;
  val = "AUDIO_PAUSE";
  return config_sys_node(TSYNC_EVENT, val);
}

static int tsync_reset_pcr (GstAmlHalAsink * sink)
{
    config_sys_node(TSYNC_PCRSCR, "0");
    return 0;
}

static int tsync_enable (GstAmlHalAsink * sink, gboolean enable)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;

  if (!priv->direct_mode_)
    return 0;

  if (enable && !priv->tsync_enable_) {
    config_sys_node(TSYNC_ENABLE, "1");
    priv->tsync_enable_ = TRUE;
    GST_DEBUG_OBJECT (sink, "tsync enable");
    if (!priv->pcr_master_)
      config_sys_node(TSYNC_MODE, "1");//audio master
    else
      config_sys_node(TSYNC_MODE, "2");//pcr master
    return 0;
  }

  if (!enable && priv->tsync_enable_) {
    config_sys_node(TSYNC_ENABLE, "0");
    priv->tsync_enable_ = FALSE;
    GST_DEBUG_OBJECT (sink, "tsync disable");
  }
  return 0;
}
#endif

#define HAL_WRONG_STAT 3
static gboolean hal_start (GstAmlHalAsink * sink)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  GST_DEBUG_OBJECT (sink, "enter");

  if (!priv->stream_) {
    GST_INFO_OBJECT (sink, "stream not created yet");
    priv->paused_ = FALSE;
  } else {
    g_mutex_lock(&priv->feed_lock);
    if (priv->paused_) {
      int ret;

      ret = priv->stream_->resume(priv->stream_);
      if (ret)
        GST_WARNING_OBJECT (sink, "resume failure:%d", ret);

      GST_DEBUG_OBJECT (sink, "resume");

      if (priv->xrun_timer)
        g_timer_start(priv->xrun_timer);
      priv->paused_ = FALSE;
      g_cond_signal (&priv->run_ready);
    }
    g_mutex_unlock(&priv->feed_lock);
  }

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

  ret = priv->stream_->flush(priv->stream_);
  if (ret) {
    GST_ERROR_OBJECT (sink, "pause failure:%d", ret);
    return FALSE;
  }

  g_mutex_lock (&priv->feed_lock);
  priv->flushing_ = TRUE;
  g_cond_signal (&priv->run_ready);
  g_mutex_unlock (&priv->feed_lock);
  GST_DEBUG_OBJECT (sink, "stop");

  if (priv->avsync) {
    /* if session is still alive, recover mode for next playback */
    if (priv->sync_mode == AV_SYNC_MODE_PCR_MASTER) {
      GST_INFO_OBJECT(sink, "recover avsync mode");
      av_sync_change_mode (priv->avsync, AV_SYNC_MODE_PCR_MASTER);
    }
    av_sync_destroy (priv->avsync);
    priv->avsync = NULL;
  }

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
    GST_DEBUG_OBJECT (sink, "sr:%d spf:%d sync:%d",
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

static void diag_print(GstAmlHalAsink * sink, uint32_t pts_90k)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  struct timespec ts;
  FILE *fd;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  fd = fopen(priv->log_path, "a+");
  if (fd) {
    fprintf(fd, "[%6lu.%06lu] %u 0 A GtoA\n", ts.tv_sec, ts.tv_nsec/1000, pts_90k);
    fclose(fd);
  }
}

/* audio gap is for Netflix, 2ch AAC is the only use case */
static void vol_ramp(guchar * data, gint size, int dir)
{
  int i;
  int frames = size / 4;
  int16_t *l = (int16_t *)(data);
  int16_t *r = l + 1;

  if (dir == RAMP_DOWN) {
    for (i = 0; i < frames; i++, l+=2, r+=2) {
      float t = (float)(i) / frames;
      *l *= 1.0f - t * t * t;
      *r *= 1.0f - t * t * t;
    }
  } else {
     for (i = 0; i < frames; i++, l+=2, r+=2) {
      float t = (float)i / frames - 1.0;
      *l *= t * t * t + 1;
      *r *= t * t * t + 1;
    }
  }
}

static guint hal_commit (GstAmlHalAsink * sink, guchar * data,
    gint size, guint64 pts_64)
{
  GstAmlHalAsinkPrivate *priv = sink->priv;
  guint towrite;
  gboolean raw_data;
  guint offset = 0;
  guint hw_header_s = sizeof (struct hw_sync_header_v2);

  if (!priv->stream_) {
    GST_WARNING_OBJECT (sink, "stream closed");
    return 0;
  }

  raw_data = is_raw_type(priv->spec.type);
  towrite = size;

  if (towrite)
    dump ("/data/asink_", data, towrite);

  if (priv->format_ == AUDIO_FORMAT_AC4) {
    /* parse sync frame */
    parse_bit_stream(sink, data, towrite);
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

    if (priv->flushing_)
      break;

    if (priv->format_ == AUDIO_FORMAT_AC3) {
      /* Frame aligned
       * AC4 has constant bit rate (CBR) and variable bit reate(VBR) streams
       * And VBR doesn't have to be encoded size aligned.
       * EAC3 has dependent stream and substream. Hard to split.
       */
      if (parse_bit_stream(sink, data, towrite) < 0 || towrite < priv->encoded_size) {
        GST_WARNING_OBJECT (sink, "stream not frame aligned left %d discarded", towrite);
        return size;
      }
      cur_size = priv->encoded_size;
    } else if (priv->tempo_used) {
      cur_size = scaletemp_get_stride(&priv->st);
      if (cur_size > towrite)
        cur_size = towrite;
    } else
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
          if (raw_data) {
            /* truncate and alight to 16B */
            cur_size = MAX_TRANS_BUF_SIZE - hw_header_s;
            cur_size &= 0xFFFFFFF0;
          } else {
            GST_ERROR_OBJECT(sink, "frame too big %d", cur_size);
            return offset;
          }
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

      if (priv->diag_log_enable)
        diag_print (sink, pts_32);
    } else if (raw_data) {
      /* audio hal can not handle too big frame, limit to 4K*/
      if (cur_size > 4*1024) {
        cur_size = 4*1024;
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

    GST_LOG_OBJECT (sink,
        "write %d/%d left %d ts: %llu", written, cur_size, towrite, pts_64);

    /* update PTS for next sample */
    if (priv->direct_mode_) {
      if (priv->sr_) {
        if (raw_data) {
          gint bpf = GST_AUDIO_INFO_BPF (&priv->spec.info);

          if (bpf)
            pts_inc = gst_util_uint64_scale_int (written/bpf,
                GST_SECOND, priv->sr_);
        } else
          pts_inc = gst_util_uint64_scale_int (priv->sample_per_frame,
              GST_SECOND, priv->sr_);
      } else
        GST_WARNING_OBJECT (sink, "invalid sample rate %d",  priv->sr_);
      pts_64 += pts_inc;
    }

    if (priv->xrun_timer)
      g_timer_start(priv->xrun_timer);

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

GstClock *gst_aml_hal_asink_get_clock (GstElement *element)
{
  return gst_aml_hal_asink_provide_clock (element);
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
