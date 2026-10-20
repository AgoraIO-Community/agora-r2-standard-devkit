/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"
#include "bk7259_factory_reset.h"
#include "bk7259_provision.h"
#include "bk7259_wifi_internal.h"

#include <common/bk_err.h>
#include <components/event.h>
#include "bk7259_platform_log.h"
#include <components/netif_types.h>
#include <easyflash.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>
#include <os/os.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_wifi"
#define WIFI_CREDENTIAL_KEY "mybot.wifi.v1"
#define WIFI_CREDENTIAL_MAGIC 0x4d425746u
#define WIFI_CREDENTIAL_VERSION 1u
#define WIFI_CONNECT_TIMEOUT_MS 30000u
#define WIFI_RESTART_DELAY_MS 100u
#define WIFI_WAIT_SLICE_MS 250u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved[2];
    char ssid[WIFI_SSID_STR_LEN];
    char password[WIFI_PASSWORD_LEN];
    uint8_t reserved_tail[2];
} wifi_credential_record_t;

_Static_assert(sizeof(wifi_credential_record_t) == 112,
               "unexpected Wi-Fi credential record size");

typedef struct {
    beken_mutex_t lock;
    beken_mutex_t operation_lock;
    beken_semaphore_t event;
    mybot_wifi_event_handler_t emit;
    void *emit_user_data;
    mybot_wifi_event_t last_sdk_event;
    uint32_t sta_generation;
    char target_ssid[WIFI_SSID_STR_LEN];
    bool prepared;
    bool shutting_down;
    bool netif_callback_registered;
    bool disconnect_callback_registered;
    bool scan_callback_registered;
    bool sdk_attached;
    bool last_sdk_event_valid;
    bool sta_started;
    bool sta_has_ip;
    bool scan_in_progress;
    bool scan_done;
    /* Only the product/network operation owner reads or writes this latch. */
    bool credential_rollback_failed;
} wifi_manager_t;

static wifi_manager_t s_wifi;

