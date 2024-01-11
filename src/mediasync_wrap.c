/*
 * Copyright (C) 2010 Amlogic Corporation.
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

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>

#include <gst/gst.h>

#include "mediasync_wrap.h"


typedef void* (*MediaSync_create_func)(void);

typedef mediasync_result (*MediaSync_allocInstance_func)(void* handle, int32_t DemuxId,
                                                         int32_t PcrPid,
                                                         int32_t *SyncInsId);

typedef mediasync_result (*MediaSync_bindInstance_func)(void* handle, uint32_t SyncInsId,
                                                         sync_stream_type streamtype);

typedef mediasync_result (*MediaSync_setPlaybackRate_func)(void* handle, float rate);
typedef mediasync_result (*MediaSync_getPlaybackRate_func)(void* handle, float *rate);
typedef mediasync_result (*MediaSync_getMediaTime_func)(void* handle, int64_t realUs,
                                int64_t *outMediaUs,
                                bool allowPastMaxTime);
typedef mediasync_result (*MediaSync_getRealTimeFor_func)(void* handle, int64_t targetMediaUs, int64_t *outRealUs);
typedef mediasync_result (*MediaSync_getRealTimeForNextVsync_func)(void* handle, int64_t *outRealUs);
typedef mediasync_result (*MediaSync_getTrackMediaTime_func)(void* handle, int64_t *outMediaUs);

typedef mediasync_result (*MediaSync_GetMediaTimeByType)(void* handle, media_time_type mediaTimeType, mediasync_time_unit tunit,int64_t* mediaTime);
typedef mediasync_result (*MediaSync_audioSwitch)(void* handle, bool start, int64_t pts);
typedef mediasync_result (*MediaSync_reset_func)(void* handle);
typedef void (*MediaSync_destroy_func)(void* handle);

static MediaSync_create_func gMediaSync_create = NULL;
static MediaSync_allocInstance_func gMediaSync_allocInstance = NULL;
static MediaSync_bindInstance_func gMediaSync_bindInstance = NULL;
static MediaSync_setPlaybackRate_func gMediaSync_setPlaybackRate = NULL;
static MediaSync_getPlaybackRate_func gMediaSync_getPlaybackRate = NULL;
static MediaSync_getMediaTime_func gMediaSync_getMediaTime = NULL;
static MediaSync_getRealTimeFor_func gMediaSync_getRealTimeFor = NULL;
static MediaSync_getRealTimeForNextVsync_func gMediaSync_getRealTimeForNextVsync = NULL;
static MediaSync_getTrackMediaTime_func gMediaSync_getTrackMediaTime = NULL;
static MediaSync_GetMediaTimeByType gMediaSync_GetMediaTimeByType = NULL;
static MediaSync_audioSwitch gMediaSync_audioSwitch = NULL;
static MediaSync_reset_func gMediaSync_reset = NULL;
static MediaSync_destroy_func gMediaSync_destroy = NULL;

static void* glibHandle = NULL;
static int gMediasync_init = 0;

static bool mediasync_wrap_create_init()
{
    bool err = false;

    if (glibHandle == NULL) {
        glibHandle = dlopen("libmediahal_mediasync.so", RTLD_NOW);
        if (glibHandle == NULL) {
            GST_ERROR("unable to dlopen libmediahal_mediasync.so: %s", dlerror());
            return err;
        }
    }

    gMediaSync_create =
        (MediaSync_create_func)dlsym(glibHandle, "MediaSync_create");
    if (gMediaSync_create == NULL) {
        GST_ERROR("dlsym MediaSync_create failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_allocInstance =
        (MediaSync_allocInstance_func)dlsym(glibHandle, "MediaSync_allocInstance");
    if (gMediaSync_allocInstance == NULL) {
        GST_ERROR("dlsym MediaSync_allocInstance failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_bindInstance =
    (MediaSync_bindInstance_func)dlsym(glibHandle, "MediaSync_bindInstance");
    if (gMediaSync_bindInstance == NULL) {
        GST_ERROR("dlsym MediaSync_bindInstance failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_setPlaybackRate =
    (MediaSync_setPlaybackRate_func)dlsym(glibHandle, "MediaSync_setPlaybackRate");
    if (gMediaSync_setPlaybackRate == NULL) {
        GST_ERROR("dlsym MediaSync_setPlaybackRate failed, err=%s \n", dlerror());
        return err;
    }
    gMediaSync_getPlaybackRate =
    (MediaSync_getPlaybackRate_func)dlsym(glibHandle, "MediaSync_getPlaybackRate");
    if (gMediaSync_getPlaybackRate == NULL) {
        GST_ERROR("dlsym MediaSync_getPlaybackRate failed, err=%s \n", dlerror());
        return err;
    }
    gMediaSync_getMediaTime =
        (MediaSync_getMediaTime_func)dlsym(glibHandle, "MediaSync_getMediaTime");
    if (gMediaSync_getMediaTime == NULL) {
        GST_ERROR("dlsym MediaSync_getMediaTime failed, err=%s \n", dlerror());
        return err;
    }
    gMediaSync_getRealTimeFor =
    (MediaSync_getRealTimeFor_func)dlsym(glibHandle, "MediaSync_getRealTimeFor");
    if (gMediaSync_getRealTimeFor == NULL) {
        GST_ERROR("dlsym MediaSync_getRealTimeFor failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_getRealTimeForNextVsync =
    (MediaSync_getRealTimeForNextVsync_func)dlsym(glibHandle, "MediaSync_getRealTimeForNextVsync");
    if (gMediaSync_getRealTimeForNextVsync == NULL) {
        GST_ERROR("dlsym MediaSync_getRealTimeForNextVsync failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_reset =
    (MediaSync_reset_func)dlsym(glibHandle, "MediaSync_reset");
    if (gMediaSync_reset == NULL) {
        GST_ERROR("dlsym MediaSync_reset failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_destroy =
    (MediaSync_destroy_func)dlsym(glibHandle, "MediaSync_destroy");
    if (gMediaSync_destroy == NULL) {
        GST_ERROR("dlsym MediaSync_destroy failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_getTrackMediaTime =
    (MediaSync_getTrackMediaTime_func)dlsym(glibHandle, "MediaSync_getTrackMediaTime");
    if (gMediaSync_getTrackMediaTime == NULL) {
        GST_ERROR("dlsym MediaSync_destroy failed, err=%s \n", dlerror());
        return err;
    }

    gMediaSync_GetMediaTimeByType =
    (MediaSync_GetMediaTimeByType)dlsym(glibHandle, "MediaSync_GetMediaTimeByType");
    if (gMediaSync_GetMediaTimeByType == NULL) {
        GST_ERROR("dlsym MediaSync_AudioProcess failed, err=%s\n", dlerror());
        return err;
    }

    gMediaSync_audioSwitch =
    (MediaSync_audioSwitch)dlsym(glibHandle, "MediaSync_audioSwitch");
    if (gMediaSync_audioSwitch == NULL) {
        GST_ERROR("dlsym MediaSync_audioSwitch failed, err=%s\n", dlerror());
        return err;
    }

    gMediasync_init = 1;
    return true;
}

void* mediasync_wrap_create() {
    bool ret = mediasync_wrap_create_init();
    if (!ret) {
        return NULL;
    }
    return gMediaSync_create();
}


int mediasync_wrap_allocInstance(void* handle, int32_t DemuxId,
        int32_t PcrPid,
        int32_t *SyncInsId) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_allocInstance(handle, DemuxId, PcrPid, SyncInsId);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return -1;
}

int mediasync_wrap_bindInstance(void* handle, uint32_t SyncInsId,
                                sync_stream_type streamtype) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_bindInstance(handle, SyncInsId, streamtype);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return -1;
}

int mediasync_wrap_setPlaybackRate(void* handle, float rate) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_setPlaybackRate(handle, rate);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return false;
}
int mediasync_wrap_getPlaybackRate(void* handle, float *rate) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_getPlaybackRate(handle, rate);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return false;
}
int mediasync_wrap_getMediaTime(void* handle, int64_t realUs,
                                int64_t *outMediaUs,
                                bool allowPastMaxTime) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_getMediaTime(handle, realUs, outMediaUs, allowPastMaxTime);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return -1;
}
int mediasync_wrap_getRealTimeFor(void* handle, int64_t targetMediaUs, int64_t *outRealUs) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_getRealTimeFor(handle, targetMediaUs, outRealUs);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return -1;
}
int mediasync_wrap_getRealTimeForNextVsync(void* handle, int64_t *outRealUs) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_getRealTimeForNextVsync(handle, outRealUs);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }
     return -1;
}
int mediasync_wrap_getTrackMediaTime(void* handle, int64_t *outMeidaUs) {
     if (handle != NULL && gMediasync_init) {
         mediasync_result ret = gMediaSync_getTrackMediaTime(handle, outMeidaUs);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }

     return -1;
}

int mediasync_wrap_GetMediaTimeByType(void* handle, media_time_type mediaTimeType,
                                            mediasync_time_unit tunit,int64_t* mediaTime) {
    if (handle != NULL && gMediasync_init) {
        mediasync_result ret = gMediaSync_GetMediaTimeByType(handle, mediaTimeType, tunit, mediaTime);
        if (ret == AM_MEDIASYNC_OK) {
           return 0;
        }
    }

    return -1;
}

int mediasync_wrap_reset(void* handle) {
     if (handle != NULL && gMediasync_init)  {
         mediasync_result ret = gMediaSync_reset(handle);
         if (ret == AM_MEDIASYNC_OK) {
            return 0;
         }
     }

     return -1;
}

void mediasync_wrap_destroy(void* handle) {
     if (handle != NULL && gMediasync_init)  {
         gMediaSync_destroy(handle);
     } else {
        GST_ERROR("[%s] no handle\n", __func__);
     }
}

int MediaSync_wrap_audioSwitch(void* handle, bool start, int64_t pts) {
    if (handle != NULL && gMediaSync_audioSwitch)  {
         gMediaSync_audioSwitch(handle, start, pts);
     } else {
        GST_ERROR("[%s] no handle\n", __func__);
     }
}
