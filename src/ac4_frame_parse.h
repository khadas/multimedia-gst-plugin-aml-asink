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
#ifndef AC4_FRAME_PARSE_H_
#define AC4_FRAME_PARSE_H_

#include <stdint.h>

struct ac4_info {
    /* 44100 or 48000 */
    uint16_t frame_rate;
    uint16_t samples_per_frame;
    uint16_t seq_cnt;
    uint8_t  sync_frame;
};

int ac4_toc_parse(uint8_t* data, int32_t len, struct ac4_info* info);
uint8_t * ac4_syncframe_header(int32_t len, int32_t *header_len);

#endif