static const char *wifi_event_name(mybot_wifi_event_t event) {
    switch (event) {
    case MYBOT_WIFI_EVENT_STA_CONNECTED:
        return "connected";
    case MYBOT_WIFI_EVENT_STA_DISCONNECTED:
        return "disconnected";
    case MYBOT_WIFI_EVENT_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static int run_provisioning_portal(const char *device_id) {
    int result = bk7259_provision_begin();
    if (result < 0) {
        return result;
    }
    s_wifi.credential_rollback_failed = false;
    if (bk7259_wifi_manager_disconnect() < 0) {
        return -1;
    }
    (void)bk7259_lcd_show_screen(MYBOT_LCD_SCREEN_WIFI_PROVISIONING);
    if (bk7259_prompt_play_provisioning() < 0) {
        MYBOT_LOGW(TAG, "provisioning prompt unavailable; continuing without audio");
    }
    result = bk7259_wifi_portal_run(device_id);
    if (result < 0) {
        return result;
    }
    if (result == 0 && bk7259_prompt_play_success() < 0) {
        MYBOT_LOGW(TAG, "provisioning success prompt unavailable");
    }
    int aborted = bk7259_provision_abort_reason();
    if (aborted) {
        return bk7259_wifi_manager_disconnect() == 0 ? aborted : -1;
    }
    (void)bk7259_lcd_show_screen(result == 0 ? MYBOT_LCD_SCREEN_STARTING_SERVICES
                                             : MYBOT_LCD_SCREEN_FAILED);
    return result;
}

static void secure_zero(void *data, size_t size) {
    volatile uint8_t *bytes = data;
    while (size-- > 0) {
        *bytes++ = 0;
    }
}

static size_t bounded_string_length(const char *value, size_t capacity) {
    if (!value) {
        return capacity;
    }
    const char *terminator = memchr(value, '\0', capacity);
    return terminator ? (size_t)(terminator - value) : capacity;
}

static bool credentials_are_valid(const wifi_credential_record_t *record) {
    if (!record || record->magic != WIFI_CREDENTIAL_MAGIC ||
        record->version != WIFI_CREDENTIAL_VERSION ||
        record->record_size != sizeof(*record) || record->ssid_length == 0 ||
        record->ssid_length >= sizeof(record->ssid) ||
        record->password_length >= sizeof(record->password) ||
        record->ssid[record->ssid_length] != '\0' ||
        record->password[record->password_length] != '\0' ||
        memchr(record->ssid, '\0', record->ssid_length) ||
        memchr(record->password, '\0', record->password_length)) {
        return false;
    }

    static const uint8_t zeros[2];
    return memcmp(record->reserved, zeros, sizeof(record->reserved)) == 0 &&
           memcmp(record->reserved_tail, zeros, sizeof(record->reserved_tail)) == 0;
}

/* Returns 1 for an absent record, 0 for a valid record, and -1 for corruption. */
static int load_credentials(wifi_credential_record_t *record) {
    if (!record) {
        return -1;
    }

    memset(record, 0, sizeof(*record));
    size_t saved_length = 0;
    (void)ef_get_env_blob(WIFI_CREDENTIAL_KEY, NULL, 0, &saved_length);
    if (saved_length == 0) {
        return 1;
    }
    if (saved_length != sizeof(*record) ||
        ef_get_env_blob(WIFI_CREDENTIAL_KEY, record, sizeof(*record), NULL) !=
            sizeof(*record) ||
        !credentials_are_valid(record)) {
        secure_zero(record, sizeof(*record));
        return -1;
    }
    return 0;
}

static bool manager_ready_locked(const wifi_manager_t *manager) {
    return manager->prepared && !manager->shutting_down;
}

/* Keeping emit() under the manager lock serializes SDK events and makes the
 * SDK destroy callback a completion barrier without owning the product STA. */
static void emit_sdk_transition_locked(wifi_manager_t *manager,
                                       mybot_wifi_event_t event) {
    if (!manager->sdk_attached || !manager->emit ||
        (manager->last_sdk_event_valid && manager->last_sdk_event == event)) {
        return;
    }

    manager->last_sdk_event = event;
    manager->last_sdk_event_valid = true;
    MYBOT_LOGI(TAG, "SDK connectivity event: %s", wifi_event_name(event));
    manager->emit(event, manager->emit_user_data);
}

static void set_sta_connected_locked(wifi_manager_t *manager) {
    if (!manager->sta_has_ip) {
        manager->sta_has_ip = true;
        MYBOT_LOGI(TAG, "STA connectivity -> connected (IPv4 ready)");
        emit_sdk_transition_locked(manager, MYBOT_WIFI_EVENT_STA_CONNECTED);
    }
}

static void set_sta_disconnected_locked(wifi_manager_t *manager) {
    bool was_connected = manager->sta_has_ip;
    manager->sta_has_ip = false;
    if (was_connected) {
        MYBOT_LOGI(TAG, "STA connectivity -> disconnected");
        emit_sdk_transition_locked(manager, MYBOT_WIFI_EVENT_STA_DISCONNECTED);
    }
}

static void signal_waiter(wifi_manager_t *manager) {
    if (manager->event) {
        (void)rtos_set_semaphore(&manager->event);
    }
}

static bool string_is_terminated(const char *value, size_t capacity) {
    return value && memchr(value, '\0', capacity) != NULL;
}

static bool sta_link_matches(const char *ssid) {
    wifi_link_status_t status = {0};
    bool matches = ssid && bk_wifi_sta_get_link_status(&status) == BK_OK &&
                   status.state == WIFI_LINKSTATE_STA_GOT_IP &&
                   string_is_terminated(status.ssid, sizeof(status.ssid)) &&
                   strcmp(status.ssid, ssid) == 0;
    secure_zero(&status, sizeof(status));
    return matches;
}

static bk_err_t wifi_event_callback(void *arg, event_module_t module, int event_id,
                                    void *event_data) {
    wifi_manager_t *manager = arg;
    if (!manager || !manager->lock) {
        return BK_OK;
    }

    if (module == EVENT_MOD_NETIF && event_id == EVENT_NETIF_GOT_IP4) {
        const netif_event_got_ip4_t *got_ip = event_data;
        if (!got_ip || got_ip->netif_if != NETIF_IF_STA) {
            return BK_OK;
        }

        char target_ssid[WIFI_SSID_STR_LEN] = {0};
        uint32_t generation = 0;
        if (rtos_lock_mutex(&manager->lock) != BK_OK) {
            return BK_OK;
        }
        if (manager_ready_locked(manager) && manager->target_ssid[0]) {
            memcpy(target_ssid, manager->target_ssid, sizeof(target_ssid));
            generation = manager->sta_generation;
        }
        (void)rtos_unlock_mutex(&manager->lock);

        bool matches = generation != 0 && sta_link_matches(target_ssid);
        if (rtos_lock_mutex(&manager->lock) == BK_OK) {
            if (matches && manager_ready_locked(manager) &&
                manager->sta_generation == generation &&
                strcmp(manager->target_ssid, target_ssid) == 0) {
                set_sta_connected_locked(manager);
            }
            (void)rtos_unlock_mutex(&manager->lock);
        }
        signal_waiter(manager);
        return BK_OK;
    }

    if (module != EVENT_MOD_WIFI) {
        return BK_OK;
    }
    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        return BK_OK;
    }
    if (manager_ready_locked(manager)) {
        if (event_id == EVENT_WIFI_STA_DISCONNECTED) {
            set_sta_disconnected_locked(manager);
        } else if (event_id == EVENT_WIFI_SCAN_DONE) {
            manager->scan_done = true;
            manager->scan_in_progress = false;
        }
    }
    (void)rtos_unlock_mutex(&manager->lock);
    signal_waiter(manager);
    return BK_OK;
}

static void drain_event(wifi_manager_t *manager) {
    while (manager->event &&
           rtos_get_semaphore(&manager->event, BEKEN_NO_WAIT) == BK_OK) {
    }
}

static uint32_t remaining_time_ms(uint32_t start_ms, uint32_t timeout_ms) {
    uint32_t elapsed_ms = rtos_get_time() - start_ms;
    return elapsed_ms < timeout_ms ? timeout_ms - elapsed_ms : 0;
}

static bool manager_accept_connected(wifi_manager_t *manager, const char *ssid,
                                     uint32_t generation) {
    if (!sta_link_matches(ssid) || rtos_lock_mutex(&manager->lock) != BK_OK) {
        return false;
    }

    bool accepted = manager_ready_locked(manager) &&
                    manager->sta_generation == generation &&
                    strcmp(manager->target_ssid, ssid) == 0;
    if (accepted) {
        set_sta_connected_locked(manager);
    }
    (void)rtos_unlock_mutex(&manager->lock);
    return accepted;
}

static int wait_for_ipv4(wifi_manager_t *manager, const char *ssid,
                         uint32_t generation, uint32_t timeout_ms) {
    uint32_t start_ms = rtos_get_time();
    for (;;) {
        int aborted = bk7259_provision_abort_reason();
        if (aborted) {
            return aborted;
        }
        if (manager_accept_connected(manager, ssid, generation)) {
            return bk7259_provision_abort_reason();
        }

        if (rtos_lock_mutex(&manager->lock) != BK_OK) {
            return -1;
        }
        bool valid = manager_ready_locked(manager) &&
                     manager->sta_generation == generation &&
                     strcmp(manager->target_ssid, ssid) == 0;
        (void)rtos_unlock_mutex(&manager->lock);
        uint32_t remaining_ms = remaining_time_ms(start_ms, timeout_ms);
        if (!valid) {
            MYBOT_LOGW(TAG, "STA IPv4 wait aborted by a network state change");
            return -1;
        }
        if (remaining_ms == 0) {
            MYBOT_LOGW(TAG, "STA IPv4 acquisition timed out");
            return -1;
        }
        (void)rtos_get_semaphore(&manager->event,
                                 remaining_ms < WIFI_WAIT_SLICE_MS
                                     ? remaining_ms
                                     : WIFI_WAIT_SLICE_MS);
    }
}

/* operation_lock is held by the caller. */
static int stop_sta(wifi_manager_t *manager) {
    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        return -1;
    }
    bool was_started = manager->sta_started;
    /* Invalidate callbacks now, but retain driver ownership until stop
     * succeeds so a failed teardown can be retried by the product owner. */
    ++manager->sta_generation;
    manager->target_ssid[0] = '\0';
    set_sta_disconnected_locked(manager);
    (void)rtos_unlock_mutex(&manager->lock);

    if (was_started) {
        MYBOT_LOGI(TAG, "stopping STA");
        if (bk_wifi_sta_stop() != BK_OK) {
            MYBOT_LOGE(TAG, "failed to stop STA");
            return -1;
        }
        if (rtos_lock_mutex(&manager->lock) != BK_OK) {
            return -1;
        }
        manager->sta_started = false;
        (void)rtos_unlock_mutex(&manager->lock);
        MYBOT_LOGI(TAG, "STA stopped");
    }
    return 0;
}

