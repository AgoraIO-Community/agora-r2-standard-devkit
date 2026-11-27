/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal_cli.h"
#include <sdkconfig.h>

#if defined(CONFIG_CLI) && CONFIG_CLI && \
    defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
#include <bk_private/bk_cli.h>
#include <bk7259_gimbal.h>
#include <components/log.h>
#if CONFIG_SHELL_ASYNCLOG
#include <components/shell_task.h>
#include <os/os.h>
#endif
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TAG "gimbal_cmd"

static bool s_registered;
/* AP CLI callbacks are serialized by the shell. Advance only after admission. */
static bk7259_gimbal_scene_t s_next_scene = BK7259_GIMBAL_SCENE_HAPPY;
static const char *const k_scene_names[] = {"happy", "sad", "angry", "surprised"};

static void command_reply(char *buffer, int length, const char *message)
{
    if (buffer != NULL && length > 0)
        (void)snprintf(buffer, (size_t)length, "%s\r\n", message);
    else
        BK_LOGI(TAG, "%s\r\n", message);
}

static void motion_reply(char *buffer, int length, int result)
{
    if (result == 0)
        command_reply(buffer, length, "gimbal motion queued");
    else if (result == BK7259_GIMBAL_BUSY)
        command_reply(buffer, length, "gimbal busy");
    else if (result == BK7259_GIMBAL_NOT_READY)
        command_reply(buffer, length, "gimbal not ready: waiting for center confirmation");
    else if (result == BK7259_GIMBAL_MOTION_DISABLED)
        command_reply(buffer, length, "gimbal motion disabled by settings");
    else
        command_reply(buffer, length, "gimbal unavailable");
}

static void gimbal_command(char *buffer, int length, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_cmd (send the next one of four commands)");
        return;
    }
    motion_reply(buffer, length, bk7259_gimbal_send_next_command());
}

static void gimbal_scene_command(char *buffer, int length, int argc, char **argv)
{
    bk7259_gimbal_scene_t scene = s_next_scene;
    if (argc == 2) {
        size_t index = 0;
        for (; index < sizeof(k_scene_names) / sizeof(k_scene_names[0]); ++index)
            if (strcmp(argv[1], k_scene_names[index]) == 0)
                break;
        if (index == sizeof(k_scene_names) / sizeof(k_scene_names[0])) {
            command_reply(buffer, length, "Usage: gimbal_scene [happy|sad|angry|surprised]");
            return;
        }
        scene = (bk7259_gimbal_scene_t)index;
    } else if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_scene [happy|sad|angry|surprised]");
        return;
    }

    int result = bk7259_gimbal_request(scene);
    if (result == 0)
        s_next_scene = (bk7259_gimbal_scene_t)(((unsigned)scene + 1U) % BK7259_GIMBAL_SCENE_COUNT);
    motion_reply(buffer, length, result);
}

static void gimbal_dialogue_command(char *buffer, int length, int argc, char **argv)
{
    static const struct {
        const char *name;
        bool active;
        bk7259_gimbal_phase_t phase;
    } events[] = {
        {"start", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"listening", true, BK7259_GIMBAL_PHASE_LISTENING},
        {"thinking", true, BK7259_GIMBAL_PHASE_THINKING},
        {"speaking", true, BK7259_GIMBAL_PHASE_SPEAKING},
        {"neutral", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"end", false, BK7259_GIMBAL_PHASE_NEUTRAL},
    };
    if (argc == 2) {
        for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
            if (strcmp(argv[1], events[i].name) == 0) {
                motion_reply(buffer, length,
                             bk7259_gimbal_update_dialogue(events[i].active, events[i].phase));
                return;
            }
        }
    }
    command_reply(buffer, length, "Usage: gimbal_dialogue start|listening|thinking|speaking|neutral|end");
}

static void gimbal_probe_command(char *buffer, int length, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_probe");
        return;
    }
    int result = bk7259_gimbal_request_probe();
    if (result == 0)
        command_reply(buffer, length, "gimbal_probe queued; check the UART result in bk7259_gimbal logs");
    else if (result == -2)
        command_reply(buffer, length, "gimbal_probe busy");
    else
        command_reply(buffer, length, "gimbal_probe unavailable; UART worker is not running or is shutting down");
}

