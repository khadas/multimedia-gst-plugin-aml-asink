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
#include <string.h>
#include <gst/audio/audio.h>
#include "scaletempo.h"

GST_DEBUG_CATEGORY_EXTERN(gst_aml_hal_asink_debug_category);
#define GST_CAT_DEFAULT gst_aml_hal_asink_debug_category

/* buffer padding for loop optimization: sizeof(gint32) * (loop_size - 1) */
#define UNROLL_PADDING (4*3)
static guint
best_overlap_offset_s16 (struct scale_tempo * st)
{
  gint32 *pw, *ppc;
  gint16 *po, *search_start;
  gint64 best_corr = G_MININT64;
  guint best_off = 0;
  guint off;
  glong i;
  guint ret = 0;

  if (1.0 == st->scale){
    ret = 0;
  } else {
    pw = st->table_window;
    po = st->buf_overlap;
    po += st->samples_per_frame;
    ppc = st->buf_pre_corr;
    for (i = st->samples_per_frame; i < st->samples_overlap; i++) {
      *ppc++ = (*pw++ * *po++) >> 15;
    }

    search_start = (gint16 *) st->buf_queue + st->samples_per_frame;
    for (off = 0; off < st->frames_search; off++) {
      gint64 corr = 0;
      gint16 *ps = search_start;
      ppc = st->buf_pre_corr;
      ppc += st->samples_overlap - st->samples_per_frame;
      ps += st->samples_overlap - st->samples_per_frame;
      i = -((glong) st->samples_overlap - (glong) st->samples_per_frame);
      do {
        corr += ppc[i + 0] * ps[i + 0];
        corr += ppc[i + 1] * ps[i + 1];
        corr += ppc[i + 2] * ps[i + 2];
        corr += ppc[i + 3] * ps[i + 3];
        i += 4;
      } while (i < 0);
      if (corr > best_corr) {
        best_corr = corr;
        best_off = off;
      }
      search_start += st->samples_per_frame;
    }
    ret = best_off * st->bytes_per_frame;
  }

  return ret;
}

static void
output_overlap_s16 (struct scale_tempo * st, gpointer buf_out, guint bytes_off)
{
  gint16 *pout = buf_out;
  gint32 *pb = st->table_blend;
  gint16 *po = st->buf_overlap;
  gint16 *pin = (gint16 *) (st->buf_queue + bytes_off);
  gint i;

  if (1.0 == st->scale){
    memcpy (pout, st->buf_queue, st->bytes_overlap);
  } else {
    for (i = 0; i < st->samples_overlap; i++) {
      *pout++ = *po - ((*pb++ * (*po - *pin++)) >> 16);
      po++;
    }
  }
}

static guint
fill_queue (struct scale_tempo * st, GstBuffer * buf_in, guint offset)
{
  guint bytes_in = gst_buffer_get_size (buf_in) - offset;
  guint offset_unchanged = offset;
  GstMapInfo map;

  gst_buffer_map (buf_in, &map, GST_MAP_READ);
  if (st->bytes_to_slide > 0) {
    if (st->bytes_to_slide < st->bytes_queued) {
      guint bytes_in_move = st->bytes_queued - st->bytes_to_slide;
      memmove (st->buf_queue, st->buf_queue + st->bytes_to_slide,
          bytes_in_move);
      st->bytes_to_slide = 0;
      st->bytes_queued = bytes_in_move;
    } else {
      guint bytes_in_skip;
      st->bytes_to_slide -= st->bytes_queued;
      bytes_in_skip = MIN (st->bytes_to_slide, bytes_in);
      st->bytes_queued = 0;
      st->bytes_to_slide -= bytes_in_skip;
      offset += bytes_in_skip;
      bytes_in -= bytes_in_skip;
    }
  }

  if (bytes_in > 0) {
    guint bytes_in_copy =
        MIN (st->bytes_queue_max - st->bytes_queued, bytes_in);
    memcpy (st->buf_queue + st->bytes_queued, map.data + offset, bytes_in_copy);
    st->bytes_queued += bytes_in_copy;
    offset += bytes_in_copy;
  }
  gst_buffer_unmap (buf_in, &map);

  return offset - offset_unchanged;
}

