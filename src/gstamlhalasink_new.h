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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


#ifndef __GST_AUDIO_AUDIO_H__
#include <gst/audio/audio.h>
#endif

#ifndef _GST_AML_HAL_ASINK_H_
#define _GST_AML_HAL_ASINK_H_

#include <gst/base/gstbasesink.h>

G_BEGIN_DECLS

#define GST_TYPE_AML_HAL_ASINK   (gst_aml_hal_asink_get_type())
#define GST_AML_HAL_ASINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AML_HAL_ASINK,GstAmlHalAsink))
#define GST_AML_HAL_ASINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AML_HAL_ASINK,GstAmlHalAsinkClass))
#define GST_IS_AML_HAL_ASINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AML_HAL_ASINK))
#define GST_IS_AML_HAL_ASINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AML_HAL_ASINK))

#define GST_AML_HAL_ASINK_CLOCK(obj)   (GST_TYPE_AML_HAL_ASINK(obj)->clock)
#define GST_AML_HAL_ASINK_PAD(obj)     (GST_BASE_SINK (obj)->sinkpad)

typedef struct _GstAmlHalAsink GstAmlHalAsink;
typedef struct _GstAmlHalAsinkClass GstAmlHalAsinkClass;
typedef struct _GstAmlHalAsinkPrivate GstAmlHalAsinkPrivate;

struct _GstAmlHalAsink {
  GstBaseSink         element;
  /* clock */
  GstClock           *provided_clock;
  /*< private >*/
  GstAmlHalAsinkPrivate *priv;
};

struct _GstAmlHalAsinkClass {
  GstBaseSinkClass     parent_class;
};

GType gst_aml_hal_asink_get_type (void);

G_END_DECLS

#endif
