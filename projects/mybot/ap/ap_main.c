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
 * for a fresh long press; its durable intent is also retried after power loss. */
static void mybot_factory_reset_loop(bool requested)
{
    for (;;) {
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
            BK_LOGE(TAG, "factory reset incomplete; hold five seconds to retry\r\n");
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
        bk7259_factory_reset_requested()) {
        return;
    }
    mybot_state_t state = mybot_get_state();
    if (state == MYBOT_STATE_READY) {
        bk7259_key_dispatch_conversation(true);
    } else if (state == MYBOT_STATE_IN_CONVERSATION) {
        bk7259_key_dispatch_conversation(false);
    }
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
            BK_LOGE(TAG, "factory reset intent invalid; waiting for a fresh long press\r\n");
            show_factory_reset(BK7259_FACTORY_RESET_FAILED);
        }
        mybot_factory_reset_loop(reset_pending > 0);
    }

    if (mybot_bk7259_platform_register() < 0) {
        BK_LOGE(TAG, "platform registration failed\r\n");
        s_display_prepared = false;
        mybot_bk7259_platform_shutdown();
        aosl_dtor();
        return -1;
    }

    mybot_log_state_transition(&last_state, &state_valid);
    int network_result = mybot_bk7259_ensure_network(s_config.device_id);
    mybot_log_state_transition(&last_state, &state_valid);
    if (bk7259_factory_reset_take_request()) {
        mybot_factory_reset_loop(true);
    }
    if (network_result < 0) {
        BK_LOGE(TAG, "network setup failed\r\n");
    } else {
        BK_LOGI(TAG, "starting SDK\r\n");
        if (mybot_start(&s_config) < 0) {
            mybot_log_state_transition(&last_state, &state_valid);
            BK_LOGE(TAG, "SDK startup failed\r\n");
            mybot_stop();
            mybot_log_state_transition(&last_state, &state_valid);
        } else {
            BK_LOGI(TAG, "SDK started\r\n");
            mybot_log_state_transition(&last_state, &state_valid);
            while (mybot_is_running()) {
                mybot_log_state_transition(&last_state, &state_valid);
                if (bk7259_factory_reset_take_request()) {
                    mybot_factory_reset_loop(true);
                }
                mybot_poll_conversation_key();
                if (mybot_get_state() == MYBOT_STATE_FAILED) {
                    BK_LOGE(TAG, "SDK entered the failed state\r\n");
                    break;
                }
                (void)rtos_delay_milliseconds(MYBOT_POLL_INTERVAL_MS);
            }

            BK_LOGI(TAG, "stopping SDK, reason=sdk_exit\r\n");
            mybot_state_t stop_state = mybot_get_state();
            BK_LOGI(TAG, "SDK stop requested from state=%s(%d)\r\n",
                    mybot_state_name(stop_state), (int)stop_state);
            mybot_stop();
            mybot_log_state_transition(&last_state, &state_valid);
            if (mybot_is_running() || mybot_get_state() != MYBOT_STATE_STOPPED) {
                BK_LOGE(TAG, "SDK shutdown did not reach STOPPED\r\n");
            }
        }
    }

    mybot_stop();
    mybot_log_state_transition(&last_state, &state_valid);
    /* Keep the physical key and product display alive after a startup/network
     * failure too, so factory reset remains available without an SDK handler. */
    if (bk7259_factory_reset_take_request()) {
        mybot_factory_reset_loop(true);
    }
    BK_LOGW(TAG, "product stopped: waiting for a factory reset request\r\n");
    mybot_factory_reset_loop(false);
    return -1;
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
