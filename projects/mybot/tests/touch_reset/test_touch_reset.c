/* SPDX-License-Identifier: Apache-2.0 */
#include "host.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Exercise the actual request mailbox, GPIO gestures and application owner.
 * Only hardware, storage and SDK lifecycle calls are fault-injected. */
#include "bk7259_factory_reset.c"
#undef TAG
#include "bk7259_key.c"
#undef TAG
#define mybot_start host_sdk_start
#define mybot_stop host_sdk_stop
#define mybot_is_running host_sdk_running
#define mybot_get_state host_sdk_state
#define main host_firmware_main
#include "../../ap/ap_main.c"
#undef main
#undef mybot_start
#undef mybot_stop
#undef mybot_is_running
#undef mybot_get_state

enum { EXIT_REBOOT = 1, EXIT_IDLE };
enum { INTENT, AUTH, WIFI, VOLUME, UNRELATED, RECORDS };
static const char *const record_names[RECORDS] = {
    "mybot.factory_reset.v1", "mybot.device_auth",  "mybot.wifi.v1",
    "mybot.volume.v1",        "vendor.calibration",
};
typedef struct {
  bool present;
  struct env_node_obj metadata;
  unsigned char bytes[64];
} host_record_t;
static host_record_t records[RECORDS];
static jmp_buf exit_point;
static uint32_t clock_ms;
static int gpio_level;
static mybot_state_t sdk_state;
static bool sdk_running, network_stopped, stop_leaves_running;
static unsigned stop_failures, portal_failures, sta_failures, write_failures;
static int delete_failure;
static unsigned idle_polls, touch_at_poll, stop_calls, start_calls,
    network_calls;
static unsigned writes, deletes, reboot_calls, failures, conversation_events;
static bool fail_start, fail_network;
static char events[128];
static size_t event_length;
volatile const char build_version[] = "touch-reset-host";

static void record(char event) {
  assert(event_length + 1 < sizeof(events));
  events[event_length++] = event;
  events[event_length] = '\0';
}
static int record_index(const char *name) {
  for (int i = 0; i < RECORDS; ++i)
    if (!strcmp(name, record_names[i]))
      return i;
  assert(!"unexpected persistent key");
  return -1;
}
static void store(int index, const void *bytes, size_t length) {
  assert(length <= sizeof(records[index].bytes));
  host_record_t *entry = &records[index];
  memset(entry, 0, sizeof(*entry));
  entry->present = true;
  entry->metadata.status = ENV_WRITE;
  entry->metadata.crc_is_ok = true;
  entry->metadata.name_len = (uint8_t)strlen(record_names[index]);
  entry->metadata.value_len = (uint32_t)length;
  memcpy(entry->metadata.name, record_names[index], entry->metadata.name_len);
  memcpy(entry->bytes, bytes, length);
}
static void fixture(void) {
  if (s_key.prepared)
    bk7259_key_shutdown();
  (void)bk7259_factory_reset_take_request();
  memset(records, 0, sizeof(records));
  const unsigned char data[] = {1, 2, 3, 4};
  for (int i = AUTH; i < RECORDS; ++i)
    store(i, data, sizeof(data));
  clock_ms = 0;
  gpio_level = 1;
  sdk_state = MYBOT_STATE_STOPPED;
  sdk_running = network_stopped = stop_leaves_running = false;
  stop_failures = portal_failures = sta_failures = write_failures = 0;
  delete_failure = -1;
  idle_polls = touch_at_poll = stop_calls = start_calls = network_calls = 0;
  writes = deletes = reboot_calls = failures = conversation_events = 0;
  fail_start = fail_network = false;
  event_length = 0;
  events[0] = '\0';
  s_display_prepared = false;
}

