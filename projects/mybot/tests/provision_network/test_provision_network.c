/* SPDX-License-Identifier: Apache-2.0 */
#include "test_env.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Run the production mailbox, manager, HTTP parser and portal as one graph. */
#include "bk7259_provision.c"
#include "bk7259_wifi.c"
#undef TAG
#define secure_zero portal_secure_zero
#include "bk7259_wifi_portal.c"
#undef secure_zero

static unsigned active_screens, finishing_screens;
static bool late_cancel_accepted;
int bk7259_lcd_show_provision(bk7259_provision_screen_t screen) {
  if (screen == BK7259_PROVISION_ACTIVE)
    ++active_screens;
  if (screen == BK7259_PROVISION_FINISHING)
    ++finishing_screens;
  return 0;
}
static void connect_driver(void) {
  test_env.connected = true;
  strcpy(test_env.connected_ssid, test_env.sta_config.ssid);
}
static void cancel_request(void) { assert(bk7259_provision_cancel()); }
static void explicit_request(void) { assert(bk7259_provision_request()); }
static void disconnect_and_finish(void) {
  test_env.reset = false;
  test_env.sta_stop_result = test_env.scan_stop_result =
      test_env.ap_stop_result = 0;
  if (s_softap_started)
    assert(bk7259_wifi_portal_stop() == 0);
  if (s_wifi.prepared)
    bk7259_wifi_shutdown();
  (void)bk7259_provision_take_request();
  bk7259_provision_finish();
  assert(!bk7259_provision_busy() && !s_wifi.lock);
}
static void fixture(void) {
  disconnect_and_finish();
  test_env_reset();
  assert(bk7259_wifi_prepare() == 0);
  test_env.on_sta_start = connect_driver;
  wifi_credential_record_t saved = {
      .magic = WIFI_CREDENTIAL_MAGIC,
      .version = WIFI_CREDENTIAL_VERSION,
      .record_size = sizeof(saved),
      .ssid_length = 3,
      .password_length = 8,
      .ssid = "old",
      .password = "old-pass",
  };
  memcpy(test_env.saved_credentials, &saved, sizeof(saved));
  test_env.saved_length = sizeof(saved);
  active_screens = finishing_screens = 0;
  late_cancel_accepted = false;
}
static void assert_saved(const char *ssid) {
  wifi_credential_record_t saved;
  assert(load_credentials(&saved) == 0 && strcmp(saved.ssid, ssid) == 0);
}
static void connect_candidate(void) {
  assert(bk7259_wifi_manager_connect_candidate("new", "secret-password",
                                               1000) == 0);
}
static portal_http_request_t submission(void) {
  portal_http_request_t request = {.has_content_length = true,
                                   .content_type_json = true};
  strcpy(request.body, "{\"ssid\":\"new\",\"password\":\"secret-password\"}");
  request.body_length = strlen(request.body);
  return request;
}
static void test_request_and_priority_contract(void) {
  fixture();
  assert(!bk7259_provision_busy() && !bk7259_provision_cancel());
  assert(bk7259_provision_request() && !bk7259_provision_request());
  assert(bk7259_provision_abort_reason() == -4 &&
         bk7259_provision_begin() == -4);
  bk7259_provision_finish(); /* Never discard an unclaimed new request. */
  assert(bk7259_provision_abort_reason() == -4);
  assert(bk7259_provision_cancel() && bk7259_provision_begin() == -3);
  bk7259_provision_finish();
  assert(bk7259_provision_busy() && bk7259_provision_take_request());
  assert(bk7259_provision_abort_reason() == -3);
  test_env.reset = true;
  assert(bk7259_provision_abort_reason() == -2);
  assert(!bk7259_provision_request() && !bk7259_provision_cancel());
  assert(!bk7259_provision_try_commit());
  test_env.reset = false;
  bk7259_provision_finish();
  assert(bk7259_provision_begin() == 1 && bk7259_provision_begin() == 0);
  assert(!bk7259_provision_take_request() && bk7259_provision_try_commit());
  assert(!bk7259_provision_cancel() && bk7259_provision_abort_reason() == 0);
  bk7259_provision_commit_failed();
  assert(bk7259_provision_cancel() && bk7259_provision_abort_reason() == -3);
}

