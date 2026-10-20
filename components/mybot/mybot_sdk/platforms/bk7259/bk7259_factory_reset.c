/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_factory_reset.h"
#include "bk7259_wifi_internal.h"
#include "bk7259_platform_log.h"

#include <api/aosl_atomic.h>
#include <easyflash.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_reset"
#define RESET_INTENT_KEY "mybot.factory_reset.v1"
#define RESET_INTENT_MAGIC 0x4d425246U
#define RESET_INTENT_VERSION 1U

/* The UI can post before SDK attachment. Only the product owner consumes;
 * network setup observes the same request without clearing cancellation. */
static aosl_atomic_t s_factory_reset_requested;

void bk7259_factory_reset_request(void) {
    aosl_atomic_set(&s_factory_reset_requested, 1);
}

bool bk7259_factory_reset_requested(void) {
    return aosl_atomic_read(&s_factory_reset_requested) != 0;
}

bool bk7259_factory_reset_take_request(void) {
    return aosl_atomic_xchg(&s_factory_reset_requested, 0) != 0;
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t inverse_magic;
} reset_intent_t;

static const reset_intent_t s_reset_intent = {
    .magic = RESET_INTENT_MAGIC,
    .version = RESET_INTENT_VERSION,
    .inverse_magic = ~RESET_INTENT_MAGIC,
};

/* Only the product application task calls these functions. The SDK and
 * provisioning writers must finish before clear; no credentials are read. */
static int record_present(const char *key, struct env_node_obj *record) {
    memset(record, 0, sizeof(*record));
    if (!ef_get_env_obj(key, record)) {
        return 0;
    }
    /* EasyFlash's cached lookup can succeed even when the flash node is
     * invalid. Do not trust that result without validating its metadata. */
    size_t key_length = strlen(key);
    if (!record->crc_is_ok || record->status != ENV_WRITE ||
        record->name_len != key_length ||
        memcmp(record->name, key, key_length) != 0) {
        MYBOT_LOGE(TAG, "invalid storage metadata for %s", key);
        return -1;
    }
    return 1;
}

static int read_intent(void) {
    struct env_node_obj node;
    int present = record_present(RESET_INTENT_KEY, &node);
    if (present != 1) {
        return present;
    }
    if (node.value_len != sizeof(reset_intent_t)) {
        MYBOT_LOGE(TAG, "invalid reset intent length");
        return -1;
    }

    reset_intent_t saved;
    /* The vendor port ignores low-level flash errors. Pre-fill the buffer and
     * verify the actual stored bytes, not just the reported read length. */
    memset(&saved, 0, sizeof(saved));
    size_t saved_length = 0;
    size_t read_length = ef_get_env_blob(RESET_INTENT_KEY, &saved, sizeof(saved),
                                       &saved_length);
    if (read_length != sizeof(saved) || saved_length != sizeof(saved) ||
        saved.magic != s_reset_intent.magic ||
        saved.version != s_reset_intent.version ||
        saved.inverse_magic != s_reset_intent.inverse_magic) {
        MYBOT_LOGE(TAG, "reset intent readback failed");
        return -1;
    }
    return 1;
}

int bk7259_factory_reset_pending(void) {
    if (easyflash_init() != EF_NO_ERR) {
        MYBOT_LOGE(TAG, "reset storage initialization failed");
        return -1;
    }
    return read_intent();
}

static int write_intent(void) {
    EfErrCode result = ef_set_env_blob(RESET_INTENT_KEY, &s_reset_intent,
                                       sizeof(s_reset_intent));
    if (result != EF_NO_ERR || read_intent() != 1) {
        MYBOT_LOGE(TAG, "reset intent persistence failed (err=%d)", (int)result);
        return -1;
    }
    return 0;
}

int bk7259_factory_reset_begin(void) {
    if (easyflash_init() != EF_NO_ERR) {
        MYBOT_LOGE(TAG, "reset storage initialization failed");
        return -1;
    }
    int pending = read_intent();
    if (pending == 1) {
        return 0;
    }
    /* Only a new explicit confirmation may begin a reset when an invalid marker
     * exists. Boot detects that error and waits for the user's new request. */
    if (write_intent() < 0) {
        return -1;
    }
    MYBOT_LOGI(TAG, "factory reset intent persisted");
    return 0;
}

static int delete_record(const char *key, bool allow_invalid_metadata) {
    struct env_node_obj node;
    int present = record_present(key, &node);
    if (present == 0 || (present < 0 && !allow_invalid_metadata)) {
        return present;
    }
    /* Reset owns these exact product keys, so invalid contents or node metadata
     * must not prevent their deletion. The intent remains strictly validated. */
    EfErrCode result = ef_del_env(key);
    /* Missing keys are idempotent, but a reported successful deletion is not
     * enough: read the flash-backed lookup again before completing the reset. */
    int remaining = record_present(key, &node);
    if (result != EF_NO_ERR || remaining != 0) {
        MYBOT_LOGE(TAG, "reset deletion failed for %s (err=%d, remaining=%d)",
                   key, (int)result, remaining);
        return -1;
    }
    return 0;
}

int bk7259_factory_reset_clear(void) {
    if (bk7259_factory_reset_pending() != 1) {
        MYBOT_LOGE(TAG, "refused reset cleanup without a valid persisted intent");
        return -1;
    }
    static const char *const keys[] = {
        "mybot.device_auth",
        "mybot.wifi.v1",
        "mybot.volume.v1",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (delete_record(keys[i], true) < 0) {
            /* Leave the intent intact so boot or an explicit retry resumes. */
            return -1;
        }
    }
    if (delete_record(RESET_INTENT_KEY, false) < 0) {
        /* An error may be reported after the marker was actually removed.
         * Restore the intent before returning a failure to the application. */
        if (write_intent() < 0) {
            MYBOT_LOGE(TAG, "could not preserve reset intent after final cleanup failure");
        }
        return -1;
    }
    MYBOT_LOGI(TAG, "factory reset records cleared and verified");
    return 0;
}

int bk7259_factory_reset_disconnect_network(void) {
    int result = bk7259_wifi_portal_stop();
    if (bk7259_wifi_manager_disconnect() < 0) {
        result = -1;
    }
    return result;
}