static void gimbal_charge_command(char *buffer, int length, int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        command_reply(buffer, length, "Usage: gimbal_charge on|off");
        return;
    }

    bool enabled = strcmp(argv[1], "on") == 0;
    if (bk7259_gimbal_set_charging(enabled) == 0)
        command_reply(buffer, length, enabled ? "gimbal_charge on queued" : "gimbal_charge off queued");
    else
        command_reply(buffer, length, "gimbal_charge unavailable; check UART readiness and pending operations");
}

static void gimbal_test_command(char *buffer, int length, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_test");
        return;
    }
    int result = bk7259_gimbal_schedule_test();
    if (result == 0) {
        command_reply(buffer, length, "gimbal_test scheduled in 10s. You can unplug Type-C now.");
    } else {
        motion_reply(buffer, length, result);
    }
}

typedef struct {
    char chunk[136];
    size_t used;
    bool sent;
    bool failed;
} stats_output_t;

static void stats_flush(stats_output_t *output)
{
    if (output->used == 0 || output->failed) {
        return;
    }
#if CONFIG_SHELL_ASYNCLOG
    /* AP command responses have 252 bytes and the shared UART has eight
     * 136-byte log blocks. Give each chunk time to leave a 115200 8N1 UART. */
    if ((output->sent && rtos_delay_milliseconds(15) != BK_OK) ||
        !shell_log_raw_data((const uint8_t *)output->chunk, (uint16_t)output->used)) {
        output->failed = true;
    }
#else
    output->chunk[output->used] = '\0';
    BK_LOGI(TAG, "%s", output->chunk);
#endif
    output->sent = true;
    output->used = 0;
}

static void stats_line(stats_output_t *output, const char *format, ...)
{
    if (output->failed) {
        return;
    }
    char line[sizeof(output->chunk)];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (count <= 0 || (size_t)count >= sizeof(line)) {
        output->failed = true;
        return;
    }
    if (output->used + (size_t)count >= sizeof(output->chunk)) {
        stats_flush(output);
        if (output->failed) {
            return;
        }
    }
    memcpy(output->chunk + output->used, line, (size_t)count);
    output->used += (size_t)count;
}

static void stats_percent(char *percent, size_t capacity, uint64_t accepted, uint64_t settled)
{
    if (settled == 0) {
        (void)snprintf(percent, capacity, "n/a");
        return;
    }
    unsigned tenths = (unsigned)(accepted * 1000U / settled);
    (void)snprintf(percent, capacity, "%u.%u%%", tenths / 10U, tenths % 10U);
}

static void stats_arrival(stats_output_t *output, const char *name,
                          const bk7259_gimbal_arrival_counts_t *arrival, uint32_t overflow)
{
    uint64_t settled = (uint64_t)arrival->reached + arrival->expired + arrival->rejected + overflow;
    char percent[16];
    stats_percent(percent, sizeof(percent), arrival->reached, settled);
    stats_line(output, "arrival %s reached=%" PRIu32 " timeout=%" PRIu32 " rejected=%" PRIu32
               " cancelled=%" PRIu32 " pending=%" PRIu32 " success=%s\r\n",
               name, arrival->reached, arrival->expired, arrival->rejected,
               arrival->cancelled, arrival->pending, percent);
}

static void stats_group(stats_output_t *output, const char *name, const bk7259_gimbal_stats_counts_t *counts)
{
    stats_line(output, "%s tx=%" PRIu32 " tx_failed=%" PRIu32 " accepted=%" PRIu32
               " rejected=%" PRIu32 " timeout=%" PRIu32 " pending=%" PRIu32 "\r\n",
               name, counts->tx, counts->tx_failed, counts->accepted, counts->rejected,
               counts->expired, counts->pending);
    uint64_t settled = (uint64_t)counts->accepted + counts->rejected + counts->expired + counts->overflow;
    char percent[16];
    stats_percent(percent, sizeof(percent), counts->accepted, settled);
    stats_line(output, "%s success=%s overflow=%" PRIu32 " estimated=%" PRIu32
               " completion_fallback=%" PRIu32 "\r\n", name, percent, counts->overflow,
               counts->estimated, counts->completion_fallback);
    stats_arrival(output, name, &counts->arrival, counts->overflow);
}

