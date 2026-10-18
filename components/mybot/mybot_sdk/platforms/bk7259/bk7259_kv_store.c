/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"

#include <api/aosl_mm.h>
#include "bk7259_platform_log.h"
#include <easyflash.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KV_KEY_CAPACITY 64U
#define KV_MAGIC 0x4d42544bU
#define TAG "mybot_kv"

typedef struct {
    uint32_t magic;
    uint32_t length;
} kv_header_t;

static int make_key(const char *key, char output[KV_KEY_CAPACITY]) {
    if (!key || !key[0]) {
        return -1;
    }
    int written = snprintf(output, KV_KEY_CAPACITY, "mybot.%s", key);
    return written > 0 && written < (int)KV_KEY_CAPACITY ? 0 : -1;
}

static int kv_init(void **ctx) {
    if (!ctx) {
        MYBOT_LOGE(TAG, "KV store initialization rejected: invalid context");
        return -1;
    }
    if (easyflash_init() != EF_NO_ERR) {
        MYBOT_LOGE(TAG, "EasyFlash initialization failed");
        return -1;
    }
    *ctx = (void *)(uintptr_t)1U;
    MYBOT_LOGI(TAG, "KV store ready");
    return 0;
}

static int kv_get(void *ctx, const char *key, void *value, size_t capacity,
                  size_t *out_len) {
    if (!ctx || !value || !out_len) {
        return -1;
    }

    char namespaced[KV_KEY_CAPACITY];
    if (make_key(key, namespaced) < 0) {
        return -1;
    }

    size_t saved_len = 0;
    (void)ef_get_env_blob(namespaced, NULL, 0, &saved_len);
    if (saved_len == 0) {
        return MYBOT_ERR_NOT_FOUND;
    }
    if (saved_len < sizeof(kv_header_t)) {
        return -1;
    }

    uint8_t *stored = aosl_malloc(saved_len);
    if (!stored) {
        return -1;
    }
    size_t read_len = ef_get_env_blob(namespaced, stored, saved_len, NULL);
    if (read_len != saved_len) {
        aosl_free(stored);
        return -1;
    }

    kv_header_t header;
    memcpy(&header, stored, sizeof(header));
    if (header.magic != KV_MAGIC || header.length != saved_len - sizeof(header) ||
        header.length > capacity) {
        aosl_free(stored);
        return -1;
    }
    memcpy(value, stored + sizeof(header), header.length);
    *out_len = header.length;
    aosl_free(stored);
    return 0;
}

static int kv_set(void *ctx, const char *key, const void *value, size_t len) {
    if (!ctx || (!value && len != 0) || len > UINT32_MAX) {
        return -1;
    }

    char namespaced[KV_KEY_CAPACITY];
    if (make_key(key, namespaced) < 0 || len > SIZE_MAX - sizeof(kv_header_t)) {
        return -1;
    }

    size_t stored_len = sizeof(kv_header_t) + len;
    uint8_t *stored = aosl_malloc(stored_len);
    if (!stored) {
        return -1;
    }
    kv_header_t header = {.magic = KV_MAGIC, .length = (uint32_t)len};
    memcpy(stored, &header, sizeof(header));
    if (len != 0) {
        memcpy(stored + sizeof(header), value, len);
    }
    EfErrCode result = ef_set_env_blob(namespaced, stored, stored_len);
    aosl_free(stored);
    if (result != EF_NO_ERR) {
        MYBOT_LOGE(TAG, "KV write failed (err=%d)", (int)result);
    }
    return result == EF_NO_ERR ? 0 : -1;
}

static int kv_erase(void *ctx, const char *key) {
    if (!ctx) {
        return -1;
    }
    char namespaced[KV_KEY_CAPACITY];
    if (make_key(key, namespaced) < 0) {
        return -1;
    }
    size_t saved_len = 0;
    (void)ef_get_env_blob(namespaced, NULL, 0, &saved_len);
    if (saved_len == 0) {
        return 0;
    }
    EfErrCode result = ef_del_env(namespaced);
    if (result != EF_NO_ERR) {
        MYBOT_LOGE(TAG, "KV erase failed (err=%d)", (int)result);
    }
    return result == EF_NO_ERR ? 0 : -1;
}

static void kv_destroy(void *ctx) {
    if (ctx) {
        MYBOT_LOGI(TAG, "KV store destroyed");
    }
}

const mybot_kv_store_ops_t g_mybot_bk7259_kv_ops = {
    .init = kv_init,
    .get = kv_get,
    .set = kv_set,
    .erase = kv_erase,
    .destroy = kv_destroy,
};
