#include "bk_private/bk_init.h"

#include <components/bk_uid.h>
#include <components/log.h>
#include <components/system.h>
#include <media_service.h>
#include <mbedtls/md.h>
#include <os/os.h>
#include <api/aosl.h>

#include <mybot/mybot.h>
#include <mybot/mybot_version.h>

#include <mybot_bk7259_platform.h>
#include <bk7259_factory_reset.h>
#include <bk7259_provision.h>
#include <bk7259_key.h>
#include <bk7259_lcd.h>
#include <bk7259_gimbal.h>
#include "bk7259_gimbal_cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TAG "mybot"
#define MYBOT_POLL_INTERVAL_MS 100
#define MYBOT_APP_TASK_STACK_SIZE (16 * 1024)
#define DEVICE_ID_DIGEST_BYTES 12

/* Use a compact HMAC identity with a BK7259-specific derivation domain. The
 * visible prefix separates this platform namespace from other device IDs. */
static const char k_device_id_hmac_key[] = "mybot-bk7259-device-id-v1";

/* The SDK generates this in the project ELF source as __DATE__ " " __TIME__,
 * so it carries the build time of the image that is actually running. */
extern volatile const char build_version[];

static mybot_config_t s_config;
/* Only the product task accesses this flag; LCD calls copy into its snapshot. */
static bool s_display_prepared;

static const char *mybot_state_name(mybot_state_t state)
{
    switch (state) {
    case MYBOT_STATE_STOPPED:
        return "STOPPED";
    case MYBOT_STATE_WIFI_PROVISIONING:
        return "WIFI_PROVISIONING";
    case MYBOT_STATE_STARTING_SERVICES:
        return "STARTING_SERVICES";
    case MYBOT_STATE_READY:
        return "READY";
    case MYBOT_STATE_WIFI_DISCONNECTED:
        return "WIFI_DISCONNECTED";
    case MYBOT_STATE_FAILED:
        return "FAILED";
    case MYBOT_STATE_STOPPING:
        return "STOPPING";
    case MYBOT_STATE_IN_CONVERSATION:
        return "IN_CONVERSATION";
    case MYBOT_STATE_PAIRING:
        return "PAIRING";
    default:
        return "UNKNOWN";
    }
}

static void mybot_publish_runtime_status(mybot_state_t state)
{
    static bool update_failed;
    char text[BK7259_LCD_STATE_CAPACITY] = {0};
    const char *name = mybot_state_name(state);

    if (!s_display_prepared) {
        return;
    }
    for (size_t i = 0; name[i] && i < sizeof(text) - 1U; ++i) {
        text[i] = name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 'a' - 'A' : name[i];
    }
    if (bk7259_lcd_set_runtime_status(text, state == MYBOT_STATE_IN_CONVERSATION) < 0) {
        if (!update_failed) {
            BK_LOGW(TAG, "runtime status display update failed\r\n");
        }
        update_failed = true;
    } else {
        update_failed = false;
    }
}

/* The SDK owns the state machine. Poll its atomic view at the application
 * boundary, copy display text, and log each observed edge. Publish every
 * observation so LCD preparation and SDK detach cannot lose an unchanged state. */
static void mybot_log_state_transition(mybot_state_t *last_state, bool *valid)
{
    mybot_state_t current;

    if (!last_state || !valid) {
        return;
    }
    current = mybot_get_state();
    mybot_publish_runtime_status(current);
    if (!*valid) {
        BK_LOGI(TAG, "runtime state=%s(%d)\r\n", mybot_state_name(current),
                (int)current);
        *last_state = current;
        *valid = true;
        return;
    }
    if (current != *last_state) {
        BK_LOGI(TAG, "runtime state transition: %s(%d) -> %s(%d)\r\n",
                mybot_state_name(*last_state), (int)*last_state,
                mybot_state_name(current), (int)current);
        *last_state = current;
    }
}

/*
 * Product identity: HMAC-SHA256 over the stable per-chip UID, retaining the
 * first 12 digest bytes as uppercase hex. BK7259 uses its own derivation key
 * and visible prefix. The server base follows the selected language, the
 * firmware version is the SDK's own version, and the
 * hardware model is fixed by this port. None of them is a Kconfig value.
 */