void host_log(const char *format, ...) { (void)format; }
int rtos_init_mutex(beken_mutex_t *value) {
  *value = (void *)1;
  return 0;
}
int rtos_deinit_mutex(beken_mutex_t *value) {
  *value = NULL;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t *value) {
  assert(*value);
  return 0;
}
int rtos_unlock_mutex(beken_mutex_t *value) {
  assert(*value);
  return 0;
}
int rtos_init_semaphore(beken_semaphore_t *value, int count) {
  assert(count == 1);
  *value = (void *)1;
  return 0;
}
int rtos_deinit_semaphore(beken_semaphore_t *value) {
  *value = NULL;
  return 0;
}
int rtos_get_semaphore(beken_semaphore_t *value, uint32_t timeout) {
  (void)value;
  (void)timeout;
  assert(!"unexpected in-flight callback");
  return -1;
}
int rtos_set_semaphore(beken_semaphore_t *value) {
  assert(*value);
  return 0;
}
int rtos_init_timer(beken_timer_t *timer, uint32_t period, void (*fn)(void *),
                    void *arg) {
  assert(period == 20);
  *timer = (beken_timer_t){.callback = fn, .arg = arg};
  return 0;
}
int rtos_start_timer(beken_timer_t *timer) {
  timer->started = true;
  return 0;
}
int rtos_stop_timer(beken_timer_t *timer) {
  timer->started = false;
  return 0;
}
int rtos_deinit_timer(beken_timer_t *timer) {
  *timer = (beken_timer_t){0};
  return 0;
}
uint32_t rtos_get_time(void) { return clock_ms; }
int rtos_delay_milliseconds(uint32_t ms) {
  clock_ms += ms;
  if (ms == MYBOT_POLL_INTERVAL_MS) {
    ++idle_polls;
    if (idle_polls == touch_at_poll)
      bk7259_factory_reset_request();
    else if (idle_polls >= 4)
      longjmp(exit_point, EXIT_IDLE);
  }
  return 0;
}
int bk_gpio_get_input(gpio_id_t gpio) {
  assert(gpio == 47);
  return gpio_level;
}
int bk_gpio_disable_output(gpio_id_t gpio) {
  assert(gpio == 47);
  return 0;
}
int bk_gpio_enable_input(gpio_id_t gpio) {
  assert(gpio == 47);
  return 0;
}
int bk_gpio_enable_pull(gpio_id_t gpio) {
  assert(gpio == 47);
  return 0;
}
int bk_gpio_pull_up(gpio_id_t gpio) {
  assert(gpio == 47);
  return 0;
}
int bk_gpio_pull_down(gpio_id_t gpio) {
  (void)gpio;
  assert(false);
  return -1;
}
int bk_init(void) { return 0; }
int bk_uid_get_data(uint8_t *uid) {
  memset(uid, 0x21, 32);
  return 0;
}
int media_service_init(void) { return 0; }
void aosl_ctor(void) {}
void aosl_dtor(void) {}
void aosl_set_log_level(int level) { (void)level; }
int rtos_delete_thread(beken_thread_t *thread) {
  (void)thread;
  return 0;
}
int rtos_create_psram_thread(beken_thread_t *thread, uint8_t priority,
                             const char *name, beken_thread_function_t fn,
                             uint32_t stack, beken_thread_arg_t arg) {
  (void)thread;
  (void)priority;
  (void)name;
  (void)fn;
  (void)stack;
  (void)arg;
  return 0;
}
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) {
  static const mbedtls_md_info_t info = {0};
  assert(type == MBEDTLS_MD_SHA256);
  return &info;
}
int mbedtls_md_hmac(const mbedtls_md_info_t *info, const unsigned char *key,
                    size_t key_length, const unsigned char *input,
                    size_t length, unsigned char *output) {
  (void)info;
  (void)key;
  (void)key_length;
  (void)input;
  (void)length;
  memset(output, 0x42, 32);
  return 0;
}