static void
reinit_buffers (struct scale_tempo * st)
{
  gint i, j;
  guint frames_overlap;
  guint new_size;

  guint frames_stride = st->ms_stride * st->sample_rate / 1000.0;
  st->bytes_stride = frames_stride * st->bytes_per_frame;

  /* overlap */
  frames_overlap = frames_stride * st->percent_overlap;
  if (frames_overlap < 1) {     /* if no overlap */
    st->bytes_overlap = 0;
    st->bytes_standing = st->bytes_stride;
    st->samples_standing = st->bytes_standing / st->bytes_per_sample;
    st->output_overlap = NULL;
  } else {
    guint prev_overlap = st->bytes_overlap;
    st->bytes_overlap = frames_overlap * st->bytes_per_frame;
    st->samples_overlap = frames_overlap * st->samples_per_frame;
    st->bytes_standing = st->bytes_stride - st->bytes_overlap;
    st->samples_standing = st->bytes_standing / st->bytes_per_sample;
    st->buf_overlap = g_realloc (st->buf_overlap, st->bytes_overlap);
    /* S16 uses gint32 blend table, floats/doubles use their respective type */
    st->table_blend =
        g_realloc (st->table_blend,
        st->samples_overlap * (st->format ==
            GST_AUDIO_FORMAT_S16 ? 4 : st->bytes_per_sample));
    if (st->bytes_overlap > prev_overlap) {
      memset ((guint8 *) st->buf_overlap + prev_overlap, 0,
          st->bytes_overlap - prev_overlap);
    }
    if (st->format == GST_AUDIO_FORMAT_S16) {
      gint32 *pb = st->table_blend;
      gint64 blend = 0;
      for (i = 0; i < frames_overlap; i++) {
        gint32 v = blend / frames_overlap;
        for (j = 0; j < st->samples_per_frame; j++) {
          *pb++ = v;
        }
        blend += 65535;         /* 2^16 */
      }
      st->output_overlap = output_overlap_s16;
    }
  }

  /* best overlap */
  st->frames_search =
      (frames_overlap <= 1) ? 0 : st->ms_search * st->sample_rate / 1000.0;
  if (st->frames_search < 1) {  /* if no search */
    st->best_overlap_offset = NULL;
  } else {
    /* S16 uses gint32 buffer, floats/doubles use their respective type */
    guint bytes_pre_corr =
        (st->samples_overlap - st->samples_per_frame) * (st->format ==
        GST_AUDIO_FORMAT_S16 ? 4 : st->bytes_per_sample);
    st->buf_pre_corr =
        g_realloc (st->buf_pre_corr, bytes_pre_corr + UNROLL_PADDING);
    st->table_window = g_realloc (st->table_window, bytes_pre_corr);
    if (st->format == GST_AUDIO_FORMAT_S16) {
      gint64 t = frames_overlap;
      gint32 n = 8589934588LL / (t * t);        /* 4 * (2^31 - 1) / t^2 */
      gint32 *pw;

      memset ((guint8 *) st->buf_pre_corr + bytes_pre_corr, 0, UNROLL_PADDING);
      pw = st->table_window;
      for (i = 1; i < frames_overlap; i++) {
        gint32 v = (i * (t - i) * n) >> 15;
        for (j = 0; j < st->samples_per_frame; j++) {
          *pw++ = v;
        }
      }
      st->best_overlap_offset = best_overlap_offset_s16;
    }
  }

  new_size =
      (st->frames_search + frames_stride +
      frames_overlap) * st->bytes_per_frame;
  if (st->bytes_queued > new_size) {
    if (st->bytes_to_slide > st->bytes_queued) {
      st->bytes_to_slide -= st->bytes_queued;
      st->bytes_queued = 0;
    } else {
      guint new_queued = MIN (st->bytes_queued - st->bytes_to_slide, new_size);
      memmove (st->buf_queue,
          st->buf_queue + st->bytes_queued - new_queued, new_queued);
      st->bytes_to_slide = 0;
      st->bytes_queued = new_queued;
    }
  }

  st->bytes_queue_max = new_size;
  st->buf_queue = g_realloc (st->buf_queue, st->bytes_queue_max);

  st->bytes_stride_scaled = st->bytes_stride * st->scale;
  st->frames_stride_scaled = st->bytes_stride_scaled / st->bytes_per_frame;

  GST_DEBUG
      ("%.3f scale, %.3f stride_in, %i stride_out, %i standing, %i overlap, %i search, %i queue, %s mode",
      st->scale, st->frames_stride_scaled,
      (gint) (st->bytes_stride / st->bytes_per_frame),
      (gint) (st->bytes_standing / st->bytes_per_frame),
      (gint) (st->bytes_overlap / st->bytes_per_frame), st->frames_search,
      (gint) (st->bytes_queue_max / st->bytes_per_frame),
      gst_audio_format_to_string (st->format));

  st->reinit_buffers = FALSE;
}

