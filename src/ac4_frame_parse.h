/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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