EfErrCode easyflash_init(void) { return EF_NO_ERR; }
bool ef_get_env_obj(const char *name, env_node_obj_t node) {
  host_record_t *entry = &records[record_index(name)];
  if (!entry->present)
    return false;
  *node = entry->metadata;
  return true;
}
size_t ef_get_env_blob(const char *name, void *out, size_t capacity,
                       size_t *saved) {
  host_record_t *entry = &records[record_index(name)];
  assert(entry->present && capacity >= entry->metadata.value_len);
  *saved = entry->metadata.value_len;
  memcpy(out, entry->bytes, *saved);
  return *saved;
}
EfErrCode ef_set_env_blob(const char *name, const void *bytes, size_t length) {
  assert(record_index(name) == INTENT);
  ++writes;
  record('B');
  if (write_failures) {
    --write_failures;
    return EF_WRITE_ERR;
  }
  store(INTENT, bytes, length);
  return EF_NO_ERR;
}
EfErrCode ef_del_env(const char *name) {
  int index = record_index(name);
  assert(index != UNRELATED);
  assert(!sdk_running && sdk_state == MYBOT_STATE_STOPPED && network_stopped);
  ++deletes;
  record("xawv"[index]);
  if (index == delete_failure) {
    delete_failure = -1;
    return EF_ERASE_ERR;
  }
  records[index].present = false;
  return EF_NO_ERR;
}
int bk7259_wifi_portal_stop(void) {
  record('P');
  if (portal_failures) {
    --portal_failures;
    return -1;
  }
  return 0;
}
int bk7259_wifi_manager_disconnect(void) {
  record('D');
  if (sta_failures) {
    --sta_failures;
    return -1;
  }
  network_stopped = true;
  return 0;
}
int bk7259_lcd_show_factory_reset(bk7259_factory_reset_screen_t screen) {
  if (screen == BK7259_FACTORY_RESET_FAILED)
    ++failures;
  return 0;
}
int bk7259_lcd_set_runtime_status(const char *state, bool active) {
  (void)state;
  (void)active;
  return 0;
}
int bk7259_gimbal_cli_init(void) { return 0; }
int mybot_bk7259_platform_prepare(void) {
  record('L');
  return bk7259_key_prepare();
}
int mybot_bk7259_platform_register(void) {
  record('G');
  return 0;
}
void mybot_bk7259_platform_shutdown(void) {
  assert(!"reset must keep product UI alive");
}
int mybot_bk7259_ensure_network(const char *device_id) {
  assert(device_id[0]);
  ++network_calls;
  record('N');
  if (bk7259_factory_reset_requested())
    return BK7259_NETWORK_CANCELLED;
  return fail_network ? -1 : 0;
}
int host_sdk_start(const mybot_config_t *config) {
  assert(config->device_id[0]);
  ++start_calls;
  record('A');
  if (fail_start)
    return -1;
  sdk_state = MYBOT_STATE_READY;
  sdk_running = true;
  return 0;
}
void host_sdk_stop(void) {
  ++stop_calls;
  record('S');
  if (stop_failures) {
    --stop_failures;
    sdk_running = stop_leaves_running;
    sdk_state =
        stop_leaves_running ? MYBOT_STATE_STOPPED : MYBOT_STATE_STOPPING;
  } else {
    sdk_running = false;
    sdk_state = MYBOT_STATE_STOPPED;
  }
}
bool host_sdk_running(void) { return sdk_running; }
mybot_state_t host_sdk_state(void) { return sdk_state; }
void bk_reboot(void) {
  ++reboot_calls;
  record('R');
  longjmp(exit_point, EXIT_REBOOT);
}

static void key_event(mybot_key_event_t event, void *user) {
  assert(user == &conversation_events);
  assert(event == MYBOT_KEY_EVENT_CONVERSATION_START ||
         event == MYBOT_KEY_EVENT_CONVERSATION_STOP);
  ++conversation_events;
}
static void attach_key(void) {
  void *ctx = NULL;
  assert(bk7259_key_prepare() == 0);
  assert(g_mybot_bk7259_key_ops.init(&ctx, key_event, &conversation_events) ==
         0);
  assert(ctx == &s_key);
}
static void sample(int level, uint32_t time) {
  gpio_level = level;
  clock_ms = time;
  assert(s_key.scan_timer.started);
  s_key.scan_timer.callback(s_key.scan_timer.arg);
}
static void press_and_release(uint32_t start, uint32_t duration) {
  sample(0, start);
  sample(0, start + 20);
  sample(1, start + duration);
  sample(1, start + duration + 20);
}
static int run_owner(bool boot) {
  int reason = setjmp(exit_point);
  if (!reason) {
    if (boot)
      (void)mybot_run();
    else
      mybot_factory_reset_loop(false);
    assert(!"product owner unexpectedly returned");
  }
  return reason;
}
static void assert_erased(void) {
  for (int i = INTENT; i <= VOLUME; ++i)
    assert(!records[i].present);
  assert(records[UNRELATED].present && records[UNRELATED].bytes[0] == 1);
}