/* operation_lock is held by the caller. */
static int stop_scan(wifi_manager_t *manager) {
    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        return -1;
    }
    bool in_progress = manager->scan_in_progress;
    manager->scan_done = false;
    (void)rtos_unlock_mutex(&manager->lock);
    if (!in_progress) {
        return 0;
    }
    if (bk_wifi_scan_stop() != BK_OK) {
        MYBOT_LOGE(TAG, "failed to stop Wi-Fi scan; retaining driver ownership");
        return -1;
    }
    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        return -1;
    }
    manager->scan_in_progress = false;
    manager->scan_done = false;
    (void)rtos_unlock_mutex(&manager->lock);
    return 0;
}

static int start_sta(wifi_manager_t *manager, const char *ssid,
                     const char *password, uint32_t *generation) {
    wifi_sta_config_t config = {0};
    size_t ssid_length = bounded_string_length(ssid, WIFI_SSID_STR_LEN);
    size_t password_length = bounded_string_length(password, WIFI_PASSWORD_LEN);
    if (ssid_length == 0 || ssid_length >= WIFI_SSID_STR_LEN ||
        password_length >= WIFI_PASSWORD_LEN) {
        return -1;
    }

    memcpy(config.ssid, ssid, ssid_length + 1);
    memcpy(config.password, password, password_length + 1);
    config.security = WIFI_SECURITY_AUTO;
    config.auto_reconnect_count = 0;
    config.auto_reconnect_timeout = 0;
    config.disable_auto_reconnect_after_disconnect = false;

    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        secure_zero(&config, sizeof(config));
        return -1;
    }
    int aborted = bk7259_provision_abort_reason();
    if (!manager_ready_locked(manager) || aborted) {
        (void)rtos_unlock_mutex(&manager->lock);
        secure_zero(&config, sizeof(config));
        return aborted ? aborted : -1;
    }
    ++manager->sta_generation;
    *generation = manager->sta_generation;
    memcpy(manager->target_ssid, ssid, ssid_length + 1);
    manager->sta_has_ip = false;
    (void)rtos_unlock_mutex(&manager->lock);

    MYBOT_LOGI(TAG, "starting STA");
    bk_err_t result = bk_wifi_sta_set_config(&config);
    secure_zero(&config, sizeof(config));
    if (result == BK_OK) {
        if (rtos_lock_mutex(&manager->lock) != BK_OK) {
            return -1;
        }
        bool may_start = manager_ready_locked(manager) &&
                         manager->sta_generation == *generation &&
                         bk7259_provision_abort_reason() == 0;
        /* A failed start may own partial driver resources. Release ownership
         * only after the idempotent stop has actually succeeded. */
        if (may_start) {
            manager->sta_started = true;
        }
        (void)rtos_unlock_mutex(&manager->lock);
        result = may_start ? bk_wifi_sta_start() : BK_FAIL;
    }

    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        (void)stop_sta(manager);
        return -1;
    }
    aborted = bk7259_provision_abort_reason();
    if (!manager_ready_locked(manager) || manager->sta_generation != *generation || aborted) {
        result = BK_FAIL;
    }
    (void)rtos_unlock_mutex(&manager->lock);

    if (result != BK_OK) {
        if (stop_sta(manager) < 0) {
            MYBOT_LOGE(TAG, "STA start cleanup incomplete; disconnect must retry");
            return -1;
        }
        MYBOT_LOGE(TAG, "failed to start STA");
        return aborted ? aborted : -1;
    }
    MYBOT_LOGI(TAG, "STA driver started; waiting for IPv4");
    return 0;
}