typedef struct {
  pthread_barrier_t *barrier;
  bool accepted;
  bool commit;
} race_t;
static void *race_worker(void *arg) {
  race_t *race = arg;
  int result = pthread_barrier_wait(race->barrier);
  assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
  race->accepted =
      race->commit ? bk7259_provision_try_commit() : bk7259_provision_cancel();
  return NULL;
}
static void test_cancel_commit_cas_race(void) {
  fixture();
  for (unsigned iteration = 0; iteration < 256; ++iteration) {
    bk7259_provision_finish();
    assert(bk7259_provision_begin() == 1);
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    race_t cancel = {.barrier = &barrier},
           commit = {.barrier = &barrier, .commit = true};
    pthread_t cancel_thread, commit_thread;
    assert(pthread_create(&cancel_thread, NULL, race_worker, &cancel) == 0);
    assert(pthread_create(&commit_thread, NULL, race_worker, &commit) == 0);
    assert(pthread_join(cancel_thread, NULL) == 0 &&
           pthread_join(commit_thread, NULL) == 0);
    assert(pthread_barrier_destroy(&barrier) == 0);
    assert(cancel.accepted != commit.accepted);
    assert(bk7259_provision_abort_reason() == (cancel.accepted ? -3 : 0));
  }
}
static void test_old_credentials_until_ipv4_and_commit(void) {
  fixture();
  assert(bk7259_provision_begin() == 1);
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -1);
  assert(!test_env.saves);
  assert_saved("old");
  test_env.on_sta_start = NULL;
  assert(bk7259_wifi_manager_connect_candidate("new", "secret-password", 0) ==
         -1);
  assert(!test_env.saves);
  assert_saved("old");
  test_env.on_sta_start = connect_driver;
  connect_candidate();
  test_env.on_link_status = cancel_request;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -3);
  assert(!test_env.saves && !finishing_screens);
  assert_saved("old");
}
static void try_late_cancel(void) {
  late_cancel_accepted = bk7259_provision_cancel();
}
static void test_commit_wins_late_cancel_and_failed_save_reopens(void) {
  fixture();
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.on_save = try_late_cancel;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == 0);
  assert(!late_cancel_accepted && finishing_screens == 1 &&
         !bk7259_provision_cancel());
  assert(test_env.saves == 1);
  assert_saved("new");

  fixture();
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.save_result = -1;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -1);
  assert(finishing_screens == 1 && active_screens == 1);
  assert_saved("old");
  assert(bk7259_provision_cancel());
}
static void test_partial_write_and_readback_recovery(void) {
  fixture();
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.write_faults[0] = 2; /* Error reported after complete new record. */
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == 0);
  assert(!bk7259_provision_cancel() && !bk7259_wifi_manager_storage_fault());
  assert_saved("new");
  for (int fault = 3; fault <= 6; ++fault) {
    fixture();
    assert(bk7259_provision_begin() == 1);
    connect_candidate();
    test_env.write_faults[0] = fault;
    assert(bk7259_wifi_manager_save_credentials("new", "secret-password") ==
           -1);
    assert(!bk7259_wifi_manager_storage_fault() && bk7259_provision_cancel());
    assert_saved("old");
  }
  fixture();
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.write_faults[0] = 3;
  test_env.write_faults[1] = 1;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -1);
  assert(bk7259_wifi_manager_storage_fault() && !bk7259_provision_cancel());
  assert(bk7259_provision_abort_reason() ==
         0); /* Still COMMITTING, never false cancel. */

  fixture();
  test_env.saved_length = 0;
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.write_faults[0] = 3;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -1);
  assert(test_env.deletes == 1 && !test_env.saved_length &&
         bk7259_provision_cancel());
  fixture();
  test_env.saved_length = 0;
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.write_faults[0] = 3;
  test_env.delete_fails = true;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -1);
  assert(test_env.deletes == 1 && bk7259_wifi_manager_storage_fault());
  assert(!bk7259_provision_cancel());
  fixture();
  test_env.saved_credentials[0] ^= 1;
  assert(bk7259_provision_begin() == 1);
  connect_candidate();
  test_env.write_faults[0] = 3;
  assert(bk7259_wifi_manager_save_credentials("new", "secret-password") == -1);
  assert(!test_env.deletes && bk7259_wifi_manager_storage_fault());
  assert(!bk7259_provision_cancel());
}
static void test_all_waits_observe_cancel_and_new_request(void) {
  fixture();
  test_env.on_sta_start = NULL;
  test_env.on_wait = explicit_request;
  assert(bk7259_wifi_restore_saved("device") == -4);
  assert(test_env.sta_starts == 1 && test_env.sta_stops == 1 &&
         !test_env.saves);
  assert(bk7259_provision_take_request());
  test_env.on_wait = cancel_request;
  assert(bk7259_wifi_manager_connect_candidate("new", "secret-password",
                                               1000) == -3);
  assert(!s_wifi.sta_started);
  assert_saved("old");

  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_scan_start = cancel_request;
  wifi_scan_ap_info_t result[1];
  size_t count = 12;
  assert(bk7259_wifi_manager_scan_sync(result, 1, &count, 1000) == -3);
  assert(!count && !s_wifi.scan_in_progress && test_env.scan_stops == 1);

  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_select = cancel_request;
  assert(send_all(6, "secret-password", 15) == -3 && !test_env.socket_writes);

  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_select = cancel_request;
  portal_http_request_t request = {0};
  assert(receive_http_request(6, &request) == -3);
  assert(!test_env.socket_reads && !test_env.allocations);
}
static void test_saved_only_restore_has_no_portal_fallback(void) {
  fixture();
  assert(bk7259_wifi_restore_saved("device") == 0);
  assert(!test_env.ap_starts && !bk7259_provision_busy());
  assert_saved("old");
  fixture();
  test_env.saved_length = 0;
  assert(bk7259_wifi_restore_saved("device") == -1 && !test_env.ap_starts);
  fixture();
  test_env.saved_credentials[0] ^= 1;
  assert(bk7259_wifi_restore_saved("device") == -1 && !test_env.ap_starts);
  fixture();
  test_env.on_sta_start = NULL;
  assert(bk7259_wifi_restore_saved("device") == -1 && !test_env.ap_starts);
}
static void cancel_on_second_select(void) {
  if (test_env.selects == 2)
    cancel_request();
}
static void test_auto_and_explicit_portal_cancel_preserve_old_record(void) {
  fixture();
  test_env.saved_length = 0;
  test_env.on_select = cancel_request;
  assert(mybot_bk7259_ensure_network("device") == -3);
  assert(active_screens == 1 && test_env.ap_starts == 1 &&
         test_env.ap_stops == 1);
  assert(!test_env.saves && !s_softap_started && !s_wifi.sta_started);
  assert(bk7259_provision_busy());

  fixture();
  assert(bk7259_provision_request() && bk7259_provision_take_request());
  test_env.http_request = "POST /submit HTTP/1.1\r\nContent-Length: 20\r\n";
  test_env.on_select = cancel_on_second_select;
  assert(mybot_bk7259_provision_wifi("device") == -3);
  assert(test_env.socket_closes == 2 && test_env.allocations == 0);
  assert(!test_env.saves && !s_softap_started && !s_wifi.sta_started);
  assert_saved("old");
  bk7259_provision_finish(); /* App clears the accepted cancel after teardown.
                              */
  assert(bk7259_wifi_restore_saved("device") == 0);
  assert_saved("old");
}
static void test_portal_success_commits_once(void) {
  fixture();
  assert(bk7259_provision_request() && bk7259_provision_take_request());
  portal_http_request_t request = submission();
  char http[768];
  snprintf(http, sizeof(http),
           "POST /submit HTTP/1.1\r\nContent-Type: application/json\r\n"
           "Content-Length: %zu\r\n\r\n%s",
           request.body_length, request.body);
  test_env.http_request = http;
  test_env.on_save = try_late_cancel;
  assert(mybot_bk7259_provision_wifi("device") == 0);
  assert(test_env.saves == 1 && finishing_screens == 1 &&
         !late_cancel_accepted);
  assert(s_wifi.sta_started && !s_softap_started && !bk7259_provision_cancel());
  assert(test_env.listener_timeout_ms == 200 && test_env.socket_closes == 2);
  assert_saved("new");
}
static void test_rollback_fault_ends_portal(void) {
  fixture();
  assert(bk7259_provision_request() && bk7259_provision_take_request());
  portal_http_request_t request = submission();
  char http[768];
  snprintf(http, sizeof(http),
           "POST /submit HTTP/1.1\r\nContent-Type: application/json\r\n"
           "Content-Length: %zu\r\n\r\n%s",
           request.body_length, request.body);
  test_env.http_request = http;
  test_env.write_faults[0] = 3;
  test_env.write_faults[1] = 1;
  assert(mybot_bk7259_provision_wifi("device") == -1);
  assert(bk7259_wifi_manager_storage_fault() && !bk7259_provision_cancel());
  assert(!s_softap_started && !s_wifi.sta_started &&
         test_env.socket_closes == 2);
}
static void test_stop_failures_retain_ownership_and_beat_cancel(void) {
  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_sta_start = cancel_request;
  test_env.sta_stop_result = -1;
  assert(bk7259_wifi_manager_connect_candidate("new", "secret-password",
                                               1000) == -1);
  assert(s_wifi.sta_started && bk7259_provision_abort_reason() == -3);
  test_env.sta_stop_result = 0;
  assert(bk7259_wifi_manager_disconnect() == 0 && !s_wifi.sta_started);

  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_scan_start = cancel_request;
  test_env.scan_stop_result = -1;
  wifi_scan_ap_info_t result[1];
  size_t count;
  assert(bk7259_wifi_manager_scan_sync(result, 1, &count, 1000) == -1);
  assert(s_wifi.scan_in_progress);
  test_env.scan_stop_result = 0;
  assert(bk7259_wifi_manager_disconnect() == 0 && !s_wifi.scan_in_progress);

  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_select = cancel_request;
  test_env.ap_stop_result = -1;
  assert(bk7259_wifi_portal_run("device") == -1);
  assert(s_softap_started && bk7259_provision_abort_reason() == -3);
  test_env.ap_stop_result = 0;
  assert(bk7259_wifi_portal_stop() == 0 && !s_softap_started);
}
static void test_partial_start_failures_and_factory_precedence(void) {
  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.sta_start_result = test_env.sta_stop_result = -1;
  assert(bk7259_wifi_manager_connect_candidate("new", "secret-password",
                                               1000) == -1);
  assert(s_wifi.sta_started);
  test_env.sta_stop_result = 0;
  assert(bk7259_wifi_manager_disconnect() == 0 && !s_wifi.sta_started);
  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.ap_start_result = test_env.ap_stop_result = -1;
  assert(bk7259_wifi_portal_run("device") == -1 && s_softap_started);
  test_env.ap_stop_result = 0;
  assert(bk7259_wifi_portal_stop() == 0);
  fixture();
  assert(bk7259_provision_begin() == 1);
  test_env.on_select = test_request_reset;
  assert(bk7259_wifi_portal_run("device") == -2);
  assert(!s_softap_started && !s_wifi.sta_started && !test_env.saves);
  assert_saved("old");
}
int main(void) {
  test_request_and_priority_contract();
  test_cancel_commit_cas_race();
  test_old_credentials_until_ipv4_and_commit();
  test_commit_wins_late_cancel_and_failed_save_reopens();
  test_partial_write_and_readback_recovery();
  test_all_waits_observe_cancel_and_new_request();
  test_saved_only_restore_has_no_portal_fallback();
  test_auto_and_explicit_portal_cancel_preserve_old_record();
  test_portal_success_commits_once();
  test_rollback_fault_ends_portal();
  test_stop_failures_retain_ownership_and_beat_cancel();
  test_partial_start_failures_and_factory_precedence();
  disconnect_and_finish();
  puts("Provision network: 12 groups, CAS race, credentials, cancellation and "
       "cleanup passed");
  return 0;
}