static void test_request_is_nonblocking_and_independent(void) {
  fixture();
  bk7259_factory_reset_request();
  bk7259_factory_reset_request();
  assert(bk7259_factory_reset_requested());
  assert(!writes && !deletes && !stop_calls && !clock_ms);
  assert(bk7259_key_prepare() == 0 && bk7259_factory_reset_requested());
  bk7259_key_shutdown();
  assert(bk7259_factory_reset_requested() && bk7259_factory_reset_requested());
  assert(bk7259_factory_reset_take_request() &&
         !bk7259_factory_reset_take_request());
}
static void test_key_gestures_and_reset_priority(void) {
  fixture();
  attach_key();
  sdk_state = MYBOT_STATE_READY;
  sample(0, 100);
  sample(1, 110);
  sample(1, 130);
  assert(!bk7259_key_take_conversation_request()); /* debounce rejected */
  press_and_release(200, 100);
  mybot_poll_conversation_key();
  assert(conversation_events == 1 && !bk7259_factory_reset_requested());
  sample(0, 1000);
  sample(0, 1020);
  sample(0, 6000);
  sample(1, 6100);
  sample(1, 6120);
  assert(!bk7259_key_take_conversation_request() &&
         !bk7259_factory_reset_requested());
  press_and_release(7000,
                    5000); /* exact boundary on release, without a held scan */
  assert(!bk7259_key_take_conversation_request() &&
         !bk7259_factory_reset_requested());
  press_and_release(13000, 4999);
  assert(bk7259_key_take_conversation_request());
  press_and_release(19000, 100);
  bk7259_factory_reset_request();
  mybot_poll_conversation_key();
  bk7259_key_dispatch_conversation(true);
  assert(conversation_events == 1 && bk7259_factory_reset_requested());
}
static void test_key_boot_hold_and_wraparound(void) {
  fixture();
  gpio_level = 0;
  attach_key();
  sample(0, 6000);
  sample(1, 6100);
  sample(1, 6120);
  assert(!bk7259_key_take_conversation_request() &&
         !bk7259_factory_reset_requested());
  press_and_release(UINT32_MAX - 40, 100);
  assert(bk7259_key_take_conversation_request());
  g_mybot_bk7259_key_ops.destroy(&s_key);
  press_and_release(200, 100);
  assert(!bk7259_key_take_conversation_request());
}
static void test_live_reset_order_and_deduplication(void) {
  fixture();
  sdk_running = true;
  sdk_state = MYBOT_STATE_IN_CONVERSATION;
  bk7259_factory_reset_request();
  bk7259_factory_reset_request();
  assert(run_owner(false) == EXIT_REBOOT);
  assert(!strcmp(events, "BSPDawvxR") && writes == 1 && stop_calls == 1);
  assert(reboot_calls == 1 && !failures);
  assert_erased();
}
static void test_failures_wait_for_fresh_touch(void) {
  for (int fault = 0; fault < 6; ++fault) {
    fixture();
    sdk_running = true;
    sdk_state = MYBOT_STATE_IN_CONVERSATION;
    stop_failures = fault < 2;
    stop_leaves_running = fault == 1;
    write_failures = fault == 2;
    portal_failures = fault == 3;
    sta_failures = fault == 4;
    delete_failure = fault == 5 ? WIFI : -1;
    bk7259_factory_reset_request();
    assert(run_owner(false) == EXIT_IDLE);
    assert(failures == 1 && !reboot_calls && stop_calls == 1 &&
           idle_polls == 4);
    if (fault != 5)
      assert(!deletes);
    if (fault != 2)
      assert(records[INTENT].present);
    /* Waiting does not auto-retry. A new confirmed UI action resumes. */
    idle_polls = 0;
    touch_at_poll = 2;
    assert(run_owner(false) == EXIT_REBOOT && idle_polls == 2);
    assert(stop_calls == 2 && reboot_calls == 1);
    assert_erased();
  }
}
static void test_boot_resume_and_invalid_marker(void) {
  fixture();
  assert(bk7259_factory_reset_begin() == 0);
  event_length = 0;
  events[0] = '\0';
  assert(run_owner(true) == EXIT_REBOOT);
  assert(!network_calls && !start_calls && !strcmp(events, "LSPDawvxR"));
  assert_erased();

  fixture();
  const unsigned char invalid = 0;
  store(INTENT, &invalid, 1);
  assert(run_owner(true) == EXIT_IDLE);
  assert(!start_calls && !network_calls && !writes && !deletes &&
         failures == 1);
  idle_polls = 0;
  touch_at_poll = 2;
  assert(run_owner(false) == EXIT_REBOOT && idle_polls == 2);
  assert_erased();
}
static void test_startup_failure_and_cancel_keep_touch_entry(void) {
  for (int fault = 0; fault < 2; ++fault) {
    fixture();
    fail_start = fault == 1;
    fail_network = fault == 0;
    touch_at_poll = 2;
    assert(run_owner(true) == EXIT_REBOOT);
    assert(start_calls == (unsigned)fault && idle_polls == 2);
    assert_erased();
  }
  fixture();
  bk7259_factory_reset_request();
  assert(run_owner(true) == EXIT_REBOOT && !start_calls);
  assert(network_calls == 1 && strstr(events, "NBSPD") != NULL);
  assert_erased();
}

int main(void) {
  test_request_is_nonblocking_and_independent();
  test_key_gestures_and_reset_priority();
  test_key_boot_hold_and_wraparound();
  test_live_reset_order_and_deduplication();
  test_failures_wait_for_fresh_touch();
  test_boot_resume_and_invalid_marker();
  test_startup_failure_and_cancel_keep_touch_entry();
  fixture();
  puts("Touch reset: request mailbox, GPIO gestures, durable owner and failure "
       "recovery passed");
  return 0;
}