static void stats_test(stats_output_t *output, const bk7259_gimbal_stats_snapshot_t *stats)
{
    static const char *const states[] = {
        [BK7259_GIMBAL_TEST_IDLE] = "idle",
        [BK7259_GIMBAL_TEST_SCHEDULED] = "scheduled",
        [BK7259_GIMBAL_TEST_RUNNING] = "running",
        [BK7259_GIMBAL_TEST_COMPLETED] = "completed",
        [BK7259_GIMBAL_TEST_INTERRUPTED] = "interrupted",
    };
    static const char *const stages[BK7259_GIMBAL_TEST_STAGE_COUNT] = {
        [BK7259_GIMBAL_TEST_NONE] = "none",
        [BK7259_GIMBAL_TEST_OPENING] = "opening",
        [BK7259_GIMBAL_TEST_LISTENING] = "listening",
        [BK7259_GIMBAL_TEST_THINKING] = "thinking",
        [BK7259_GIMBAL_TEST_SPEAKING] = "speaking",
        [BK7259_GIMBAL_TEST_NEUTRAL] = "neutral",
        [BK7259_GIMBAL_TEST_HANGUP] = "hangup",
        [BK7259_GIMBAL_TEST_HAPPY] = "happy",
        [BK7259_GIMBAL_TEST_SAD] = "sad",
        [BK7259_GIMBAL_TEST_ANGRY] = "angry",
        [BK7259_GIMBAL_TEST_SURPRISED] = "surprised",
    };
    unsigned state = (unsigned)stats->test_state;
    unsigned stage = (unsigned)stats->test_stage;
    stats_line(output, "test=%s stage=%s\r\n",
               state < sizeof(states) / sizeof(states[0]) ? states[state] : "unknown",
               stage < BK7259_GIMBAL_TEST_STAGE_COUNT ? stages[stage] : "unknown");
    if (state == BK7259_GIMBAL_TEST_IDLE) {
        return;
    }
    for (unsigned i = BK7259_GIMBAL_TEST_OPENING; i < BK7259_GIMBAL_TEST_STAGE_COUNT; ++i) {
        const bk7259_gimbal_test_counts_t *counts = &stats->test_counts[i];
        stats_line(output, "stage %s tx=%" PRIu32 " accepted=%" PRIu32
                   " rejected=%" PRIu32 " timeout=%" PRIu32 " overflow=%" PRIu32
                   " tx_failed=%" PRIu32 "\r\n", stages[i], counts->tx, counts->accepted,
                   counts->rejected, counts->expired, counts->overflow, counts->tx_failed);
        if (counts->motion_pairs != 0) {
            stats_line(output, "stage %s motion_pairs=%" PRIu32 " min_motion_gap=%" PRIu32 "ms\r\n",
                       stages[i], counts->motion_pairs, counts->min_motion_gap_ms);
        } else {
            stats_line(output, "stage %s motion_pairs=0 min_motion_gap=n/a\r\n", stages[i]);
        }
        stats_arrival(output, stages[i], &counts->arrival, counts->overflow);
    }
}

static void stats_idle_gap(stats_output_t *output, const char *direction, uint32_t pairs, uint32_t minimum)
{
    if (pairs != 0) {
        stats_line(output, "%s pairs=%" PRIu32 " min_idle_est=%" PRIu32 "ms\r\n",
                   direction, pairs, minimum);
    } else {
        stats_line(output, "%s pairs=0 min_idle_est=n/a\r\n", direction);
    }
}