/* GstBaseTransform vmethod implementations */
GstFlowReturn scaletempo_transform (struct scale_tempo * st,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  gint8 *pout;
  guint offset_in, bytes_out;
  GstMapInfo omap;
  guint64 timestamp;
  guint offset_out = 0;

  gst_buffer_map (outbuf, &omap, GST_MAP_WRITE);
  pout = (gint8 *) omap.data;
  bytes_out = omap.size;

  offset_in = fill_queue (st, inbuf, 0);
  bytes_out = 0;
  while (st->bytes_queued >= st->bytes_queue_max) {
    guint bytes_off = 0;
    gdouble frames_to_slide;
    guint frames_to_stride_whole;

    /* output stride */
    if (st->output_overlap) {
      if (st->best_overlap_offset) {
        bytes_off = st->best_overlap_offset (st);
      }
      st->output_overlap (st, pout, bytes_off);
    }
    memcpy (pout + st->bytes_overlap,
        st->buf_queue + bytes_off + st->bytes_overlap, st->bytes_standing);
    pout += st->bytes_stride;
    bytes_out += st->bytes_stride;

    /* input stride */
    memcpy (st->buf_overlap,
        st->buf_queue + bytes_off + st->bytes_stride, st->bytes_overlap);
    frames_to_slide = st->frames_stride_scaled + st->frames_stride_error;
    frames_to_stride_whole = (gint) frames_to_slide;
    st->bytes_to_slide = frames_to_stride_whole * st->bytes_per_frame;
    st->frames_stride_error = frames_to_slide - frames_to_stride_whole;

    offset_in += fill_queue (st, inbuf, offset_in);
  }
  gst_buffer_unmap (outbuf, &omap);


  GST_BUFFER_DURATION (outbuf) =
    gst_util_uint64_scale (bytes_out, GST_SECOND,
    st->bytes_per_frame * st->sample_rate);
  offset_out = bytes_out * st->scale + st->bytes_queued - gst_buffer_get_size (inbuf);
  timestamp = GST_BUFFER_TIMESTAMP (inbuf) - gst_util_uint64_scale (offset_out, GST_SECOND, st->bytes_per_frame * st->sample_rate);;
  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;
  gst_buffer_set_size (outbuf, bytes_out);

  return GST_FLOW_OK;
}

gboolean scaletempo_transform_size (struct scale_tempo * scaletempo,
    gsize size, gsize * othersize)
{
  gint bytes_to_out;

  if (scaletempo->reinit_buffers)
    reinit_buffers (scaletempo);

  bytes_to_out = size + scaletempo->bytes_queued - scaletempo->bytes_to_slide;
  if (bytes_to_out < (gint) scaletempo->bytes_queue_max) {
    GST_TRACE ("size %d bytes_to_out %d queued %d slide %d max %d",
        size, bytes_to_out, scaletempo->bytes_queued,
        scaletempo->bytes_to_slide, scaletempo->bytes_queue_max);
    *othersize = 0;
  } else {
    /* while (total_buffered - stride_length * n >= queue_max) n++ */
    *othersize = scaletempo->bytes_stride * ((guint) (
            (bytes_to_out - scaletempo->bytes_queue_max +
                /* rounding protection */ scaletempo->bytes_per_frame)
            / scaletempo->bytes_stride_scaled) + 1);
  }

  return TRUE;
}

