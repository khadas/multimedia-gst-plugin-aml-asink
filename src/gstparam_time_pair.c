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

#include <gobject/gvaluecollector.h>
#include "gstparam_time_pair.h"

/* --- GType GstTimePair --- */

static GTypeInfo _info = {
  0, NULL, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL,
};

static GTypeFundamentalInfo _finfo = {
  0
};

static void
gst_value_init_time_pair (GValue * value)
{
  value->data[0].v_int64 = -1;
  value->data[1].v_uint64 = 0;
}

static void
gst_value_copy_time_pair (const GValue * src_value, GValue * dest_value)
{
  dest_value->data[0].v_int64 = src_value->data[0].v_int64;
  dest_value->data[1].v_uint64 = src_value->data[1].v_uint64;
}

static gchar *
gst_value_collect_time_pair (GValue * value, guint n_collect_values,
    GTypeCValue * collect_values, guint collect_flags)
{
  if (n_collect_values != 2)
    return g_strdup_printf ("not enough value locations for `%s' passed",
        G_VALUE_TYPE_NAME (value));

  gst_value_set_time_pair (value,
      collect_values[0].v_int64, collect_values[1].v_int64);

  return NULL;
}

static gchar *
gst_value_lcopy_time_pair (const GValue * value, guint n_collect_values,
    GTypeCValue * collect_values, guint collect_flags)
{
  gint64 *pos = collect_values[0].v_pointer;
  guint64 *mono = collect_values[1].v_pointer;

  if (!pos)
    return g_strdup_printf ("pos for `%s' passed as NULL",
        G_VALUE_TYPE_NAME (value));
  if (!mono)
    return g_strdup_printf ("mono for `%s' passed as NULL",
        G_VALUE_TYPE_NAME (value));

  *pos = value->data[0].v_int64;
  *mono = value->data[1].v_uint64;

  return NULL;
}

static const GTypeValueTable _gst_time_pair_value_table = {
  gst_value_init_time_pair,
  NULL,
  gst_value_copy_time_pair,
  NULL,
  (char *) "ii",
  gst_value_collect_time_pair, (char *) "pp", gst_value_lcopy_time_pair
};

GType gst_time_pair_get_type (void)
{
  static volatile GType gst_time_pair_type = 0;

  if (g_once_init_enter (&gst_time_pair_type)) {
    GType _type;
    _info.class_size = 0;
    _finfo.type_flags = 0;
    _info.value_table = & _gst_time_pair_value_table;

    _type = g_type_register_fundamental (
        g_type_fundamental_next (),
        "GstTimePair", &_info, &_finfo, 0);
    g_once_init_leave(&gst_time_pair_type, _type);
  }

  return gst_time_pair_type;
}

/* --- GstParamSpecTimePair --- */

static void
_gst_param_time_pair_init (GParamSpec * pspec)
{
  GstParamSpecTimePair *fspec = GST_PARAM_SPEC_TIME_PAIR (pspec);

  fspec->pos= G_MAXINT64;
  fspec->mono = 0;
  fspec->def_pos = G_MININT64;
  fspec->def_mono = 0;
}

static void
_gst_param_time_pair_set_default (GParamSpec * pspec, GValue * value)
{
  value->data[0].v_int64 = GST_PARAM_SPEC_TIME_PAIR (pspec)->def_pos;
  value->data[1].v_uint64 = GST_PARAM_SPEC_TIME_PAIR (pspec)->def_mono;
}

static gboolean
_gst_param_time_pair_validate (GParamSpec * pspec, GValue * value)
{
  GstParamSpecTimePair *fspec = GST_PARAM_SPEC_TIME_PAIR (pspec);
  /* return FALSE if everything ok, otherwise TRUE */

  return FALSE;
}

#if 0
static gint
_gst_param_fraction_values_cmp (GParamSpec * pspec, const GValue * value1,
    const GValue * value2)
{
  gint res;

  res = gst_value_compare (value1, value2);

  g_assert (res != GST_VALUE_UNORDERED);

  /* GST_VALUE_LESS_THAN is -1, EQUAL is 0, and GREATER_THAN is 1 */
  return res;
}
#endif


GType
gst_param_spec_time_pair_get_type (void)
{
  static volatile GType gst_spec_time_pair_type = 0;

  /* register GST_TYPE_PARAM_TIME_PAIR */
  if (g_once_init_enter (&gst_spec_time_pair_type)) {
    GType type;
    static GParamSpecTypeInfo pspec_info = {
      sizeof (GstParamSpecTimePair),    /* instance_size     */
      0,                        /* n_preallocs       */
      _gst_param_time_pair_init, /* instance_init     */
      G_TYPE_INVALID,           /* value_type        */
      NULL,                     /* finalize          */
      _gst_param_time_pair_set_default,  /* value_set_default */
      _gst_param_time_pair_validate,     /* value_validate    */
      NULL ,   /* values_cmp        */
    };
    pspec_info.value_type = gst_time_pair_get_type();
    type = g_param_type_register_static ("GstParamTimePair", &pspec_info);

    /* register GValue type */
    static GstValueTable gstvtable = {
      G_TYPE_NONE,
      (GstValueCompareFunc) NULL,
      (GstValueSerializeFunc) NULL,
      (GstValueDeserializeFunc)NULL
    };
    gstvtable.type = gst_time_pair_get_type();
    gst_value_register (&gstvtable);
    g_once_init_leave (&gst_spec_time_pair_type, type);
  }

  return gst_spec_time_pair_type;
}

/**
 * gst_param_spec_time_pair:
 * @name: canonical name of the property specified
 * @nick: nick name for the property specified
 * @blurb: description of the property specified
 * @default_pos: default value
 * @default_mono: default value
 * @flags: flags for the property specified
 *
 * This function creates a time pair GParamSpec for use by objects/elements
 * that want to expose properties of time pair type. This function is typically
 * used in connection with g_object_class_install_property() in a GObjects's
 * instance_init function.
 *
 * Returns: (transfer full) (nullable): a newly created parameter specification
 */
GParamSpec *
gst_param_spec_time_pair (const gchar * name, const gchar * nick,
    const gchar * blurb, gint64 default_pos, guint64 default_mono,
    GParamFlags flags)
{
  GstParamSpecTimePair *fspec;
  GParamSpec *pspec;

  fspec =
      g_param_spec_internal (GST_TYPE_PARAM_TIME_PAIR, name, nick, blurb, flags);

  fspec->def_pos= default_pos;
  fspec->def_mono= default_mono;

  pspec = G_PARAM_SPEC (fspec);

  return pspec;
}

gint64 gst_value_get_time_pair_pos (const GValue * value)
{
  return value->data[0].v_int64;
}

guint64 gst_value_get_time_pair_mono (const GValue * value)
{
  return value->data[1].v_uint64;
}

void gst_value_set_time_pair (GValue * value, const gint64 pos, const guint64 mono)
{
  value->data[0].v_int64 = pos;
  value->data[1].v_uint64 = mono;
}
