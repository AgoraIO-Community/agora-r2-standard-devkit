/* SPDX-License-Identifier: Apache-2.0 */
/* Test the actual registered CLI callbacks without UART or a motor worker. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_SHELL_ASYNCLOG 1
#include "../../ap/bk7259_gimbal_cli.c"

static const struct cli_command *registered;
static int registered_count, register_calls, register_result = -4;
static int request_result, raw_calls, scene_calls, charge_calls, probe_calls, dialogue_calls;
static bk7259_gimbal_scene_t requested_scene;
static bk7259_gimbal_phase_t requested_phase;
static bool requested_charging;
static bool requested_dialogue;
static bk7259_gimbal_stats_snapshot_t stats;
static int stats_result, stats_queries, stats_resets, test_requests;
static char console_output[4096];
static size_t console_length;
static unsigned console_chunks, console_delays;
static bool console_fail;

int cli_register_commands(const struct cli_command *commands, int count)
{
    ++register_calls;
    if (register_result == 0) {
        registered = commands;
        registered_count = count;
    }
    return register_result;
}

int bk7259_gimbal_send_next_command(void)
{
    ++raw_calls;
    return request_result;
}

int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{
    ++scene_calls;
    requested_scene = scene;
    return request_result;
}

int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase)
{
    ++dialogue_calls;
    requested_dialogue = active;
    requested_phase = phase;
    return request_result;
}

int bk7259_gimbal_set_charging(bool enabled)
{
    ++charge_calls;
    requested_charging = enabled;
    return request_result;
}

int bk7259_gimbal_request_probe(void)
{
    ++probe_calls;
    return request_result;
}

int bk7259_gimbal_schedule_test(void)
{
    ++test_requests;
    return request_result;
}

int bk7259_gimbal_get_stats(bk7259_gimbal_stats_snapshot_t *out)
{
    ++stats_queries;
    *out = stats;
    return stats_result;
}

int bk7259_gimbal_reset_stats(void)
{
    ++stats_resets;
    return stats_result;
}

int shell_log_raw_data(const uint8_t *data, uint16_t data_len)
{
    assert(data_len >= 2 && data_len <= 135);
    assert(data[data_len - 2] == '\r' && data[data_len - 1] == '\n');
    if (console_fail) {
        return 0;
    }
    assert(console_chunks == console_delays);
    ++console_chunks;
    assert(console_length + data_len < sizeof(console_output));
    memcpy(console_output + console_length, data, data_len);
    console_length += data_len;
    console_output[console_length] = '\0';
    return 1;
}

bk_err_t rtos_delay_milliseconds(uint32_t milliseconds)
{
    assert(milliseconds == 15);
    ++console_delays;
    return BK_OK;
}

static void reset_console(void)
{
    console_length = 0;
    console_output[0] = '\0';
    console_chunks = 0;
    console_delays = 0;
}

static const struct cli_command *find_command(const char *name)
{
    for (int index = 0; index < registered_count; ++index)
        if (strcmp(registered[index].name, name) == 0)
            return &registered[index];
    assert(!"missing registered command");
    return NULL;
}

static void invoke(const char *name, const char *argument, const char *extra,
                   const char *expected_reply)
{
    /* AP shell_task passes 256 bytes minus four reserved for the prompt. */
    char response[252] = {0};
    char *arguments[] = {(char *)name, (char *)argument, (char *)extra};
    int argc = extra ? 3 : argument ? 2 : 1;
    find_command(name)->function(response, sizeof(response), argc, arguments);
    assert(strstr(response, expected_reply));
}

