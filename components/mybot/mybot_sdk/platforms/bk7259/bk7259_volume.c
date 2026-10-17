/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"

#include <bk_ef.h>
#include <api/aosl_thread.h>
#include "bk7259_platform_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_volume"

#define BK7259_VOLUME_CONTEXT_MAGIC 0x564f4c55u
#define BK7259_VOLUME_RECORD_MAGIC 0x4d42564cu
#define BK7259_VOLUME_RECORD_VERSION 1u
#define BK7259_VOLUME_RECORD_KEY "mybot.volume.v1"
#define BK7259_VOLUME_LEVEL_STEP 10
#define BK7259_VOLUME_LEVEL_COUNT 11

/* Keep the same perceptual ladder used by the BK7259 audio engine.  Level zero
 * is the DAC's hard-silence value; level seven is the product's default
 * (-7.68 dB, close to the speaker stream's -7 dB default). */
static const float s_volume_gain_db[BK7259_VOLUME_LEVEL_COUNT] = {
    -100.0f, -33.06f, -29.82f, -26.22f, -22.26f, -17.88f,
    -13.03f, -7.68f, -1.76f, 4.77f, 12.0f,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    int32_t volume;
} bk7259_volume_record_t;

typedef struct {
    uint32_t magic;
    bool active;
    int volume;
    bool persisted_volume_known;
    int persisted_volume;
    bool persist_dirty;
} bk7259_volume_context_t;

_Static_assert(sizeof(bk7259_volume_record_t) == 12, "volume record size");

static bk7259_volume_context_t s_volume;
static aosl_static_lock_t s_volume_lock = AOSL_STATIC_LOCK_INIT;

static float float_abs(float value) {
    return value < 0.0f ? -value : value;
}

static float volume_to_gain_db(int volume) {
    if (volume <= MYBOT_AUDIO_VOLUME_MIN) {
        return s_volume_gain_db[0];
    }
    if (volume >= MYBOT_AUDIO_VOLUME_MAX) {
        return s_volume_gain_db[BK7259_VOLUME_LEVEL_COUNT - 1];
    }

    int level = volume / BK7259_VOLUME_LEVEL_STEP;
    int remainder = volume % BK7259_VOLUME_LEVEL_STEP;
    float lower = s_volume_gain_db[level];
    float upper = s_volume_gain_db[level + 1];
    return lower + (upper - lower) * ((float)remainder / BK7259_VOLUME_LEVEL_STEP);
}

static int gain_db_to_volume(float gain_db) {
    int closest_level = 0;
    float closest_distance = float_abs(gain_db - s_volume_gain_db[0]);

    if (gain_db != gain_db) {
        return 0;
    }

    for (int level = 1; level < BK7259_VOLUME_LEVEL_COUNT; ++level) {
        float distance = float_abs(gain_db - s_volume_gain_db[level]);
        if (distance < closest_distance) {
            closest_distance = distance;
            closest_level = level;
        }
    }
    return closest_level * BK7259_VOLUME_LEVEL_STEP;
}

static bool volume_context_is_valid(const bk7259_volume_context_t *ctx) {
    return ctx == &s_volume && ctx->magic == BK7259_VOLUME_CONTEXT_MAGIC && ctx->active;
}

static bool load_persisted_volume(int *volume) {
    uint8_t raw[sizeof(bk7259_volume_record_t) + 1] = {0};
    bk7259_volume_record_t record;
    int length;

    if (!volume) {
        return false;
    }

    length = bk_get_env_enhance(BK7259_VOLUME_RECORD_KEY, raw, sizeof(raw));
    if (length == 0) {
        MYBOT_LOGI(TAG, "no persisted volume; using speaker default");
        return false;
    }
    if (length < 0 || length != (int)sizeof(record)) {
        MYBOT_LOGW(TAG, "ignored persisted volume with invalid length=%d", length);
        return false;
    }

    memcpy(&record, raw, sizeof(record));
    if (record.magic != BK7259_VOLUME_RECORD_MAGIC ||
        record.version != BK7259_VOLUME_RECORD_VERSION ||
        record.record_size != sizeof(record) ||
        record.volume < MYBOT_AUDIO_VOLUME_MIN || record.volume > MYBOT_AUDIO_VOLUME_MAX) {
        MYBOT_LOGW(TAG, "ignored invalid persisted volume record");
        return false;
    }

    *volume = record.volume;
    MYBOT_LOGI(TAG, "loaded persisted volume=%d", *volume);
    return true;
}

static int save_volume(int volume) {
    const bk7259_volume_record_t record = {
        .magic = BK7259_VOLUME_RECORD_MAGIC,
        .version = BK7259_VOLUME_RECORD_VERSION,
        .record_size = sizeof(record),
        .volume = volume,
    };

    EfErrCode result = bk_set_env_enhance(BK7259_VOLUME_RECORD_KEY, &record, sizeof(record));
    if (result != EF_NO_ERR) {
        MYBOT_LOGW(TAG, "failed to persist volume=%d (err=%d)", volume, (int)result);
        return -1;
    }
    MYBOT_LOGI(TAG, "persisted volume=%d", volume);
    return 0;
}

static void adopt_persisted_volume(int volume) {
    s_volume.volume = volume;
    s_volume.persisted_volume = volume;
    s_volume.persisted_volume_known = true;
    s_volume.persist_dirty = false;
}