int bk7259_wifi_manager_connect_candidate(const char *ssid, const char *password,
                                          uint32_t timeout_ms) {
    wifi_manager_t *manager = &s_wifi;
    if (!ssid || !password || !manager->operation_lock ||
        rtos_lock_mutex(&manager->operation_lock) != BK_OK) {
        return -1;
    }

    int result = -1;
    result = bk7259_provision_abort_reason();
    if (result) {
        goto done;
    }
    result = -1;
    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        goto done;
    }
    bool ready = manager_ready_locked(manager);
    (void)rtos_unlock_mutex(&manager->lock);
    if (!ready || stop_sta(manager) < 0) {
        goto done;
    }

    (void)rtos_delay_milliseconds(WIFI_RESTART_DELAY_MS);
    int aborted = bk7259_provision_abort_reason();
    if (aborted) {
        result = aborted;
        goto done;
    }
    drain_event(manager);
    uint32_t generation = 0;
    result = start_sta(manager, ssid, password, &generation);
    if (result < 0) {
        goto done;
    }
    result = wait_for_ipv4(manager, ssid, generation, timeout_ms);
    if (result < 0) {
        if (stop_sta(manager) < 0) {
            MYBOT_LOGE(TAG, "STA connection cleanup incomplete; disconnect must retry");
            result = -1;
        }
        if (result == BK7259_NETWORK_CANCELLED) {
            MYBOT_LOGI(TAG, "STA connection cancelled for factory reset");
        } else {
            MYBOT_LOGW(TAG, "candidate STA connection failed");
        }
    } else {
        MYBOT_LOGI(TAG, "candidate STA connection succeeded");
    }

done:
    (void)rtos_unlock_mutex(&manager->operation_lock);
    return result;
}

