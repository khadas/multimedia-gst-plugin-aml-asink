/* GStreamer
 * Copyright (C) 2020 <song.zhao@amlogic.com>
 *
 * gstamlclock.c: Clock for use by amlogic pipeline
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <gst/gstclock.h>
#include "gstamlclock.h"
#include "aml_avsync.h"

GST_DEBUG_CATEGORY_STATIC (gst_aml_clock_debug);
#define GST_CAT_DEFAULT gst_aml_clock_debug
#define GST_AML_CLOCK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_AML_CLOCK, GstAmlClockPrivate))

struct _GstAmlClockPrivate
{
  GstAmlClockGetTimeFunc   func;
  gpointer                 user_data;
  GDestroyNotify           destroy_notify;
  GstClockTime             last_time;

  int session;
  int session_id;
};

#define parent_class gst_aml_clock_parent_class
#if GST_CHECK_VERSION(1,14,0)
G_DEFINE_TYPE_WITH_CODE (GstAmlClock, gst_aml_clock, GST_TYPE_SYSTEM_CLOCK,
	GST_DEBUG_CATEGORY_INIT (gst_aml_clock_debug, "audioclock", 0,
	"amlclock");G_ADD_PRIVATE(GstAmlClock));
#else
G_DEFINE_TYPE_WITH_CODE (GstAmlClock, gst_aml_clock, GST_TYPE_SYSTEM_CLOCK,
	GST_DEBUG_CATEGORY_INIT (gst_aml_clock_debug, "audioclock", 0,
	"amlclock"););
#endif

static void gst_aml_clock_dispose (GObject * object);
static GstClockTime gst_aml_clock_get_internal_time (GstClock * clock);

static void
gst_aml_clock_class_init (GstAmlClockClass * klass)
{
  GObjectClass *gobject_class;
  GstClockClass *gstclock_class;

  gobject_class = (GObjectClass *) klass;
  gstclock_class = (GstClockClass *) klass;
  gstclock_class->get_internal_time = gst_aml_clock_get_internal_time;
  gobject_class->dispose = gst_aml_clock_dispose;

#if GST_CHECK_VERSION(1,14,0)
#else
  g_type_class_add_private (klass, sizeof (GstAmlClockPrivate));
#endif
}

static void
gst_aml_clock_init (GstAmlClock * clock)
{
  GST_DEBUG_OBJECT (clock, "init");
  GST_OBJECT_FLAG_SET (clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
#if GST_CHECK_VERSION(1,14,0)
  GstAmlClockPrivate *priv = gst_aml_clock_get_instance_private (clock);
#else
  GstAmlClockPrivate *priv = GST_AML_CLOCK_GET_PRIVATE (clock);
#endif
  clock->priv = priv;
  priv->session_id = -1;
  priv->last_time = 0;

  priv->session = av_sync_open_session(&priv->session_id);
  if (priv->session < 0) {
    GST_ERROR("can not create session");
  }
}

static void
gst_aml_clock_dispose (GObject * object)
{
  GstAmlClock *clock = GST_AML_CLOCK (object);
  GstAmlClockPrivate *priv = clock->priv;

  GST_DEBUG_OBJECT (clock, "dispose");
  if (priv->destroy_notify && priv->user_data)
    priv->destroy_notify (priv->user_data);
  priv->destroy_notify = NULL;
  priv->user_data = NULL;
  if (priv->session >= 0)
    av_sync_close_session(priv->session);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

/**
 * gst_aml_clock_new:
 * @name: the name of the clock
 * @user_data: user data
 * @destroy_notify: #GDestroyNotify for @user_data
 *
 * Create a new #GstAmlClock instance. Whenever the clock time should be
 * calculated it will call @func with @user_data. When @func returns
 * #GST_CLOCK_TIME_NONE, the clock will return the last reported time.
 *
 * Returns: a new #GstAmlClock casted to a #GstClock.
 */
GstClock *
gst_aml_clock_new (const gchar * name, GstAmlClockGetTimeFunc func,
    gpointer user_data, GDestroyNotify destroy_notify)
{
  GstAmlClock *aclock =
      GST_AML_CLOCK (g_object_new (GST_TYPE_AML_CLOCK, "name", name,
          "clock-type", GST_CLOCK_TYPE_OTHER, NULL));
  GstAmlClockPrivate *priv = aclock->priv;

  priv->func = func;
  priv->user_data = user_data;
  priv->destroy_notify = destroy_notify;

  return (GstClock *) aclock;
}

/**
 * gst_aml_clock_get_session_id:
 * return session ID used for audio and video sink.
 * This ID need to be passed to libamlavsync.so
 */
int gst_aml_clock_get_session_id(GstClock * clock)
{
  GstAmlClock *aclock =  GST_AML_CLOCK_CAST (clock);
  GstAmlClockPrivate *priv = aclock->priv;

  return priv->session_id;
}

static GstClockTime
gst_aml_clock_get_internal_time (GstClock * clock)
{
  GstAmlClock *aclock = GST_AML_CLOCK_CAST (clock);
  GstAmlClockPrivate *priv = aclock->priv;
  GstClockTime result;


  result = priv->func (clock, priv->user_data);
  if (result == GST_CLOCK_TIME_NONE) {
    result = priv->last_time;
  } else {
    /* clock must be increasing */
    if (priv->last_time < result)
      priv->last_time = result;
    else
      result = priv->last_time;
  }

  GST_DEBUG_OBJECT (aclock,
      "result %" GST_TIME_FORMAT ", last_time %" GST_TIME_FORMAT,
      GST_TIME_ARGS (result), GST_TIME_ARGS (priv->last_time));

  return result;
}