static int mybot_build_config(void)
{
    uint8_t uid[32] = {0};
    uint8_t digest[32];
    char digest_hex[DEVICE_ID_DIGEST_BYTES * 2 + 1];
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int written;

    s_config = (mybot_config_t){0};
    (void)snprintf(s_config.server_base, sizeof(s_config.server_base), "%s",
                   MYBOT_SERVER_BASE);
    (void)snprintf(s_config.firmware_ver, sizeof(s_config.firmware_ver), "%s",
                   MYBOT_VERSION_STRING);
    (void)snprintf(s_config.hw_model, sizeof(s_config.hw_model), "%s",
                   "mybot-bk7259");

    if (bk_uid_get_data(uid) != BK_OK) {
        BK_LOGE(TAG, "device UID unavailable; no device identity\r\n");
        return -1;
    }
    if (!sha256 ||
        mbedtls_md_hmac(sha256,
                        (const unsigned char *)k_device_id_hmac_key,
                        sizeof(k_device_id_hmac_key) - 1U,
                        uid, sizeof(uid), digest) != 0) {
        BK_LOGE(TAG, "failed to derive device identity\r\n");
        return -1;
    }
    for (size_t i = 0; i < DEVICE_ID_DIGEST_BYTES; ++i) {
        (void)snprintf(&digest_hex[i * 2], 3, "%02X", digest[i]);
    }
    digest_hex[sizeof(digest_hex) - 1] = '\0';
    written = snprintf(s_config.device_id, sizeof(s_config.device_id),
                       "BK7259-%s", digest_hex);
    return written > 0 && (size_t)written < sizeof(s_config.device_id) ? 0 : -1;
}

static void show_factory_reset(bk7259_factory_reset_screen_t screen)
{
    if (bk7259_lcd_show_factory_reset(screen) < 0) {
        BK_LOGE(TAG, "factory reset display update failed: phase=%d\r\n", (int)screen);
    }
}

/* The product task owns this operation. A failed reset stays stopped and waits
 * for a fresh touch confirmation; its durable intent is also retried after power loss. */
static void mybot_factory_reset_loop(bool requested)
{
    for (;;) {
        /* Recovery owns the product until reboot. Discard setup requests as
         * well as stale short presses; neither may restart SDK or networking. */
        (void)bk7259_provision_take_request();
        bk7259_provision_finish();
        if (!requested) {
            (void)bk7259_key_take_conversation_request();
            requested = bk7259_factory_reset_take_request();
            if (!requested) {
                (void)rtos_delay_milliseconds(MYBOT_POLL_INTERVAL_MS);
                continue;
            }
        }
        requested = false;
        (void)bk7259_factory_reset_take_request();
        (void)bk7259_key_take_conversation_request();
        show_factory_reset(BK7259_FACTORY_RESET_RESTORING);

        /* Writing intent does not erase live SDK data. Persist it first so a
         * power loss during shutdown also resumes recovery on the next boot. */
        int intent_result = bk7259_factory_reset_begin();
        BK_LOGW(TAG, "stopping SDK for factory reset\r\n");
        mybot_stop();
        mybot_publish_runtime_status(mybot_get_state());
        if (mybot_is_running() || mybot_get_state() != MYBOT_STATE_STOPPED) {
            BK_LOGE(TAG, "factory reset deferred: SDK has not stopped\r\n");
            show_factory_reset(BK7259_FACTORY_RESET_FAILED);
            continue;
        }
        if (bk7259_gimbal_center_wait(3000) < 0) {
            BK_LOGW(TAG, "gimbal centering unavailable before factory reset\r\n");
        }
        if (bk7259_factory_reset_disconnect_network() < 0) {
            BK_LOGE(TAG, "factory reset deferred: network teardown has not completed\r\n");
            show_factory_reset(BK7259_FACTORY_RESET_FAILED);
            continue;
        }
        if (intent_result < 0 || bk7259_factory_reset_clear() < 0) {
            BK_LOGE(TAG, "factory reset incomplete; tap Retry on the display\r\n");
            show_factory_reset(BK7259_FACTORY_RESET_FAILED);
            continue;
        }

        show_factory_reset(BK7259_FACTORY_RESET_REBOOTING);
        BK_LOGW(TAG, "factory reset complete: restarting for provisioning\r\n");
        (void)rtos_delay_milliseconds(250);
        bk_reboot();
        /* The reboot API may return before the watchdog resets both cores. */
        for (;;) {
            (void)rtos_delay_milliseconds(1000);
        }
    }
}

