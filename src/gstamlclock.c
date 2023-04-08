/* GStreamer
 * Copyright (C) 2020 Amlogic, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free SoftwareFoundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <gst/gstclock.h>
#include <stdlib.h>
#include <stdio.h>

#include "gstamlclock.h"
#include "aml_avsync.h"
#include "mediasync_wrap.h"


GST_DEBUG_CATEGORY_STATIC (gst_aml_clock_debug);
#define GST_CAT_DEFAULT gst_aml_clock_debug
#define GST_AML_CLOCK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_AML_CLOCK, GstAmlClockPrivate))

struct _GstAmlClockPrivate
{
  GstAmlClockGetTimeFunc   func;
  gpointer                 user_data;
  GDestroyNotify           destroy_notify;

  enum gst_aml_clock_type type;
  int session;
  int session_id;
};

#define parent_class gst_aml_clock_parent_class
#if GLIB_CHECK_VERSION(2,58,0)
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

#if GLIB_CHECK_VERSION(2,58,0)
#else
  g_type_class_add_private (klass, sizeof (GstAmlClockPrivate));
#endif
}

static void
gst_aml_clock_init (GstAmlClock * clock)
{
  GST_DEBUG_OBJECT (clock, "init");
  GST_OBJECT_FLAG_SET (clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
#if GLIB_CHECK_VERSION(2,58,0)
  GstAmlClockPrivate *priv = gst_aml_clock_get_instance_private (clock);
#else
  GstAmlClockPrivate *priv = GST_AML_CLOCK_GET_PRIVATE (clock);
#endif
//if bb file set --enable-mediasync=yes, enable mediasync default
#ifdef ENABLE_MEDIA_SYNC
  priv->type = GST_AML_CLOCK_TYPE_MEDIASYNC;
#endif
  const char *env = getenv("AML_AV_SYNC_TYPE");
  clock->priv = priv;
  priv->session_id = -1;

  if (env) {
    priv->type = atoi(env);
    GST_WARNING("clock type %d, env %s", priv->type, env);
  }

  if (priv->type != GST_AML_CLOCK_TYPE_MSYNC && priv->type != GST_AML_CLOCK_TYPE_MEDIASYNC) {
    priv->type = GST_AML_CLOCK_TYPE_MSYNC;
  }

  GST_WARNING("clock type %d", priv->type);

  if (priv->type == GST_AML_CLOCK_TYPE_MEDIASYNC) {
    clock->handle = mediasync_wrap_create();
    if (!clock->handle) {
      GST_ERROR("could not create mediasync handle");
      return;
    }
    priv->session = mediasync_wrap_allocInstance(clock->handle, 0, 0, &priv->session_id);
    FILE * fp;
    fp = fopen("/data/MediaSyncId", "w");
    if (fp == NULL) {
      GST_ERROR("could not open file:/data/MediaSyncId failed");
    } else {
      fwrite(&priv->session_id, sizeof(int), 1, fp);
      fclose(fp);
    }
  } else {
    clock->handle = NULL;
    priv->session = av_sync_open_session(&priv->session_id);
  }

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

  if (clock->handle && priv->type == GST_AML_CLOCK_TYPE_MEDIASYNC) {
    mediasync_wrap_destroy(clock->handle);
  } else if (priv->session >= 0) {
      av_sync_close_session(priv->session);
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

/**
 * gst_aml_clock_new:
 * @name: the name of the clock
 * @user_data: user data
 * @destroy_notify: #GDestroyNotify for @user_data
 *
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright 2000 Wim Taymans <wtay@chello.be>
 * Here licensed under the GNU Lesser General Public License, version 2.1
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

  GST_DEBUG_OBJECT (aclock,
      "result %" GST_TIME_FORMAT, GST_TIME_ARGS (result));

  return result;
}

GstClockTime gst_aml_clock_get_time (GstClock * clock)
{
  return gst_aml_clock_get_internal_time (clock);
}

enum gst_aml_clock_type gst_aml_clock_get_clock_type (GstClock * clock)
{
    GstAmlClock *aclock = GST_AML_CLOCK_CAST (clock);
    GstAmlClockPrivate *priv = aclock->priv;

    return priv->type;
}
