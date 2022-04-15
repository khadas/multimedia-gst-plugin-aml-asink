/* GStreamer
 * Copyright (C) 2020 <song.zhao@amlogic.com>
 *
 * gstamlclock.h: Clock for use by amlogic pipeline
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

//#ifndef __GST_AUDIO_AUDIO_H__
//#include <gst/audio/audio.h>
//#endif

#ifndef __GST_AML_CLOCK_H__
#define __GST_AML_CLOCK_H__

#include <gst/gst.h>
#include <gst/gstsystemclock.h>

G_BEGIN_DECLS

#define GST_TYPE_AML_CLOCK \
  (gst_aml_clock_get_type())
#define GST_AML_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AML_CLOCK,GstAmlClock))
#define GST_AML_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AML_CLOCK,GstAmlClockClass))
#define GST_IS_AML_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AML_CLOCK))
#define GST_IS_AML_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AML_CLOCK))
#define GST_AML_CLOCK_CAST(obj) \
  ((GstAmlClock*)(obj))

typedef struct _GstAmlClock GstAmlClock;
typedef struct _GstAmlClockClass GstAmlClockClass;
typedef struct _GstAmlClockPrivate GstAmlClockPrivate;

/**
 * GstAmlClockGetTimeFunc:
 * @clock: the #GstAmlClock
 * @user_data: user data
 *
 * This function will be called whenever the current clock time needs to be
 * calculated. If this function returns #GST_CLOCK_TIME_NONE, the last reported
 * time will be returned by the clock.
 *
 * Returns: the current time or #GST_CLOCK_TIME_NONE if the previous time should
 * be used.
 */
typedef GstClockTime (*GstAmlClockGetTimeFunc) (GstClock *clock, gpointer user_data);

/**
 * GstAmlClock:
 *
 * Opaque #GstAmlClock.
 */
struct _GstAmlClock {
  GstSystemClock clock;
  void * handle;
  /*< private >*/
  GstAmlClockPrivate *priv;
};

struct _GstAmlClockClass {
  GstSystemClockClass parent_class;
};

GType           gst_aml_clock_get_type        (void);
GstClock*       gst_aml_clock_new             (const gchar *name, GstAmlClockGetTimeFunc func,
                                                 gpointer user_data, GDestroyNotify destroy_notify);
GstClockTime    gst_aml_clock_get_time        (GstClock * clock);
int             gst_aml_clock_get_session_id  (GstClock * clock);

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GstAmlClock, gst_object_unref)
#endif

G_END_DECLS

#endif /* __GST_AML_CLOCK_H__ */
