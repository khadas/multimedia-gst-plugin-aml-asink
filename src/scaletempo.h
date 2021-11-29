/*
 * GStreamer
 * Copyright (C) 2008 Rov Juvano <rovjuvano@users.sourceforge.net>
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

/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

struct scale_tempo
{
  gdouble scale;
  GstBuffer *queued_buf;

  /* parameters */
  guint ms_stride;
  gdouble percent_overlap;
  guint ms_search;

  /* caps */
  GstAudioFormat format;
  guint samples_per_frame;      /* AKA number of channels */
  guint bytes_per_sample;
  guint bytes_per_frame;
  guint sample_rate;

  /* stride */
  gdouble frames_stride_scaled;
  gdouble frames_stride_error;
  guint bytes_stride;
  gdouble bytes_stride_scaled;
  guint bytes_queue_max;
  guint bytes_queued;
  guint bytes_to_slide;
  gint8 *buf_queue;

  /* overlap */
  guint samples_overlap;
  guint samples_standing;
  guint bytes_overlap;
  guint bytes_standing;
  gpointer buf_overlap;
  gpointer table_blend;
  void (*output_overlap) (struct scale_tempo * scaletempo, gpointer out_buf, guint bytes_off);

  /* best overlap */
  guint frames_search;
  gpointer buf_pre_corr;
  gpointer table_window;
  guint (*best_overlap_offset) (struct scale_tempo * scaletempo);

  gint64      segment_start;
  /* threads */
  gboolean reinit_buffers;
  gboolean first_frame_flag;
};


void scaletempo_init (struct scale_tempo * scaletempo);
gboolean scaletempo_start (struct scale_tempo * scaletempo);
gboolean scaletempo_stop (struct scale_tempo * scaletempo);
gboolean scaletempo_set_info (struct scale_tempo * scaletempo, GstAudioInfo * info);
void scaletempo_update_segment (struct scale_tempo * scaletempo, GstSegment* event);
gboolean scaletempo_transform_size (struct scale_tempo * scaletempo,
    gsize size, gsize * othersize);
GstFlowReturn scaletempo_transform (struct scale_tempo * st,
    GstBuffer * inbuf, GstBuffer * outbuf);
gint scaletemp_get_stride (struct scale_tempo * scaletempo);
