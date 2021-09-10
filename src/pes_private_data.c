/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: refer to ISO/IEC 13818-1:2007, ETSI TS 101 154 v2.4.1
 */
#include <gst/gstinfo.h>
#include <stdbool.h>
#include <stdio.h>
#include "pes_private_data.h"

GST_DEBUG_CATEGORY_EXTERN(gst_aml_hal_asink_debug_category);
#define GST_CAT_DEFAULT gst_aml_hal_asink_debug_category

static const uint8_t* get_private_data(const uint8_t *header, int size)
{
  uint8_t stream_id;
  //uint16_t len;
  uint8_t flags;
  uint8_t offset;

  if (size < 8)
    return NULL;
  if (header[0] != 0 || header[1] != 0 || header[2] != 1)
    return NULL;
  stream_id = header[3];
  //len = (header[4] << 8 | header[5]);

  if (stream_id == 0xbc || stream_id == 0xbf || /* program_stream_map, private_stream_2 */
      stream_id == 0xBE || /* padding_stream */
      stream_id == 0xf0 || stream_id == 0xf1 || /* ECM, EMM */
      stream_id == 0xff || stream_id == 0xf2 || /* program_stream_directory, DSMCC_stream */
      stream_id == 0xf8) /* ITU-T Rec. H.222.1 type E stream */
    return NULL;

  flags = header[7];
  if (!(flags & 0x1))
    return NULL;

  offset = 9; /* skip the pes header data length */
  if ((flags & 0xc0) == 0x80)
    offset += 5;
  else if ((flags & 0xc0) == 0xc0)
    offset += 10;

  if (flags & 0x20)
    offset += 6;
  if (flags & 0x10)
    offset += 3;
  if (flags & 0x8)
    offset += 1;
  if (flags & 0x4)
    offset += 1;
  if (flags & 0x2)
    offset += 2;

  offset += 1; /* skip private header */
  if (offset > size)
    return NULL;

  /* no PES_private_data_flag */
  if (!(header[offset] & 0x80))
    return NULL;

  if (offset + 16 > size) {
    GST_ERROR("pes too short %d/%d", offset, size);
    return NULL;
  }
  return header + offset;
}


int pes_get_ad_des(const uint8_t *header, int size, struct ad_des *des)
{
  const uint8_t* p_ad_des;
  int len;

  if (!header || !des)
    return -1;

  p_ad_des = get_private_data(header, size);

  if (!p_ad_des)
    return -2;

  if ((p_ad_des[0] & 0xF0) != 0xF0)
    return -3;
  len = p_ad_des[0] & 0xF;
  if (len != 8 && len != 11) {
    GST_LOG("wrong AD_descriptor_length %d", len);
    return -4;
  }
  if (p_ad_des[1] != 0x44 || p_ad_des[2] != 0x54 ||
      p_ad_des[3] != 0x47 || p_ad_des[4] != 0x41 ||
      p_ad_des[5] != 0x44) {
    GST_LOG("wrong AD_text_tag");
    return -5;
  }
  des->version = p_ad_des[6];
  des->fade = p_ad_des[7];
  des->pan = p_ad_des[8];
  if (des->version == 0x32) {
    des->g_c = p_ad_des[9];
    des->g_f = p_ad_des[10];
    des->g_s = p_ad_des[11];
  }
  return 0;
}