static void test_stats_output(void)
{
    invoke("gimbal_stats", "RESET", NULL, "Usage:");
    invoke("gimbal_stats", "reset", "extra", "Usage:");
    assert(stats_queries == 0 && stats_resets == 0);
    invoke("gimbal_stats", "reset", NULL, "gimbal_stats reset");
    assert(stats_queries == 0 && stats_resets == 1);

    stats_result = -1;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats unavailable");
    invoke("gimbal_stats", "reset", NULL, "gimbal_stats unavailable");
    assert(stats_queries == 1 && stats_resets == 2 && console_length == 0);
    stats_result = 0;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "test=idle stage=none\r\n"));
    assert(strstr(console_output, "move_to_charge pairs=0 min_idle_est=n/a\r\n"));
    assert(strstr(console_output, "charge_to_move pairs=0 min_idle_est=n/a\r\n"));
    assert(strstr(console_output, "motion tx=0 short_ok_rx=0 short_ok_matched=0 center_confirmed=0\r\n"));
    assert(strstr(console_output, "normal success=n/a") && strstr(console_output, "home success=n/a"));
    assert(strstr(console_output, "arrival normal reached=0 timeout=0 rejected=0 cancelled=0 pending=0 success=n/a\r\n"));
    assert(strstr(console_output, "arrival_latency n=0 min=n/a avg=n/a median=n/a max=n/a\r\n"));
    assert(strstr(console_output, "latency n=0 min=n/a avg=n/a median=n/a max=n/a"));
    assert(strstr(console_output, "power tx=0 tx_failed=0 accepted=0 rejected=0 timeout=0 pending=0 success=n/a"));
    assert(strstr(console_output, "power_latency n=0 min=n/a avg=n/a median=n/a max=n/a"));

    reset_console();
    stats.groups[BK7259_GIMBAL_STATS_NORMAL] = (bk7259_gimbal_stats_counts_t){
        .tx = 90, .tx_failed = 2, .accepted = 64, .rejected = 4, .pending = 10,
        .expired = 8, .overflow = 4, .estimated = 12, .completion_fallback = 3,
        .arrival = {.reached = 50, .expired = 20, .rejected = 1, .cancelled = 10, .pending = 5},
    };
    stats.groups[BK7259_GIMBAL_STATS_HOME] = (bk7259_gimbal_stats_counts_t){
        .tx = 12, .accepted = 3, .rejected = 1, .pending = 4, .expired = 4, .completion_fallback = 1,
    };
    stats.window_ms = 12000;
    stats.move_to_charge_pairs = 8;
    stats.min_move_to_charge_idle_ms = 40;
    stats.charge_to_move_pairs = 7;
    stats.min_charge_to_move_idle_ms = 42;
    stats.e1_00 = 64;
    stats.e1_01 = 4;
    stats.e1_02 = 1;
    stats.e1_04 = 3;
    stats.e1_06 = 41;
    stats.center_confirmed = 2;
    stats.bad_reply = 2;
    stats.unmatched = 5;
    stats.ambiguous = 6;
    stats.suppressed = 7;
    stats.latency_samples = 4;
    stats.latency_total_ms = 35;
    stats.latency_min_ms = 3;
    stats.latency_median_twice_ms = 15;
    stats.latency_max_ms = 17;
    stats.arrival_latency_samples = 2;
    stats.arrival_latency_min_ms = 85;
    stats.arrival_latency_max_ms = 200;
    stats.arrival_latency_total_ms = 285;
    stats.arrival_latency_median_twice_ms = 285;
    stats.power.groups[BK7259_GIMBAL_POWER_STOP] = (bk7259_gimbal_power_counts_t){
        .tx = 5, .tx_failed = 1, .accepted = 3, .rejected = 0, .pending = 1, .expired = 1,
    };
    stats.power.groups[BK7259_GIMBAL_POWER_REQUEST] = (bk7259_gimbal_power_counts_t){
        .tx = 7, .tx_failed = 2, .accepted = 4, .rejected = 1, .pending = 1, .expired = 1,
    };
    stats.power.e6_00 = 7;
    stats.power.e6_02 = 1;
    stats.power.bad_reply = 2;
    stats.power.late = 3;
    stats.power.unmatched = 4;
    stats.power.suppressed = 5;
    stats.power.latency_samples = 4;
    stats.power.latency_min_ms = 3;
    stats.power.latency_total_ms = 35;
    stats.power.latency_median_twice_ms = 15;
    stats.power.latency_max_ms = 17;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strlen(console_output) > 252);
    assert(strstr(console_output, "window=12000ms reply_wait normal=250ms home=900ms"));
    assert(strstr(console_output, "arrival_wait normal=250..2000ms home=900ms\r\n"));
    assert(strstr(console_output, "uart tx_guard=40ms ordinary_period=100ms\r\n"));
    assert(strstr(console_output, "move_to_charge pairs=8 min_idle_est=40ms\r\n"));
    assert(strstr(console_output, "charge_to_move pairs=7 min_idle_est=42ms\r\n"));
    assert(strstr(console_output, "motion tx=102 short_ok_rx=67 short_ok_matched=63 center_confirmed=2\r\n"));
    assert(strstr(console_output, "normal tx=90 tx_failed=2 accepted=64 rejected=4 timeout=8 pending=10"));
    assert(strstr(console_output, "normal success=80.0% overflow=4 estimated=12 completion_fallback=3"));
    assert(strstr(console_output, "arrival normal reached=50 timeout=20 rejected=1 cancelled=10 pending=5 success=66.6%\r\n"));
    assert(strstr(console_output, "arrival_latency n=2 min=85 avg=142.5 median=142.5 max=200 ms\r\n"));
    assert(strstr(console_output, "home tx=12 tx_failed=0 accepted=3 rejected=1 timeout=4 pending=4"));
    assert(strstr(console_output, "home success=37.5%"));
    assert(strstr(console_output, "E1 00=64 01=4 02=1 04=3 06=41"));
    assert(strstr(console_output, "feedback bad=2 unmatched=5 ambiguous=6 suppressed=7"));
    assert(strstr(console_output, "latency n=4 min=3 avg=8.8 median=7.5 max=17 ms"));
    assert(strstr(console_output, "power tx=12 tx_failed=3 accepted=7 rejected=1 timeout=2 pending=2 success=70.0%"));
    assert(strstr(console_output, "power 00 tx=5 tx_failed=1 accepted=3 rejected=0 timeout=1 pending=1"));
    assert(strstr(console_output, "power 01 tx=7 tx_failed=2 accepted=4 rejected=1 timeout=1 pending=1"));
    assert(strstr(console_output, "power E6 matched_00=7 matched_02=1 bad=2 late=3 unmatched=4 suppressed=5\r\n"));
    assert(strstr(console_output, "power_latency n=4 min=3 avg=8.8 median=7.5 max=17 ms"));

    reset_console();
    stats.latency_samples = 20;
    stats.latency_total_ms = 199;
    stats.latency_min_ms = 9;
    stats.latency_median_twice_ms = 20;
    stats.latency_max_ms = 10;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "latency n=20 min=9 avg=10.0 median=10.0 max=10 ms"));

    reset_console();
    stats.groups[BK7259_GIMBAL_STATS_NORMAL] = (bk7259_gimbal_stats_counts_t){
        .tx = UINT32_MAX, .tx_failed = UINT32_MAX, .accepted = UINT32_MAX,
        .rejected = UINT32_MAX, .pending = UINT32_MAX, .expired = UINT32_MAX,
        .overflow = UINT32_MAX, .estimated = UINT32_MAX, .completion_fallback = UINT32_MAX,
    };
    stats.groups[BK7259_GIMBAL_STATS_HOME] = (bk7259_gimbal_stats_counts_t){
        .tx = UINT32_MAX, .accepted = UINT32_MAX,
    };
    stats.e1_00 = UINT32_MAX;
    stats.e1_04 = UINT32_MAX;
    stats.center_confirmed = UINT32_MAX;
    stats.latency_samples = 1;
    stats.latency_total_ms = UINT64_MAX;
    stats.latency_min_ms = UINT32_MAX;
    stats.latency_median_twice_ms = UINT32_MAX;
    stats.latency_max_ms = UINT32_MAX;
    stats.power.groups[BK7259_GIMBAL_POWER_STOP] = (bk7259_gimbal_power_counts_t){
        .tx = UINT32_MAX, .tx_failed = UINT32_MAX, .accepted = UINT32_MAX,
        .rejected = UINT32_MAX, .pending = UINT32_MAX, .expired = UINT32_MAX,
    };
    stats.power.groups[BK7259_GIMBAL_POWER_REQUEST] = stats.power.groups[BK7259_GIMBAL_POWER_STOP];
    stats.power.e6_00 = UINT32_MAX;
    stats.power.e6_02 = UINT32_MAX;
    stats.power.bad_reply = UINT32_MAX;
    stats.power.late = UINT32_MAX;
    stats.power.unmatched = UINT32_MAX;
    stats.power.suppressed = UINT32_MAX;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "normal success=25.0% overflow=4294967295 estimated=4294967295"));
    assert(strstr(console_output, "motion tx=8589934590 short_ok_rx=8589934590 "
                                  "short_ok_matched=4294967295 center_confirmed=4294967295\r\n"));
    assert(strstr(console_output, "latency n=1 min=4294967295 avg=18446744073709551615.0 "
                                  "median=2147483647.5 max=4294967295 ms\r\n"));
    assert(strstr(console_output, "power tx=8589934590 tx_failed=8589934590 accepted=8589934590 "
                                  "rejected=8589934590 timeout=8589934590 pending=8589934590 success=33.3%\r\n"));
    assert(strstr(console_output, "power E6 matched_00=4294967295 matched_02=4294967295 "
                                  "bad=4294967295 late=4294967295 unmatched=4294967295 suppressed=4294967295\r\n"));
    reset_console();
    console_fail = true;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats output incomplete");
    assert(stats_queries == 6 && stats_resets == 2);
    console_fail = false;
}