void scaletempo_update_segment (struct scale_tempo * scaletempo, GstSegment * segment)
{
  if (segment->format != GST_FORMAT_TIME
      || scaletempo->scale != ABS (segment->rate)) {
    if (segment->format != GST_FORMAT_TIME || ABS (segment->rate - 1.0) < 1e-10) {
      scaletempo->scale = 1.0;
      scaletempo->bytes_stride_scaled =
          scaletempo->bytes_stride;
      scaletempo->frames_stride_scaled =
          scaletempo->bytes_stride_scaled / scaletempo->bytes_per_frame;
      scaletempo->bytes_to_slide = 0;
    } else {
      scaletempo->scale = ABS (segment->rate);
      scaletempo->bytes_stride_scaled =
          scaletempo->bytes_stride * scaletempo->scale;
      scaletempo->frames_stride_scaled =
          scaletempo->bytes_stride_scaled / scaletempo->bytes_per_frame;
      GST_DEBUG ("%.3f scale, %.3f stride_in, %i stride_out",
          scaletempo->scale, scaletempo->frames_stride_scaled,
          (gint) (scaletempo->bytes_stride / scaletempo->bytes_per_frame));

      scaletempo->bytes_to_slide = 0;
    }
  }
  scaletempo->segment_start = segment->start;
}

gboolean scaletempo_set_info (struct scale_tempo * scaletempo, GstAudioInfo * info)
{
  gint width, bps, nch, rate;
  GstAudioFormat format;

  nch = GST_AUDIO_INFO_CHANNELS (info);
  rate = GST_AUDIO_INFO_RATE (info);
  width = GST_AUDIO_INFO_WIDTH (info);
  format = GST_AUDIO_INFO_FORMAT (info);

  bps = width / 8;

  if (rate != scaletempo->sample_rate
      || nch != scaletempo->samples_per_frame
      || bps != scaletempo->bytes_per_sample || format != scaletempo->format) {
    scaletempo->sample_rate = rate;
    scaletempo->samples_per_frame = nch;
    scaletempo->bytes_per_sample = bps;
    scaletempo->bytes_per_frame = nch * bps;
    scaletempo->format = format;
    scaletempo->reinit_buffers = TRUE;
  }

  return TRUE;
}

gboolean scaletempo_start (struct scale_tempo * scaletempo)
{
  scaletempo->reinit_buffers = TRUE;

  return TRUE;
}

gboolean scaletempo_stop (struct scale_tempo * scaletempo)
{
  g_free (scaletempo->buf_queue);
  scaletempo->buf_queue = NULL;
  g_free (scaletempo->buf_overlap);
  scaletempo->buf_overlap = NULL;
  g_free (scaletempo->table_blend);
  scaletempo->table_blend = NULL;
  g_free (scaletempo->buf_pre_corr);
  scaletempo->buf_pre_corr = NULL;
  g_free (scaletempo->table_window);
  scaletempo->table_window = NULL;
  scaletempo->reinit_buffers = TRUE;

  return TRUE;
}

void scaletempo_init (struct scale_tempo * scaletempo)
{
  /* defaults */
  scaletempo->ms_stride = 25;
  scaletempo->percent_overlap = .2;
  scaletempo->ms_search = 10;

  /* uninitialized */
  scaletempo->scale = 0;
  scaletempo->sample_rate = 0;
  scaletempo->frames_stride_error = 0;
  scaletempo->bytes_stride = 0;
  scaletempo->bytes_queued = 0;
  scaletempo->bytes_to_slide = 0;
  scaletempo->segment_start = 0;
}

gint scaletemp_get_stride (struct scale_tempo * scaletempo)
{
  return scaletempo->bytes_stride;
}
