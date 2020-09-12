/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "ac4_frame_parse.h"

#define MAX_FRAME_RATE_INDEX 13
/* for 48K output */
static uint16_t table_83[14] =
{
    1920,
    1920,
    2048,
    1536,
    1536,
    960,
    960,
    1024,
    768,
    768,
    512,
    384,
    384,
    2048
};

static uint8_t syncframe_header[7] =
{0xac, 0x40, 0xff, 0xff, 0, 0, 0};

static uint16_t read_bit(int32_t start, int32_t len, uint8_t* data, int32_t data_len)
{
    int start_byte;
    int cur_bit_pos;
    uint16_t ret = 0;

    if (len > 16) {
        printf("%s %d too long\n", __func__, __LINE__);
        return 0;
    }
    if ((start + len) > data_len * 2) {
        printf("%s %d read too much\n", __func__, __LINE__);
        return 0;
    }
    start_byte = start/8;
    cur_bit_pos = start%8;
    data += start_byte;
    while (len > 0) {
        uint8_t val = *data;
        uint8_t bits_left = 8 - cur_bit_pos;

        /* clear high bits */
        if (cur_bit_pos) {
            val <<= cur_bit_pos;
            val >>= cur_bit_pos;
        }

        if (bits_left > len) {
            /* shift out low bits */
            val >>= (bits_left - len);
            ret <<= len;
            ret |= val;
            break;
        } else {
            ret <<= bits_left;
            ret |= val;
            len -= bits_left;
            /* byte aligned now */
            data++;
            cur_bit_pos = 0;
        }
    }
    return ret;
}

int ac4_toc_parse (uint8_t* data, int32_t len, struct ac4_info* info)
{
    int32_t bit_off = 0;
    uint8_t bitstream_version;
    uint16_t sequence_count;
    uint8_t b_wait_frames;
    uint8_t wait_frames;
    uint8_t fs_index;
    uint8_t frame_rate_index;

    if (len < 10) {
        printf("%s %d frame too short %d\n",
            __func__, __LINE__, len);
        return -1;
    }

    /* syncframe Annex G.2 */
    if (data[0] == 0xac && (data[1] == 0x40 || data[1] == 0x41)) {
        data += 2;
        len -= 2;
        if (data[0] == 0xff && data[1] == 0xff) {
            data += 2;
            len = data[0] << 16 | data[1] << 8 | data[2];
            data += 3;
        } else {
            len = data[0] << 8 | data[1];
            data += 2;
        }
        info->sync_frame = 1;
    } else {
        info->sync_frame = 0;
    }

    /* raw ac4 frame 4.2.1 */
    bitstream_version = read_bit(bit_off, 2, data, len);
    if (bitstream_version != 0 && bitstream_version != 1) {
        printf("%s %d wrong bitstream_version %d\n",
            __func__, __LINE__, bitstream_version);
        //return -1;
    }
    bit_off += 2;

    sequence_count = read_bit(bit_off, 10, data, len);
    info->seq_cnt = sequence_count;
    bit_off += 10;

    b_wait_frames = read_bit(bit_off, 1, data, len);
    bit_off += 1;

    if (b_wait_frames) {
        wait_frames = read_bit(bit_off, 3, data, len);
        bit_off += 3;
        /* skip 2 bits reserved */
        if (wait_frames)
            bit_off += 2;
    }

    fs_index = read_bit(bit_off, 1, data, len);
    bit_off += 1;
    if (!fs_index)
        info->frame_rate = 44100;
    else
        info->frame_rate = 48000;

    frame_rate_index = read_bit(bit_off, 4, data, len);
    bit_off += 4;

    if (frame_rate_index > MAX_FRAME_RATE_INDEX) {
        printf("%s %d wrong frame_rate_index %d for 48K\n",
            __func__, __LINE__, frame_rate_index);
        return -1;
    }
    if (info->frame_rate == 44100) {
        if (frame_rate_index != MAX_FRAME_RATE_INDEX) {
            printf("%s %d wrong frame_rate_index %d for 44.1K\n",
                    __func__, __LINE__, frame_rate_index);
            return -1;
        } else
            info->samples_per_frame = 2048;
    } else
        info->samples_per_frame = table_83[frame_rate_index];

    return 0;
}


uint8_t * ac4_syncframe_header(int32_t len, int32_t *header_len)
{
    syncframe_header[4] = (len >> 16) & 0xff;
    syncframe_header[5] = (len >> 8) & 0xff;
    syncframe_header[6] = len & 0xff;
    *header_len = 7;

    return syncframe_header;
}
