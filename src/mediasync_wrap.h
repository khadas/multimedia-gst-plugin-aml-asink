/*
 * Copyright (C) 2022 Amlogic Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */



#ifndef _MEDIASYNC_WRAP_H_
#define _MEDIASYNC_WRAP_H_
#include <stdbool.h>

typedef enum {
    AM_MEDIASYNC_OK  = 0,                      // OK
    AM_MEDIASYNC_ERROR_INVALID_PARAMS = -1,    // Parameters invalid
    AM_MEDIASYNC_ERROR_INVALID_OPERATION = -2, // Operation invalid
    AM_MEDIASYNC_ERROR_INVALID_OBJECT = -3,    // Object invalid
    AM_MEDIASYNC_ERROR_RETRY = -4,             // Retry
    AM_MEDIASYNC_ERROR_BUSY = -5,              // Device busy
    AM_MEDIASYNC_ERROR_END_OF_STREAM = -6,     // End of stream
    AM_MEDIASYNC_ERROR_IO            = -7,     // Io error
    AM_MEDIASYNC_ERROR_WOULD_BLOCK   = -8,     // Blocking error
    AM_MEDIASYNC_ERROR_MAX = -254
} mediasync_result;

typedef enum {
    MEDIA_VIDEO = 0,
    MEDIA_AUDIO = 1,
    MEDIA_DMXPCR = 2,
    MEDIA_SUBTITLE = 3,
    MEDIA_COMMON = 4,
    MEDIA_TYPE_MAX = 255,
}sync_stream_type;

typedef enum {
    MEDIASYNC_UNIT_MS = 0,
    MEDIASYNC_UNIT_US,
    MEDIASYNC_UNIT_PTS,
    MEDIASYNC_UNIT_MAX,
} mediasync_time_unit;

typedef enum {
    MEDIA_VIDEO_TIME = 0,
    MEDIA_AUDIO_TIME = 1,
    MEDIA_DMXPCR_TIME = 2,
    MEDIA_STC_TIME = 3,
    MEDIA_TIME_TYPE_MAX = 255,
} media_time_type;

void* mediasync_wrap_create();

int mediasync_wrap_allocInstance(void* handle, int32_t DemuxId,
        int32_t PcrPid,
        int32_t *SyncInsId);

int mediasync_wrap_bindInstance(void* handle, uint32_t SyncInsId, sync_stream_type streamtype);
int mediasync_wrap_setPlaybackRate(void* handle, float rate);
int mediasync_wrap_getPlaybackRate(void* handle, float *rate);
int mediasync_wrap_getMediaTime(void* handle, int64_t realUs, int64_t *outMediaUs, bool allowPastMaxTime);
int mediasync_wrap_getRealTimeFor(void* handle, int64_t targetMediaUs, int64_t *outRealUs);
int mediasync_wrap_getRealTimeForNextVsync(void* handle, int64_t *outRealUs);
int mediasync_wrap_getTrackMediaTime(void* handle, int64_t *outMeidaUs);

int mediasync_wrap_GetMediaTimeByType(void* handle, media_time_type mediaTimeType, mediasync_time_unit tunit,int64_t* mediaTime);
int MediaSync_wrap_audioSwitch(void* handle, bool start, int64_t pts);

int mediasync_wrap_reset(void* handle);
void mediasync_wrap_destroy(void* handle);
#endif