int bk7259_wifi_manager_disconnect(void) {
    wifi_manager_t *manager = &s_wifi;
    if (!manager->operation_lock ||
        rtos_lock_mutex(&manager->operation_lock) != BK_OK) {
        return -1;
    }
    int scan_result = stop_scan(manager);
    int sta_result = stop_sta(manager);
    (void)rtos_unlock_mutex(&manager->operation_lock);
    return scan_result == 0 && sta_result == 0 ? 0 : -1;
}

static bool manager_has_ipv4_for(const char *ssid);

bool bk7259_wifi_manager_storage_fault(void) {
    return s_wifi.credential_rollback_failed;
}

static bool credentials_match(const wifi_credential_record_t *expected) {
    wifi_credential_record_t actual;
    int loaded = load_credentials(&actual);
    bool matches = loaded == 0 && memcmp(&actual, expected, sizeof(actual)) == 0;
    secure_zero(&actual, sizeof(actual));
    return matches;
}

static bool restore_previous_credentials(const wifi_credential_record_t *previous, int previous_state) {
    if (previous_state == 0) {
        if (!credentials_match(previous)) {
            (void)ef_set_env_blob(WIFI_CREDENTIAL_KEY, previous, sizeof(*previous));
        }
        return credentials_match(previous);
    }
    if (previous_state < 0) {
        /* An invalid/unreadable prior record is not proof of absence. Do not
         * delete it while trying to undo an unverified replacement. */
        return false;
    }
    size_t saved_length = 0;
    (void)ef_get_env_blob(WIFI_CREDENTIAL_KEY, NULL, 0, &saved_length);
    if (saved_length != 0) {
        (void)ef_del_env(WIFI_CREDENTIAL_KEY);
    }
    (void)ef_get_env_blob(WIFI_CREDENTIAL_KEY, NULL, 0, &saved_length);
    return saved_length == 0;
}

int bk7259_wifi_manager_save_credentials(const char *ssid, const char *password) {
    int aborted = bk7259_provision_abort_reason();
    if (aborted) {
        return aborted;
    }
    size_t ssid_length = bounded_string_length(ssid, WIFI_SSID_STR_LEN);
    size_t password_length = bounded_string_length(password, WIFI_PASSWORD_LEN);
    if (ssid_length == 0 || ssid_length >= WIFI_SSID_STR_LEN ||
        password_length >= WIFI_PASSWORD_LEN || !manager_has_ipv4_for(ssid)) {
        return -1;
    }

    wifi_credential_record_t record = {
        .magic = WIFI_CREDENTIAL_MAGIC,
        .version = WIFI_CREDENTIAL_VERSION,
        .record_size = sizeof(record),
        .ssid_length = (uint8_t)ssid_length,
        .password_length = (uint8_t)password_length,
    };
    memcpy(record.ssid, ssid, ssid_length + 1);
    memcpy(record.password, password, password_length + 1);
    wifi_credential_record_t previous;
    int previous_state = load_credentials(&previous);
    if (!bk7259_provision_try_commit()) {
        secure_zero(&record, sizeof(record));
        secure_zero(&previous, sizeof(previous));
        aborted = bk7259_provision_abort_reason();
        return aborted ? aborted : -1;
    }
    (void)bk7259_lcd_show_provision(BK7259_PROVISION_FINISHING);
    aborted = bk7259_provision_abort_reason();
    if (aborted) {
        secure_zero(&record, sizeof(record));
        secure_zero(&previous, sizeof(previous));
        bk7259_provision_commit_failed();
        return aborted;
    }
    EfErrCode result = ef_set_env_blob(WIFI_CREDENTIAL_KEY, &record, sizeof(record));
    /* EasyFlash marks an old node PRE_DELETE before replacement and may report
     * an error after the new node became valid. Decide from verified contents,
     * not the API status. If no complete new record exists, restore the prior
     * valid record (or absence) before reopening cancellation. */
    bool committed = credentials_match(&record);
    if (!committed) {
        s_wifi.credential_rollback_failed = !restore_previous_credentials(&previous, previous_state);
    }
    secure_zero(&record, sizeof(record));
    secure_zero(&previous, sizeof(previous));
    if (!committed && !s_wifi.credential_rollback_failed) {
        bk7259_provision_commit_failed();
        (void)bk7259_lcd_show_provision(BK7259_PROVISION_ACTIVE);
    }
    if (s_wifi.credential_rollback_failed) {
        MYBOT_LOGE(TAG, "Wi-Fi credential rollback unverified; ending portal (err=%d)", (int)result);
        return -1;
    }
    /* A reset requested during the synchronous flash write must still reach
     * the product owner, which clears this record before restarting. */
    aborted = bk7259_provision_abort_reason();
    if (aborted) {
        return aborted;
    }
    if (committed) {
        MYBOT_LOGI(TAG, "Wi-Fi credentials saved");
    } else {
        MYBOT_LOGW(TAG, "failed to save Wi-Fi credentials");
    }
    return committed ? 0 : -1;
}

