/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal_prefs.h"

#include <api/aosl_thread.h>
#include <easyflash.h>
#include <stdint.h>
#include <string.h>

#define GIMBAL_PREFS_MAGIC 0x4d42474dU
#define GIMBAL_PREFS_VERSION 1U
#define GIMBAL_PREF_MOTION 1U
#define GIMBAL_PREF_CHARGE 2U
#define GIMBAL_PREFS_DEFAULT (GIMBAL_PREF_MOTION | GIMBAL_PREF_CHARGE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t inverse_flags;
} gimbal_prefs_record_t;

static aosl_static_lock_t s_prefs_lock = AOSL_STATIC_LOCK_INIT;
static bool s_writes_blocked;

/* The caller serializes preference access. Require valid EasyFlash metadata,
 * matching read lengths, and a complete known record before enabling features. */
static int read_prefs(uint32_t *flags)
{
    struct env_node_obj node = {0};
    if (!ef_get_env_obj(BK7259_GIMBAL_PREFS_KEY, &node)) {
        *flags = GIMBAL_PREFS_DEFAULT;
        return 0;
    }

    const size_t key_length = sizeof(BK7259_GIMBAL_PREFS_KEY) - 1U;
    gimbal_prefs_record_t saved = {0};
    size_t saved_length = 0;
    if (!node.crc_is_ok || node.status != ENV_WRITE || node.name_len != key_length ||
        memcmp(node.name, BK7259_GIMBAL_PREFS_KEY, key_length) != 0 ||
        node.value_len != sizeof(saved)) {
        return -1;
    }
    if (ef_get_env_blob(BK7259_GIMBAL_PREFS_KEY, &saved, sizeof(saved), &saved_length) !=
            sizeof(saved) || saved_length != sizeof(saved) ||
        saved.magic != GIMBAL_PREFS_MAGIC || saved.version != GIMBAL_PREFS_VERSION ||
        (saved.flags & ~GIMBAL_PREFS_DEFAULT) || saved.inverse_flags != ~saved.flags) {
        return -1;
    }
    *flags = saved.flags;
    return 1;
}

int bk7259_gimbal_prefs_load(bool *motion, bool *charge)
{
    if (!motion || !charge) {
        return -1;
    }
    *motion = false;
    *charge = false;
    if (aosl_static_lock_lock(&s_prefs_lock) < 0) {
        return -1;
    }
    uint32_t flags = 0;
    int result = easyflash_init() == EF_NO_ERR ? read_prefs(&flags) : -1;
    (void)aosl_static_lock_unlock(&s_prefs_lock);
    if (result < 0) {
        return -1;
    }
    *motion = (flags & GIMBAL_PREF_MOTION) != 0;
    *charge = (flags & GIMBAL_PREF_CHARGE) != 0;
    return 0;
}

int bk7259_gimbal_prefs_save(bool motion, bool charge)
{
    uint32_t wanted = (motion ? GIMBAL_PREF_MOTION : 0) |
                      (charge ? GIMBAL_PREF_CHARGE : 0);
    if (aosl_static_lock_lock(&s_prefs_lock) < 0) {
        return -1;
    }
    int result = -1;
    if (!s_writes_blocked && easyflash_init() == EF_NO_ERR) {
        uint32_t saved;
        if (read_prefs(&saved) >= 0 && saved == wanted) {
            result = 0;
        } else {
            const gimbal_prefs_record_t record = {
                .magic = GIMBAL_PREFS_MAGIC,
                .version = GIMBAL_PREFS_VERSION,
                .flags = wanted,
                .inverse_flags = ~wanted,
            };
            /* EasyFlash can report an error after the new record committed. */
            (void)ef_set_env_blob(BK7259_GIMBAL_PREFS_KEY, &record, sizeof(record));
            if (read_prefs(&saved) == 1 && saved == wanted) {
                result = 0;
            }
        }
    }
    (void)aosl_static_lock_unlock(&s_prefs_lock);
    return result;
}

int bk7259_gimbal_prefs_block_writes(void)
{
    if (aosl_static_lock_lock(&s_prefs_lock) < 0) {
        return -1;
    }
    s_writes_blocked = true;
    return aosl_static_lock_unlock(&s_prefs_lock);
}