static void test_scheduled_test_output(void)
{
    invoke("gimbal_test", "0", NULL, "Usage: gimbal_test");
    invoke("gimbal_test", "10", NULL, "Usage: gimbal_test");
    invoke("gimbal_test", "10", "extra", "Usage: gimbal_test");
    assert(test_requests == 0);
    request_result = 0;
    reset_console();
    invoke("gimbal_test", NULL, NULL, "scheduled in 10s. You can unplug Type-C now.");
    assert(test_requests == 1 && console_length == 0 && console_delays == 0);
    request_result = BK7259_GIMBAL_BUSY;
    invoke("gimbal_test", NULL, NULL, "gimbal busy");
    invoke("gimbal_cmd", NULL, NULL, "gimbal busy");
    invoke("gimbal_scene", "happy", NULL, "gimbal busy");
    invoke("gimbal_dialogue", "start", NULL, "gimbal busy");
    request_result = BK7259_GIMBAL_NOT_READY;
    invoke("gimbal_test", NULL, NULL, "not ready");
    request_result = BK7259_GIMBAL_MOTION_DISABLED;
    invoke("gimbal_test", NULL, NULL, "disabled by settings");
    request_result = -1;
    invoke("gimbal_test", NULL, NULL, "unavailable");
    assert(test_requests == 5);

    stats_result = BK7259_GIMBAL_BUSY;
    invoke("gimbal_stats", "reset", NULL, "gimbal_stats busy: test scheduled or running");
    stats_result = 0;
    stats = (bk7259_gimbal_stats_snapshot_t){.test_state = BK7259_GIMBAL_TEST_SCHEDULED};
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "test=scheduled stage=none\r\n"));

    reset_console();
    stats.test_state = BK7259_GIMBAL_TEST_RUNNING;
    stats.test_stage = BK7259_GIMBAL_TEST_SPEAKING;
    stats.move_to_charge_pairs = 1;
    stats.min_move_to_charge_idle_ms = 40;
    stats.test_counts[BK7259_GIMBAL_TEST_SPEAKING] = (bk7259_gimbal_test_counts_t){
        .tx = 45, .accepted = 31, .rejected = 2, .expired = 8, .overflow = 4, .tx_failed = 3,
        .motion_pairs = 44, .min_motion_gap_ms = 60,
        .arrival = {.reached = 27, .expired = 7, .rejected = 2, .cancelled = 5},
    };
    stats.test_counts[BK7259_GIMBAL_TEST_ANGRY] = (bk7259_gimbal_test_counts_t){
        .tx = UINT32_MAX, .accepted = UINT32_MAX, .rejected = UINT32_MAX,
        .expired = UINT32_MAX, .overflow = UINT32_MAX, .tx_failed = UINT32_MAX,
        .motion_pairs = UINT32_MAX, .min_motion_gap_ms = UINT32_MAX,
    };
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "test=running stage=speaking\r\n"));
    assert(strstr(console_output, "uart tx_guard=40ms ordinary_period=100ms\r\n"));
    assert(strstr(console_output, "move_to_charge pairs=1 min_idle_est=40ms\r\n"));
    assert(strstr(console_output, "charge_to_move pairs=0 min_idle_est=n/a\r\n"));
    assert(strstr(console_output, "stage speaking tx=45 accepted=31 rejected=2 timeout=8 overflow=4 tx_failed=3\r\n"));
    assert(strstr(console_output, "stage speaking motion_pairs=44 min_motion_gap=60ms\r\n"));
    assert(strstr(console_output, "arrival speaking reached=27 timeout=7 rejected=2 cancelled=5 pending=0 success=67.5%\r\n"));
    assert(strstr(console_output, "stage hangup motion_pairs=0 min_motion_gap=n/a\r\n"));
    assert(strstr(console_output, "stage angry tx=4294967295 accepted=4294967295 rejected=4294967295 "
                                  "timeout=4294967295 overflow=4294967295 tx_failed=4294967295\r\n"));
    assert(strstr(console_output, "stage angry motion_pairs=4294967295 min_motion_gap=4294967295ms\r\n"));
    assert(strstr(console_output, "stage opening ") && strstr(console_output, "stage listening ") &&
           strstr(console_output, "stage thinking ") && strstr(console_output, "stage neutral ") &&
           strstr(console_output, "stage hangup ") && strstr(console_output, "stage happy ") &&
           strstr(console_output, "stage sad ") && strstr(console_output, "stage surprised "));
    assert(console_chunks > 8 && console_delays + 1 == console_chunks);

    reset_console();
    stats.test_state = BK7259_GIMBAL_TEST_COMPLETED;
    stats.test_stage = BK7259_GIMBAL_TEST_SURPRISED;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "test=completed stage=surprised\r\n"));
    reset_console();
    stats.test_state = BK7259_GIMBAL_TEST_INTERRUPTED;
    stats.test_stage = BK7259_GIMBAL_TEST_NONE;
    invoke("gimbal_stats", NULL, NULL, "gimbal_stats report sent");
    assert(strstr(console_output, "test=interrupted stage=none\r\n"));
}