static int scan_result_compare(const wifi_scan_ap_info_t *left,
                               const wifi_scan_ap_info_t *right) {
    return right->rssi - left->rssi;
}

static void sort_scan_results(wifi_scan_ap_info_t *results, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        wifi_scan_ap_info_t selected = results[i];
        size_t position = i;
        while (position > 0 &&
               scan_result_compare(&results[position - 1], &selected) > 0) {
            results[position] = results[position - 1];
            --position;
        }
        results[position] = selected;
    }
}

static int collect_scan_results(wifi_scan_ap_info_t *results, size_t capacity,
                                size_t *result_count) {
    wifi_scan_result_t scan = {0};
    *result_count = 0;
    if (bk_wifi_scan_get_result(&scan) != BK_OK || scan.ap_num < 0 ||
        (scan.ap_num > 0 && !scan.aps)) {
        return -1;
    }

    for (int i = 0; i < scan.ap_num; ++i) {
        wifi_scan_ap_info_t *candidate = &scan.aps[i];
        if (!string_is_terminated(candidate->ssid, sizeof(candidate->ssid)) ||
            candidate->ssid[0] == '\0') {
            continue;
        }

        size_t position = 0;
        while (position < *result_count &&
               strcmp(results[position].ssid, candidate->ssid) != 0) {
            ++position;
        }
        if (position < *result_count) {
            if (candidate->rssi > results[position].rssi) {
                results[position] = *candidate;
            }
        } else if (*result_count < capacity) {
            results[(*result_count)++] = *candidate;
        }
    }
    bk_wifi_scan_free_result(&scan);
    sort_scan_results(results, *result_count);
    return 0;
}

int bk7259_wifi_manager_scan_sync(wifi_scan_ap_info_t *results, size_t capacity,
                                  size_t *result_count, uint32_t timeout_ms) {
    wifi_manager_t *manager = &s_wifi;
    if (!results || capacity == 0 || !result_count || !manager->operation_lock ||
        rtos_lock_mutex(&manager->operation_lock) != BK_OK) {
        return -1;
    }
    *result_count = 0;

    int result = -1;
    result = bk7259_provision_abort_reason();
    if (result) {
        goto done;
    }
    result = -1;
    if (stop_scan(manager) < 0) {
        goto done;
    }
    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        goto done;
    }
    if (!manager_ready_locked(manager)) {
        (void)rtos_unlock_mutex(&manager->lock);
        goto done;
    }
    manager->scan_done = false;
    manager->scan_in_progress = true;
    (void)rtos_unlock_mutex(&manager->lock);
    drain_event(manager);

    if (bk_wifi_scan_start(NULL) != BK_OK) {
        goto finish_scan;
    }
    uint32_t start_ms = rtos_get_time();
    for (;;) {
        int aborted = bk7259_provision_abort_reason();
        if (aborted) {
            result = aborted;
            break;
        }
        if (rtos_lock_mutex(&manager->lock) != BK_OK) {
            break;
        }
        bool ready = manager_ready_locked(manager);
        bool done = manager->scan_done;
        (void)rtos_unlock_mutex(&manager->lock);
        if (!ready) {
            break;
        }
        if (done) {
            result = collect_scan_results(results, capacity, result_count);
            aborted = bk7259_provision_abort_reason();
            if (aborted) {
                *result_count = 0;
                result = aborted;
            }
            break;
        }

        uint32_t remaining_ms = remaining_time_ms(start_ms, timeout_ms);
        if (remaining_ms == 0) {
            break;
        }
        (void)rtos_get_semaphore(&manager->event,
                                 remaining_ms < WIFI_WAIT_SLICE_MS
                                     ? remaining_ms
                                     : WIFI_WAIT_SLICE_MS);
    }

finish_scan:
    if (stop_scan(manager) < 0) {
        result = -1;
    }
done:
    (void)rtos_unlock_mutex(&manager->operation_lock);
    return result;
}

static bool unregister_callback(event_module_t module, int event_id,
                                event_cb_t callback) {
    bk_err_t result = bk_event_unregister_cb(module, event_id, callback);
    return result == BK_OK || result == BK_ERR_EVENT_NO_CB;
}