static void stats_latency(stats_output_t *output, const char *name, uint32_t samples,
                          uint32_t minimum, uint64_t total, uint32_t median_twice, uint32_t maximum)
{
    if (samples == 0) {
        stats_line(output, "%s n=0 min=n/a avg=n/a median=n/a max=n/a\r\n", name);
        return;
    }
    uint64_t mean_ms = total / samples;
    uint64_t remainder = total % samples;
    unsigned mean_tenths = (unsigned)((remainder * 10U + samples / 2U) / samples);
    mean_ms += mean_tenths / 10U;
    stats_line(output, "%s n=%" PRIu32 " min=%" PRIu32 " avg=%" PRIu64 ".%u"
               " median=%" PRIu32 ".%u max=%" PRIu32 " ms\r\n", name, samples, minimum,
               mean_ms, mean_tenths % 10U, median_twice / 2U, (median_twice & 1U) ? 5U : 0U,
               maximum);
}

static void stats_power_group(stats_output_t *output, const char *name,
                              const bk7259_gimbal_power_counts_t *counts)
{
    stats_line(output, "%s tx=%" PRIu32 " tx_failed=%" PRIu32 " accepted=%" PRIu32
               " rejected=%" PRIu32 " timeout=%" PRIu32 " pending=%" PRIu32 "\r\n",
               name, counts->tx, counts->tx_failed, counts->accepted, counts->rejected,
               counts->expired, counts->pending);
}

static void stats_power(stats_output_t *output, const bk7259_gimbal_power_stats_t *power)
{
    const bk7259_gimbal_power_counts_t *stop = &power->groups[BK7259_GIMBAL_POWER_STOP];
    const bk7259_gimbal_power_counts_t *request = &power->groups[BK7259_GIMBAL_POWER_REQUEST];
    uint64_t accepted = (uint64_t)stop->accepted + request->accepted;
    uint64_t rejected = (uint64_t)stop->rejected + request->rejected;
    uint64_t expired = (uint64_t)stop->expired + request->expired;
    char percent[16];
    stats_percent(percent, sizeof(percent), accepted, accepted + rejected + expired);
    stats_line(output, "power tx=%" PRIu64 " tx_failed=%" PRIu64 " accepted=%" PRIu64
               " rejected=%" PRIu64 " timeout=%" PRIu64 " pending=%" PRIu64 " success=%s\r\n",
               (uint64_t)stop->tx + request->tx, (uint64_t)stop->tx_failed + request->tx_failed,
               accepted, rejected, expired, (uint64_t)stop->pending + request->pending, percent);
    stats_power_group(output, "power 00", stop);
    stats_power_group(output, "power 01", request);
    stats_line(output, "power E6 matched_00=%" PRIu32 " matched_02=%" PRIu32
               " bad=%" PRIu32 " late=%" PRIu32 " unmatched=%" PRIu32 " suppressed=%" PRIu32 "\r\n",
               power->e6_00, power->e6_02, power->bad_reply, power->late, power->unmatched, power->suppressed);
    stats_latency(output, "power_latency", power->latency_samples, power->latency_min_ms,
                  power->latency_total_ms, power->latency_median_twice_ms, power->latency_max_ms);
}