static void mybot_poll_conversation_key(void)
{
    if (!bk7259_key_take_conversation_request() ||
        bk7259_factory_reset_requested() || bk7259_provision_busy()) {
        return;
    }
    mybot_state_t state = mybot_get_state();
    if (state == MYBOT_STATE_READY) {
        bk7259_key_dispatch_conversation(true);
    } else if (state == MYBOT_STATE_IN_CONVERSATION) {
        bk7259_key_dispatch_conversation(false);
    }
}

static void mybot_check_factory_reset(void)
{
    if (bk7259_factory_reset_take_request()) {
        mybot_factory_reset_loop(true);
    }
}

static int show_provision(bk7259_provision_screen_t screen)
{
    /* finish() deliberately preserves a newer, unclaimed request. Do not clear
     * its locally published PREPARING screen while retiring the old session. */
    if (screen == BK7259_PROVISION_NONE && bk7259_provision_busy()) {
        return -1;
    }
    int result = bk7259_lcd_show_provision(screen);
    if (result < 0) {
        BK_LOGW(TAG, "provision display update failed: phase=%d\r\n", (int)screen);
    }
    return result;
}

static bool mybot_stop_for_network(void)
{
    mybot_stop();
    mybot_publish_runtime_status(mybot_get_state());
    if (mybot_is_running() || mybot_get_state() != MYBOT_STATE_STOPPED) {
        BK_LOGE(TAG, "network setup deferred: SDK has not stopped\r\n");
        return false;
    }
    return true;
}

static bool mybot_network_ready_to_start(void)
{
    /* Replace the detached SDK's STOPPING snapshot before lifting the product
     * override. LCD init is permitted only after NONE has been published. */
    if (bk7259_lcd_show_screen(MYBOT_LCD_SCREEN_STARTING_SERVICES) < 0) {
        BK_LOGW(TAG, "network-ready workflow display update failed\r\n");
    }
    return show_provision(BK7259_PROVISION_NONE) == 0;
}

/* Called only by the product owner after network setup returns. A cancellation
 * never erases credentials, binding or volume. Confirm all portal/scan/STA
 * ownership is released before clearing cancellation and reconnecting saved Wi-Fi. */
static bool mybot_finish_network_setup(int result)
{
    mybot_check_factory_reset();
    (void)bk7259_key_take_conversation_request();
    if (result >= 0) {
        bk7259_provision_finish();
        return mybot_network_ready_to_start();
    }
    if (mybot_is_running() || mybot_get_state() != MYBOT_STATE_STOPPED ||
        bk7259_factory_reset_disconnect_network() < 0) {
        BK_LOGE(TAG, "network setup cleanup incomplete; SDK restart is blocked\r\n");
        bk7259_provision_finish();
        (void)show_provision(BK7259_PROVISION_FAILED);
        return false;
    }
    mybot_check_factory_reset();
    bk7259_provision_finish();
    /* A new explicit request remains pending across finish(). Claim it in the
     * outer loop instead of delaying it with an obsolete reconnection attempt. */
    if (result == BK7259_PROVISION_REQUESTED || bk7259_provision_abort_reason() != 0) {
        return false;
    }
    (void)show_provision(BK7259_PROVISION_RECONNECTING);
    int restored = bk7259_wifi_restore_saved(s_config.device_id);
    mybot_check_factory_reset();
    (void)bk7259_key_take_conversation_request();
    if (restored == 0) {
        return mybot_network_ready_to_start();
    }
    if (bk7259_factory_reset_disconnect_network() < 0) {
        BK_LOGE(TAG, "saved network cleanup incomplete; retaining recovery controls\r\n");
    }
    mybot_check_factory_reset();
    BK_LOGW(TAG, "saved network unavailable; waiting for setup or factory reset\r\n");
    (void)show_provision(BK7259_PROVISION_FAILED);
    return false;
}