int bk7259_wifi_prepare(void) {
    wifi_manager_t *manager = &s_wifi;
    if (manager->prepared) {
        return manager->shutting_down ? -1 : 0;
    }
    if (easyflash_init() != EF_NO_ERR || rtos_init_mutex(&manager->lock) != BK_OK ||
        rtos_init_mutex(&manager->operation_lock) != BK_OK ||
        rtos_init_semaphore(&manager->event, 1) != BK_OK) {
        goto fail;
    }

    if (bk_event_register_cb(EVENT_MOD_NETIF, EVENT_NETIF_GOT_IP4,
                             wifi_event_callback, manager) != BK_OK) {
        goto fail;
    }
    manager->netif_callback_registered = true;
    if (bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                             wifi_event_callback, manager) != BK_OK) {
        goto fail;
    }
    manager->disconnect_callback_registered = true;
    if (bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE,
                             wifi_event_callback, manager) != BK_OK) {
        goto fail;
    }
    manager->scan_callback_registered = true;

    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        goto fail;
    }
    manager->prepared = true;
    (void)rtos_unlock_mutex(&manager->lock);
    MYBOT_LOGI(TAG, "Wi-Fi manager ready");
    return 0;

fail:
    if (manager->scan_callback_registered) {
        (void)unregister_callback(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE,
                                  wifi_event_callback);
    }
    if (manager->disconnect_callback_registered) {
        (void)unregister_callback(EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                                  wifi_event_callback);
    }
    if (manager->netif_callback_registered) {
        (void)unregister_callback(EVENT_MOD_NETIF, EVENT_NETIF_GOT_IP4,
                                  wifi_event_callback);
    }
    if (manager->event) {
        (void)rtos_deinit_semaphore(&manager->event);
    }
    if (manager->operation_lock) {
        (void)rtos_deinit_mutex(&manager->operation_lock);
    }
    if (manager->lock) {
        (void)rtos_deinit_mutex(&manager->lock);
    }
    *manager = (wifi_manager_t){0};
    MYBOT_LOGE(TAG, "Wi-Fi manager initialization failed");
    return -1;
}

void bk7259_wifi_shutdown(void) {
    wifi_manager_t *manager = &s_wifi;
    if (!manager->prepared || !manager->lock) {
        return;
    }

    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        return;
    }
    manager->shutting_down = true;
    manager->sdk_attached = false;
    manager->emit = NULL;
    manager->emit_user_data = NULL;
    manager->last_sdk_event_valid = false;
    (void)rtos_unlock_mutex(&manager->lock);
    signal_waiter(manager);

    if (rtos_lock_mutex(&manager->operation_lock) != BK_OK) {
        return;
    }
    int scan_result = stop_scan(manager);
    int sta_result = stop_sta(manager);
    if (scan_result < 0 || sta_result < 0) {
        MYBOT_LOGE(TAG, "Wi-Fi teardown incomplete; retaining manager for retry");
        (void)rtos_unlock_mutex(&manager->operation_lock);
        return;
    }

    bool detached = true;
    if (manager->scan_callback_registered) {
        detached = unregister_callback(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE,
                                       wifi_event_callback) && detached;
    }
    if (manager->disconnect_callback_registered) {
        detached = unregister_callback(EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                                       wifi_event_callback) && detached;
    }
    if (manager->netif_callback_registered) {
        detached = unregister_callback(EVENT_MOD_NETIF, EVENT_NETIF_GOT_IP4,
                                       wifi_event_callback) && detached;
    }
    if (!detached) {
        MYBOT_LOGE(TAG, "failed to detach Wi-Fi event callbacks");
        (void)rtos_unlock_mutex(&manager->operation_lock);
        return;
    }

    (void)rtos_unlock_mutex(&manager->operation_lock);
    (void)rtos_deinit_semaphore(&manager->event);
    (void)rtos_deinit_mutex(&manager->operation_lock);
    (void)rtos_deinit_mutex(&manager->lock);
    *manager = (wifi_manager_t){0};
    MYBOT_LOGI(TAG, "Wi-Fi manager stopped");
}

static bool manager_has_ipv4_for(const char *ssid) {
    wifi_manager_t *manager = &s_wifi;
    if (!ssid || !sta_link_matches(ssid) ||
        rtos_lock_mutex(&manager->lock) != BK_OK) {
        return false;
    }
    bool matches = manager_ready_locked(manager) && manager->sta_started &&
                   strcmp(manager->target_ssid, ssid) == 0;
    if (matches) {
        set_sta_connected_locked(manager);
    }
    (void)rtos_unlock_mutex(&manager->lock);
    return matches;
}