static int volume_init(void **out_ctx) {
    int persisted_volume = 0;
    float gain_db = 0.0f;
    int ready_volume = 0;
    bool ready_persisted = false;

    if (!out_ctx) {
        MYBOT_LOGE(TAG, "volume init rejected: invalid output context");
        return -1;
    }
    *out_ctx = NULL;
    MYBOT_LOGI(TAG, "volume init requested");
    if (aosl_static_lock_lock(&s_volume_lock) < 0) {
        MYBOT_LOGE(TAG, "volume init lock failed");
        return -1;
    }

    if (s_volume.active) {
        MYBOT_LOGW(TAG, "volume init rejected: already active");
        (void)aosl_static_lock_unlock(&s_volume_lock);
        return -1;
    }

    if (!s_volume.persisted_volume_known && load_persisted_volume(&persisted_volume)) {
        adopt_persisted_volume(persisted_volume);
    }

    if (s_volume.persisted_volume_known) {
        s_volume.volume = s_volume.persisted_volume;
        gain_db = volume_to_gain_db(s_volume.volume);
        if (bk7259_audio_playback_gain_set(gain_db) < 0) {
            MYBOT_LOGE(TAG, "volume restore failed: volume=%d gain=%.2f dB", s_volume.volume,
                    gain_db);
            (void)aosl_static_lock_unlock(&s_volume_lock);
            return -1;
        }
    } else {
        if (bk7259_audio_playback_gain_get(&gain_db) < 0) {
            MYBOT_LOGE(TAG, "volume init failed: unable to read speaker gain");
            (void)aosl_static_lock_unlock(&s_volume_lock);
            return -1;
        }
        s_volume.volume = gain_db_to_volume(gain_db);
    }

    s_volume.magic = BK7259_VOLUME_CONTEXT_MAGIC;
    s_volume.active = true;
    *out_ctx = &s_volume;
    ready_volume = s_volume.volume;
    ready_persisted = s_volume.persisted_volume_known;
    (void)aosl_static_lock_unlock(&s_volume_lock);
    MYBOT_LOGI(TAG, "volume ready: level=%d gain=%.2f dB persisted=%s", ready_volume,
            gain_db, ready_persisted ? "yes" : "no");
    return 0;
}

static int volume_set(void *opaque, int requested_volume) {
    int result = -1;

    if (aosl_static_lock_lock(&s_volume_lock) < 0) {
        MYBOT_LOGE(TAG, "volume set lock failed");
        return -1;
    }
    if (!volume_context_is_valid(opaque) || requested_volume < MYBOT_AUDIO_VOLUME_MIN ||
        requested_volume > MYBOT_AUDIO_VOLUME_MAX) {
        MYBOT_LOGW(TAG, "volume set rejected: level=%d", requested_volume);
        (void)aosl_static_lock_unlock(&s_volume_lock);
        return -1;
    }

    float gain_db = volume_to_gain_db(requested_volume);
    if (bk7259_audio_playback_gain_set(gain_db) == 0) {
        s_volume.volume = requested_volume;
        if (!s_volume.persisted_volume_known || s_volume.persisted_volume != requested_volume ||
            s_volume.persist_dirty) {
            if (save_volume(requested_volume) == 0) {
                s_volume.persisted_volume = requested_volume;
                s_volume.persisted_volume_known = true;
                s_volume.persist_dirty = false;
            } else {
                s_volume.persist_dirty = true;
            }
        }
        result = 0;
        MYBOT_LOGI(TAG, "volume set: level=%d gain=%.2f dB", requested_volume, gain_db);
    } else {
        MYBOT_LOGE(TAG, "volume hardware update failed: level=%d gain=%.2f dB",
                requested_volume, gain_db);
    }

    (void)aosl_static_lock_unlock(&s_volume_lock);
    return result;
}

static int volume_get(void *opaque, int *current_volume) {
    float gain_db;

    if (aosl_static_lock_lock(&s_volume_lock) < 0) {
        MYBOT_LOGE(TAG, "volume get lock failed");
        return -1;
    }
    if (!volume_context_is_valid(opaque) || !current_volume ||
        bk7259_audio_playback_gain_get(&gain_db) < 0) {
        MYBOT_LOGW(TAG, "volume get unavailable");
        (void)aosl_static_lock_unlock(&s_volume_lock);
        return -1;
    }
    *current_volume = s_volume.volume;
    (void)aosl_static_lock_unlock(&s_volume_lock);
    return 0;
}

static void volume_destroy(void *opaque) {
    MYBOT_LOGI(TAG, "volume destroy requested");
    if (aosl_static_lock_lock(&s_volume_lock) < 0) {
        MYBOT_LOGE(TAG, "volume destroy lock failed");
        return;
    }
    if (!volume_context_is_valid(opaque)) {
        MYBOT_LOGW(TAG, "volume destroy ignored: invalid context");
        (void)aosl_static_lock_unlock(&s_volume_lock);
        return;
    }

    /* Playback is torn down before this callback by the SDK.  Persist only;
     * never access the speaker handle after playback_destroy(). */
    if (s_volume.persist_dirty && save_volume(s_volume.volume) == 0) {
        s_volume.persisted_volume = s_volume.volume;
        s_volume.persisted_volume_known = true;
        s_volume.persist_dirty = false;
    }
    s_volume.active = false;
    (void)aosl_static_lock_unlock(&s_volume_lock);
    MYBOT_LOGI(TAG, "volume destroyed");
}

const mybot_audio_volume_ops_t g_mybot_bk7259_volume_ops = {
    .init = volume_init,
    .set_volume = volume_set,
    .get_volume = volume_get,
    .destroy = volume_destroy,
};