static void mybot_handle_provision_request(bool *sdk_started, bool *start_pending)
{
    (void)show_provision(BK7259_PROVISION_PREPARING);
    (void)bk7259_key_take_conversation_request();
    mybot_check_factory_reset();
    if (bk7259_provision_abort_reason() == BK7259_PROVISION_CANCELLED) {
        /* A quick cancel before SDK stop leaves a live conversation and its
         * current network intact. A stopped product can reconnect saved Wi-Fi. */
        if (*sdk_started || *start_pending || mybot_is_running() ||
            mybot_get_state() != MYBOT_STATE_STOPPED) {
            bk7259_provision_finish();
            (void)show_provision(*sdk_started || *start_pending
                                    ? BK7259_PROVISION_NONE : BK7259_PROVISION_FAILED);
        } else {
            *start_pending = mybot_finish_network_setup(BK7259_PROVISION_CANCELLED);
        }
        return;
    }
    *sdk_started = false;
    *start_pending = false;
    BK_LOGI(TAG, "stopping SDK for requested Wi-Fi setup\r\n");
    bool stopped = mybot_stop_for_network();
    mybot_check_factory_reset();
    if (!stopped) {
        bk7259_provision_finish();
        (void)show_provision(BK7259_PROVISION_FAILED);
        return;
    }
    int abort = bk7259_provision_abort_reason();
    int result = abort ? abort : mybot_bk7259_provision_wifi(s_config.device_id);
    *start_pending = mybot_finish_network_setup(result);
}