static void gimbal_stats_command(char *buffer, int length, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        int result = bk7259_gimbal_reset_stats();
        command_reply(buffer, length, result == 0 ? "gimbal_stats reset" :
                      result == BK7259_GIMBAL_BUSY ? "gimbal_stats busy: test scheduled or running" :
                      "gimbal_stats unavailable");
        return;
    }
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_stats [reset]");
        return;
    }
    bk7259_gimbal_stats_snapshot_t stats;
    if (bk7259_gimbal_get_stats(&stats) != 0) {
        command_reply(buffer, length, "gimbal_stats unavailable");
        return;
    }
    stats_output_t output = {0};
    stats_test(&output, &stats);
    stats_line(&output, "gimbal_stats window=%" PRIu32 "ms reply_wait normal=%ums home=%ums power=%ums\r\n",
               stats.window_ms, BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS, BK7259_GIMBAL_STATS_HOME_WINDOW_MS,
               BK7259_GIMBAL_STATS_POWER_WINDOW_MS);
    stats_line(&output, "arrival_wait normal=%u..%ums home=%ums\r\n", BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS,
               BK7259_GIMBAL_ARRIVAL_MAX_MS, BK7259_GIMBAL_STATS_HOME_WINDOW_MS);
    stats_line(&output, "uart tx_guard=%ums ordinary_period=%ums\r\n",
               BK7259_GIMBAL_TX_GUARD_MS, BK7259_GIMBAL_MOTION_PERIOD_MS);
    stats_idle_gap(&output, "move_to_charge", stats.move_to_charge_pairs, stats.min_move_to_charge_idle_ms);
    stats_idle_gap(&output, "charge_to_move", stats.charge_to_move_pairs, stats.min_charge_to_move_idle_ms);
    const bk7259_gimbal_stats_counts_t *normal = &stats.groups[BK7259_GIMBAL_STATS_NORMAL];
    const bk7259_gimbal_stats_counts_t *home = &stats.groups[BK7259_GIMBAL_STATS_HOME];
    uint64_t motion_tx = (uint64_t)normal->tx + home->tx;
    uint64_t short_ok_rx = (uint64_t)stats.e1_00 + stats.e1_04;
    uint64_t accepted = (uint64_t)normal->accepted + home->accepted;
    uint64_t completion_fallback = (uint64_t)normal->completion_fallback + home->completion_fallback;
    stats_line(&output, "motion tx=%" PRIu64 " short_ok_rx=%" PRIu64
               " short_ok_matched=%" PRIu64 " center_confirmed=%" PRIu32 "\r\n",
               motion_tx, short_ok_rx, accepted - completion_fallback, stats.center_confirmed);
    stats_group(&output, "normal", normal);
    stats_group(&output, "home", home);
    stats_line(&output, "E1 00=%" PRIu32 " 01=%" PRIu32 " 02=%" PRIu32
               " 04=%" PRIu32 " 06=%" PRIu32 "\r\n",
               stats.e1_00, stats.e1_01, stats.e1_02, stats.e1_04, stats.e1_06);
    stats_line(&output, "feedback bad=%" PRIu32 " unmatched=%" PRIu32
               " ambiguous=%" PRIu32 " suppressed=%" PRIu32 "\r\n",
               stats.bad_reply, stats.unmatched, stats.ambiguous, stats.suppressed);
    stats_latency(&output, "latency", stats.latency_samples, stats.latency_min_ms,
                  stats.latency_total_ms, stats.latency_median_twice_ms, stats.latency_max_ms);
    stats_latency(&output, "arrival_latency", stats.arrival_latency_samples, stats.arrival_latency_min_ms,
                  stats.arrival_latency_total_ms, stats.arrival_latency_median_twice_ms,
                  stats.arrival_latency_max_ms);
    stats_power(&output, &stats.power);
    stats_flush(&output);
    command_reply(buffer, length, output.failed ? "gimbal_stats output incomplete" : "gimbal_stats report sent");
}

static const struct cli_command k_commands[] = {
    {"gimbal_cmd", "gimbal_cmd: send one command, cycling through four motions", gimbal_command},
    {"gimbal_scene", "gimbal_scene [happy|sad|angry|surprised]: play one full scene, then center", gimbal_scene_command},
    {"gimbal_dialogue", "gimbal_dialogue start|listening|thinking|speaking|neutral|end", gimbal_dialogue_command},
    {"gimbal_charge", "gimbal_charge on|off: control EB521 charging heartbeat", gimbal_charge_command},
    {"gimbal_probe", "gimbal_probe: check the EB521 UART link", gimbal_probe_command},
    {"gimbal_test", "gimbal_test: schedule one motion test in 10 seconds", gimbal_test_command},
    {"gimbal_stats", "gimbal_stats [reset]: read or clear UART motion statistics", gimbal_stats_command},
};

int bk7259_gimbal_cli_init(void)
{
    /* Product startup serializes registration; the command table lives forever. */
    if (s_registered)
        return 0;
    int result = cli_register_commands(k_commands, sizeof(k_commands) / sizeof(k_commands[0]));
    if (result == 0) {
        s_registered = true;
        BK_LOGI(TAG, "EB521 V1.2 commands: gimbal_dialogue, gimbal_scene, gimbal_cmd, gimbal_probe, gimbal_charge, gimbal_test, gimbal_stats\r\n");
    }
    return result;
}
#else
int bk7259_gimbal_cli_init(void)
{
    return 0;
}
#endif