static void expect_scene(const char *argument, int result,
                         bk7259_gimbal_scene_t expected)
{
    const int before = scene_calls;
    request_result = result;
    const char *reply = result == 0 ? "queued" : "unavailable";
    if (result == BK7259_GIMBAL_NOT_READY) {
        reply = "not ready";
    } else if (result == BK7259_GIMBAL_MOTION_DISABLED) {
        reply = "disabled by settings";
    }
    invoke("gimbal_scene", argument, NULL, reply);
    assert(scene_calls == before + 1 && requested_scene == expected);
}

int main(void)
{
    assert(bk7259_gimbal_cli_init() == -4 && register_calls == 1);
    register_result = 0;
    assert(bk7259_gimbal_cli_init() == 0 && register_calls == 2);
    assert(bk7259_gimbal_cli_init() == 0 && register_calls == 2);
    assert(registered_count == 7);

    invoke("gimbal_cmd", "happy", NULL, "Usage:");
    assert(raw_calls == 0 && scene_calls == 0);
    const int results[] = {0, BK7259_GIMBAL_NOT_READY, BK7259_GIMBAL_MOTION_DISABLED, -1};
    const char *const replies[] = {"queued", "not ready", "disabled by settings", "unavailable"};
    for (size_t index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
        request_result = results[index];
        invoke("gimbal_cmd", NULL, NULL, replies[index]);
        assert(raw_calls == (int)index + 1 && scene_calls == 0);
    }
    expect_scene(NULL, BK7259_GIMBAL_NOT_READY, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene(NULL, BK7259_GIMBAL_MOTION_DISABLED, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene(NULL, -1, BK7259_GIMBAL_SCENE_HAPPY);
    invoke("gimbal_scene", "Happy", NULL, "Usage:");
    invoke("gimbal_scene", "happy", "sad", "Usage:");
    assert(scene_calls == 3);
    for (int index = 0; index < 5; ++index)
        expect_scene(NULL, 0, (bk7259_gimbal_scene_t)(index % 4));

    expect_scene("angry", BK7259_GIMBAL_NOT_READY, BK7259_GIMBAL_SCENE_ANGRY);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_SAD);
    expect_scene("surprised", 0, BK7259_GIMBAL_SCENE_SURPRISED);
    expect_scene("sad", -1, BK7259_GIMBAL_SCENE_SAD);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene("happy", 0, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene("sad", 0, BK7259_GIMBAL_SCENE_SAD);
    expect_scene("angry", 0, BK7259_GIMBAL_SCENE_ANGRY);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_SURPRISED);
    assert(raw_calls == 4);

    invoke("gimbal_probe", "extra", NULL, "Usage:");
    assert(probe_calls == 0);
    const int probe_results[] = {0, -2, -1};
    const char *const probe_replies[] = {"queued", "busy", "unavailable"};
    for (int index = 0; index < 3; ++index) {
        request_result = probe_results[index];
        invoke("gimbal_probe", NULL, NULL, probe_replies[index]);
        assert(probe_calls == index + 1);
    }

    invoke("gimbal_dialogue", NULL, NULL, "Usage:");
    invoke("gimbal_dialogue", "Listening", NULL, "Usage:");
    invoke("gimbal_dialogue", "start", "end", "Usage:");
    assert(dialogue_calls == 0);
    const struct {
        const char *argument;
        bool active;
        bk7259_gimbal_phase_t phase;
    } dialogue[] = {
        {"start", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"neutral", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"listening", true, BK7259_GIMBAL_PHASE_LISTENING},
        {"thinking", true, BK7259_GIMBAL_PHASE_THINKING},
        {"speaking", true, BK7259_GIMBAL_PHASE_SPEAKING},
        {"end", false, BK7259_GIMBAL_PHASE_NEUTRAL},
    };
    request_result = 0;
    for (size_t i = 0; i < sizeof(dialogue) / sizeof(dialogue[0]); ++i) {
        invoke("gimbal_dialogue", dialogue[i].argument, NULL, "queued");
        assert(dialogue_calls == (int)i + 1);
        assert(requested_dialogue == dialogue[i].active && requested_phase == dialogue[i].phase);
    }
    request_result = BK7259_GIMBAL_NOT_READY;
    invoke("gimbal_dialogue", "start", NULL, "not ready");
    request_result = BK7259_GIMBAL_MOTION_DISABLED;
    invoke("gimbal_dialogue", "thinking", NULL, "disabled by settings");
    assert(requested_dialogue && requested_phase == BK7259_GIMBAL_PHASE_THINKING);
    invoke("gimbal_dialogue", "end", NULL, "disabled by settings");
    assert(!requested_dialogue);
    request_result = -1;
    invoke("gimbal_dialogue", "end", NULL, "unavailable");

    invoke("gimbal_charge", NULL, NULL, "Usage:");
    invoke("gimbal_charge", "ON", NULL, "Usage:");
    invoke("gimbal_charge", "on", "off", "Usage:");
    assert(charge_calls == 0);
    request_result = 0;
    invoke("gimbal_charge", "on", NULL, "queued");
    assert(charge_calls == 1 && requested_charging);
    invoke("gimbal_charge", "off", NULL, "queued");
    assert(charge_calls == 2 && !requested_charging);
    request_result = -1;
    invoke("gimbal_charge", "on", NULL, "unavailable");
    assert(charge_calls == 3 && requested_charging);

    test_stats_output();
    test_scheduled_test_output();
    puts("PASS: CLI registration, motion, charging, and UART statistics output");
    return 0;
}
