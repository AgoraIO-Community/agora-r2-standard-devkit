/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_announce.h"

#include "bk7259_assets.h"
#include "bk7259_ogg_pcm.h"
#include <mybot_bk7259_platform.h>

#include "bk7259_platform_log.h"
#include <os/mem.h>
#include <api/aosl_thread.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "mybot_announce"
#define ANNOUNCE_RATE_HZ 16000
/* Pairing prompts are short; this bound also prevents a malformed asset from
 * consuming an unbounded amount of PSRAM. */
#define ANNOUNCE_MAX_FRAMES (512U * 1024U / sizeof(int16_t))
#define ANNOUNCE_MAX_TOTAL_PCM_BYTES (1024U * 1024U)
#define ANNOUNCE_PATH_CAPACITY 96U

typedef struct {
    uint8_t reserved;
} bk7259_announce_context_t;

typedef struct {
    int16_t *pcm;
    int frames;
    int offset;
    size_t pcm_bytes;
} bk7259_announce_sound_t;

static aosl_static_lock_t s_announce_memory_lock = AOSL_STATIC_LOCK_INIT;
static size_t s_announce_pcm_bytes;

static const char *sound_file_name(mybot_announce_sound_t sound) {
    switch (sound) {
    case MYBOT_ANNOUNCE_SOUND_PROMPT:
        return "prompt.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_0:
        return "0.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_1:
        return "1.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_2:
        return "2.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_3:
        return "3.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_4:
        return "4.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_5:
        return "5.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_6:
        return "6.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_7:
        return "7.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_8:
        return "8.ogg";
    case MYBOT_ANNOUNCE_SOUND_DIGIT_9:
        return "9.ogg";
    default:
        return NULL;
    }
}

static void release_sound(bk7259_announce_sound_t *sound) {
    if (!sound) {
        return;
    }
    if (sound->pcm) {
        psram_free(sound->pcm);
    }
    if (sound->pcm_bytes != 0 &&
        aosl_static_lock_lock(&s_announce_memory_lock) == 0) {
        if (s_announce_pcm_bytes >= sound->pcm_bytes) {
            s_announce_pcm_bytes -= sound->pcm_bytes;
        } else {
            s_announce_pcm_bytes = 0;
        }
        (void)aosl_static_lock_unlock(&s_announce_memory_lock);
    }
    psram_free(sound);
}

static int announce_init(void **out_ctx) {
    bk7259_announce_context_t *context;

    if (!out_ctx) {
        return -1;
    }
    *out_ctx = NULL;
    context = psram_zalloc(sizeof(*context));
    if (!context) {
        MYBOT_LOGE(TAG, "announcement context allocation failed");
        return -1;
    }
    *out_ctx = context;
    MYBOT_LOGI(TAG, "embedded prompts ready: %s/%s", MYBOT_ASSETS_DIR,
            MYBOT_LANGUAGE_TAG);
    return 0;
}

static void *announce_open(void *opaque, mybot_announce_sound_t sound) {
    bk7259_announce_context_t *context = opaque;
    bk7259_announce_sound_t *handle = NULL;
    bk7259_asset_t asset = {0};
    bk7259_ogg_pcm_t decoded = {0};
    const char *file_name = sound_file_name(sound);
    char path[ANNOUNCE_PATH_CAPACITY];
    int path_length;
    bool memory_locked;

    if (!context || !file_name) {
        return NULL;
    }

    path_length = snprintf(path, sizeof(path), "%s/locales/%s/%s", MYBOT_ASSETS_DIR,
                           MYBOT_LANGUAGE_TAG, file_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        bk7259_asset_find(path, &asset) < 0) {
        MYBOT_LOGW(TAG, "announcement asset unavailable: %s", file_name);
        return NULL;
    }
    if (bk7259_ogg_pcm_load_memory(path, asset.data, asset.size, ANNOUNCE_RATE_HZ,
                                   &decoded) < 0 || decoded.frames <= 0 ||
        (size_t)decoded.frames > ANNOUNCE_MAX_FRAMES) {
        MYBOT_LOGW(TAG, "announcement decode failed or out of range: %s", path);
        bk7259_ogg_pcm_free(&decoded);
        return NULL;
    }

    handle = psram_zalloc(sizeof(*handle));
    if (!handle) {
        MYBOT_LOGE(TAG, "announcement handle allocation failed: %s", path);
        bk7259_ogg_pcm_free(&decoded);
        return NULL;
    }
    handle->pcm_bytes = (size_t)decoded.frames * sizeof(int16_t);
    memory_locked = aosl_static_lock_lock(&s_announce_memory_lock) == 0;
    if (!memory_locked || handle->pcm_bytes > ANNOUNCE_MAX_TOTAL_PCM_BYTES ||
        s_announce_pcm_bytes > ANNOUNCE_MAX_TOTAL_PCM_BYTES - handle->pcm_bytes) {
        if (memory_locked) {
            (void)aosl_static_lock_unlock(&s_announce_memory_lock);
        }
        MYBOT_LOGW(TAG, "announcement PCM budget exceeded: %s", path);
        psram_free(handle);
        bk7259_ogg_pcm_free(&decoded);
        return NULL;
    }
    s_announce_pcm_bytes += handle->pcm_bytes;
    (void)aosl_static_lock_unlock(&s_announce_memory_lock);
    handle->pcm = decoded.pcm;
    handle->frames = decoded.frames;
    return handle;
}

static int announce_read(void *opaque, void *sound, int16_t *dst, int max_frames) {
    bk7259_announce_sound_t *handle = sound;
    int remaining;
    int frames;

    (void)opaque;
    if (!handle || !dst || max_frames <= 0) {
        return 0;
    }
    remaining = handle->frames - handle->offset;
    frames = remaining < max_frames ? remaining : max_frames;
    if (frames <= 0) {
        return 0;
    }
    memcpy(dst, handle->pcm + handle->offset, (size_t)frames * sizeof(*dst));
    handle->offset += frames;
    return frames;
}

static void announce_close(void *opaque, void *sound) {
    (void)opaque;
    release_sound(sound);
}

static void announce_destroy(void *opaque) {
    if (opaque) {
        psram_free(opaque);
    }
}

const mybot_announce_ops_t g_mybot_bk7259_announce_ops = {
    .init = announce_init,
    .open = announce_open,
    .read = announce_read,
    .close = announce_close,
    .destroy = announce_destroy,
};