static int mybot_run(void)
{
    mybot_state_t last_state = MYBOT_STATE_STOPPED;
    bool state_valid = false;

    if (mybot_build_config() < 0) {
        BK_LOGE(TAG, "could not build the device configuration\r\n");
        return -1;
    }

    /* Provisioning prompts use the same AOSL-backed audio adapter as the SDK,
     * but they can run before mybot_start() acquires its own reference. Keep
     * one product-lifetime reference so both paths share initialized globals. */
    aosl_ctor();
    /* SDK core and platform adapters use AOSL; raise its gate from ERROR. */
    aosl_set_log_level(AOSL_LOG_NOTICE);
    mybot_log_state_transition(&last_state, &state_valid);

    if (s_config.server_base[0] == '\0') {
        BK_LOGW(TAG,
                "MYBOT_SERVER_BASE is empty; runtime startup will fail\r\n");
    }
    BK_LOGI(TAG, "device=%s server=%s\r\n", s_config.device_id,
            s_config.server_base);

    if (mybot_bk7259_platform_prepare() < 0) {
        BK_LOGE(TAG, "platform preparation failed\r\n");
        aosl_dtor();
        return -1;
    }
    s_display_prepared = true;
    if (bk7259_gimbal_cli_init() != 0) {
        BK_LOGW(TAG, "gimbal CLI registration failed\r\n");
    }
    mybot_log_state_transition(&last_state, &state_valid);

    int reset_pending = bk7259_factory_reset_pending();
    if (reset_pending != 0) {
        if (reset_pending > 0) {
            BK_LOGW(TAG, "resuming factory reset after interrupted shutdown\r\n");
        } else {
            BK_LOGE(TAG, "factory reset intent invalid; waiting for touch confirmation\r\n");
            show_factory_reset(BK7259_FACTORY_RESET_FAILED);
        }
        mybot_factory_reset_loop(reset_pending > 0);
    }
    mybot_check_factory_reset();

    bool platform_registered = mybot_bk7259_platform_register() == 0;
    mybot_check_factory_reset();
    if (!platform_registered) {
        BK_LOGE(TAG, "platform registration failed\r\n");
        (void)show_provision(BK7259_PROVISION_FAILED);
    }
    bool sdk_started = false;
    bool start_pending = false;
    if (platform_registered) {
        mybot_log_state_transition(&last_state, &state_valid);
        int result = mybot_bk7259_ensure_network(s_config.device_id);
        mybot_log_state_transition(&last_state, &state_valid);
        start_pending = mybot_finish_network_setup(result);
    }
    /* This owner persists through SDK/network failures. Settings and factory
     * reset keep working; only a fresh explicit setup request opens a portal. */
    for (;;) {
        mybot_check_factory_reset();
        if (bk7259_provision_take_request()) {
            mybot_handle_provision_request(&sdk_started, &start_pending);
            continue;
        }
        if (start_pending) {
            mybot_check_factory_reset();
            if (bk7259_provision_abort_reason() != 0) {
                continue;
            }
            start_pending = false;
            (void)bk7259_key_take_conversation_request();
            if (!platform_registered) {
                platform_registered = mybot_bk7259_platform_register() == 0;
            }
            if (platform_registered && mybot_network_ready_to_start()) {
                BK_LOGI(TAG, "starting SDK\r\n");
                int start_result = mybot_start(&s_config);
                mybot_check_factory_reset();
                if (start_result == 0) {
                    sdk_started = true;
                    BK_LOGI(TAG, "SDK started\r\n");
                } else {
                    BK_LOGE(TAG, "SDK startup failed\r\n");
                    (void)mybot_stop_for_network();
                    mybot_check_factory_reset();
                    (void)show_provision(BK7259_PROVISION_FAILED);
                }
            } else {
                (void)show_provision(BK7259_PROVISION_FAILED);
            }
        }
        mybot_log_state_transition(&last_state, &state_valid);
        if (sdk_started) {
            if (!mybot_is_running() || mybot_get_state() == MYBOT_STATE_FAILED) {
                BK_LOGW(TAG, "SDK exited; retaining setup and reset controls\r\n");
                sdk_started = false;
                (void)mybot_stop_for_network();
                mybot_check_factory_reset();
                (void)show_provision(BK7259_PROVISION_FAILED);
            } else {
                mybot_poll_conversation_key();
            }
        } else {
            (void)bk7259_key_take_conversation_request();
        }
        (void)rtos_delay_milliseconds(MYBOT_POLL_INTERVAL_MS);
    }
}

static void mybot_app_task(beken_thread_arg_t arg)
{
    (void)arg;
    int result = mybot_run();
    BK_LOGI(TAG, "MyBot task exited, result=%d\r\n", result);
    rtos_delete_thread(NULL);
}

int main(void)
{
    if (bk_init() != 0) {
        BK_LOGE(TAG, "BK component initialization failed\r\n");
        return -1;
    }

    /* AOSL belongs to the product task below, so early startup uses BK logging. */
    BK_LOGI(TAG, "mybot version: %s, build time: %s\r\n", MYBOT_VERSION_STRING,
            (const char *)build_version);

    /*
     * bk_display's DPU backend calls the AVDK Nano OSI wrapper indirectly
     * (for example, viv_os_mem_alloc -> vsios_malloc).  The wrapper table is
     * registered by media_service_init(); it must be ready before LCD/DPU
     * preparation starts.
     */
    if (media_service_init() != 0) {
        BK_LOGE(TAG, "media service initialization failed\r\n");
        return -1;
    }

    /*
     * The vendor startup invokes main() from a short-lived, highest-priority
     * app_main_thread and deletes it after main returns.  Keep the product
     * control loop in its own normal-priority task so it cannot monopolize an
     * SMP core while waiting for network or SDK events.
     */
    if (rtos_create_psram_thread(NULL, BEKEN_DEFAULT_WORKER_PRIORITY,
                                 "mybot_app", mybot_app_task,
                                 MYBOT_APP_TASK_STACK_SIZE, NULL) != BK_OK) {
        BK_LOGE(TAG, "failed to create MyBot task\r\n");
        return -1;
    }

    return 0;
}
