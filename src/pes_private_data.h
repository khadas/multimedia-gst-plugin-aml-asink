/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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
