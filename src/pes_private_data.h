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
#include <stdint.h>

struct ad_des {
  uint8_t version;
  uint8_t fade; /* 0 to 0xFE with unit 0.3dB, 0xFF mute */
  uint8_t pan; /* 0 to 256 with unit 360/256 degree clockwise */
  uint8_t g_c; /* gain center */
  uint8_t g_f; /* gain front */
  uint8_t g_s; /* gain surround */
};

int pes_get_ad_des(const uint8_t *header, int size, struct ad_des *des);
