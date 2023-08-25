/* GStreamer
 * Some extracts from GStreamer Plugins Base, which is:
 * Copyright 2023 Amlogic, Inc.
 * Here licensed under the GNU Lesser General Public License, version 2.1
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


#ifndef __GST_PARAM_TIME_PAIR_H__
#define __GST_PARAM_TIME_PAIR_H__

#include <gst/gstvalue.h>

G_BEGIN_DECLS

/**
 * GST_TYPE_TIME_PAIR:
 *
 * a #GValue type that represents a pair of PTS and system mono time
 *
 * Returns: the #GType of GstTimePair (which is not explicitly typed)
 */

#define GST_TYPE_TIME_PAIR                (gst_time_pair_get_type ())

GST_API
GType gst_time_pair_get_type (void);

/* --- type macros --- */

#define GST_TYPE_PARAM_TIME_PAIR           (gst_param_spec_time_pair_get_type ())
#define GST_IS_PARAM_SPEC_TIME_PAIR(pspec) (G_TYPE_CHECK_INSTANCE_TYPE ((pspec), GST_TYPE_PARAM_TIME_PAIR))
#define GST_PARAM_SPEC_TIME_PAIR(pspec)    (G_TYPE_CHECK_INSTANCE_CAST ((pspec), GST_TYPE_PARAM_TIME_PAIR, GstParamSpecTimePair))

/* --- get_type functions --- */
GST_API
GType  gst_param_spec_time_pair_get_type (void);

/* --- typedefs & structures --- */
typedef struct _GstParamSpecTimePair GstParamSpecTimePair;

/**
 * GstParamTimePair
 * @parent_instance: super class
 * @pos: media time in ns.
 * @mono: system mono time get by clock_gettime(CLOCK_MONOTONIC_RAW) in ns
 *
 * A GParamSpec derived structure that contains pair of render pos at system time.
 */
struct _GstParamSpecTimePair {
  GParamSpec parent_instance;

  gint64 pos;
  guint64 mono;
  gint64 def_pos;
  guint64 def_mono;
};

/* --- GParamSpec prototypes --- */
GST_API
GParamSpec * gst_param_spec_time_pair (const gchar * name,
    const gchar * nick,
    const gchar * blurb,
    gint64 default_pos,
    guint64 default_mono,
    GParamFlags flags) G_GNUC_MALLOC;

gint64 gst_value_get_time_pair_pos (const GValue * value);
guint64 gst_value_get_time_pair_mono (const GValue * value);
void gst_value_set_time_pair (GValue * value, const gint64 pos, const guint64 mono);

G_END_DECLS

#endif