int bk7259_wifi_restore_saved(const char *device_id) {
    wifi_manager_t *manager = &s_wifi;
    if (!device_id || !device_id[0] || !manager->prepared) {
        return -1;
    }
    int aborted = bk7259_provision_abort_reason();
    if (aborted) {
        return aborted;
    }
    wifi_credential_record_t record;
    int loaded = load_credentials(&record);
    if (loaded != 0) {
        secure_zero(&record, sizeof(record));
        return -1;
    }
    int result;
    if (manager_has_ipv4_for(record.ssid)) {
        result = bk7259_provision_abort_reason();
    } else {
        MYBOT_LOGI(TAG, "connecting saved STA");
        result = bk7259_wifi_manager_connect_candidate(
            record.ssid, record.password, WIFI_CONNECT_TIMEOUT_MS);
    }
    secure_zero(&record, sizeof(record));
    return result;
}

int mybot_bk7259_ensure_network(const char *device_id) {
    if (!device_id || !device_id[0] || !s_wifi.prepared) {
        return -1;
    }
    int result = bk7259_wifi_restore_saved(device_id);
    if (result == 0) {
        (void)bk7259_lcd_show_screen(MYBOT_LCD_SCREEN_STARTING_SERVICES);
        return 0;
    }
    if (result < -1) {
        return result;
    }
    int aborted = bk7259_provision_abort_reason();
    if (aborted) {
        /* A prior failed connection may still own a driver. Do not let an
         * accepted abort conceal a failed stop from the application owner. */
        return bk7259_wifi_manager_disconnect() == 0 ? aborted : -1;
    }
    MYBOT_LOGI(TAG, "no usable saved Wi-Fi connection, starting APSTA");
    result = run_provisioning_portal(device_id);
    return result == 0 ? 1 : result;
}

int mybot_bk7259_provision_wifi(const char *device_id) {
    if (!device_id || !device_id[0]) {
        return -1;
    }
    int aborted = bk7259_provision_abort_reason();
    if (aborted) {
        return aborted;
    }
    MYBOT_LOGI(TAG, "starting requested APSTA provisioning");
    int result = bk7259_wifi_manager_disconnect();
    if (result != 0) {
        MYBOT_LOGE(TAG, "failed to prepare STA for APSTA provisioning");
        return -1;
    }
    aborted = bk7259_provision_abort_reason();
    if (aborted) {
        return aborted;
    }
    result = run_provisioning_portal(device_id);
    if (result == BK7259_NETWORK_CANCELLED) {
        MYBOT_LOGI(TAG, "APSTA provisioning cancelled for factory reset");
    } else if (result < 0) {
        MYBOT_LOGW(TAG, "APSTA provisioning failed");
    } else {
        MYBOT_LOGI(TAG, "APSTA provisioning succeeded");
    }
    return result;
}

static int wifi_init(void **out_ctx, const char *device_id,
                     mybot_wifi_event_handler_t emit, void *user_data) {
    wifi_manager_t *manager = &s_wifi;
    if (!out_ctx || !device_id || !device_id[0] || !emit || !manager->lock) {
        return -1;
    }
    *out_ctx = NULL;

    if (rtos_lock_mutex(&manager->lock) != BK_OK) {
        return -1;
    }
    if (!manager_ready_locked(manager) || manager->sdk_attached) {
        (void)rtos_unlock_mutex(&manager->lock);
        return -1;
    }

    manager->emit = emit;
    manager->emit_user_data = user_data;
    manager->last_sdk_event_valid = false;
    manager->sdk_attached = true;
    *out_ctx = manager;
    if (manager->sta_has_ip) {
        emit_sdk_transition_locked(manager, MYBOT_WIFI_EVENT_STA_CONNECTED);
    }
    (void)rtos_unlock_mutex(&manager->lock);
    MYBOT_LOGI(TAG, "Wi-Fi callbacks attached to MyBot");
    return 0;
}

static void wifi_destroy(void *opaque) {
    wifi_manager_t *manager = opaque;
    if (manager != &s_wifi || !manager->lock ||
        rtos_lock_mutex(&manager->lock) != BK_OK) {
        return;
    }

    manager->sdk_attached = false;
    manager->emit = NULL;
    manager->emit_user_data = NULL;
    manager->last_sdk_event_valid = false;
    (void)rtos_unlock_mutex(&manager->lock);
    MYBOT_LOGI(TAG, "Wi-Fi callbacks detached from MyBot");
}

const mybot_wifi_ops_t g_mybot_bk7259_wifi_ops = {
    .init = wifi_init,
    .destroy = wifi_destroy,
};
