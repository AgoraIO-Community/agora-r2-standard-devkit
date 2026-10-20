/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse the physical/storage fault fixture, and run its original reset cases
 * before exercising the real application together with the real setup mailbox. */
#define TOUCH_RESET_TEST_MAIN legacy_reset_cases
#include "../touch_reset/test_touch_reset.c"

static unsigned provision_calls, restore_calls;
static int portal_result, restore_result;
static bool cancel_in_stop, fail_cleanup, reset_in_portal, reset_in_restore;
static bool new_request_after_finish;

static void assert_account_records_unchanged(void) {
    const unsigned char original[] = {1, 2, 3, 4};
    assert(records[AUTH].present && records[VOLUME].present && records[UNRELATED].present);
    assert(!memcmp(records[AUTH].bytes, original, sizeof(original)));
    assert(!memcmp(records[VOLUME].bytes, original, sizeof(original)));
    assert(!writes && !deletes && !reboot_calls && !records[INTENT].present);
}
static int portal(const char *device_id) {
    assert(device_id[0] && !sdk_running && sdk_state == MYBOT_STATE_STOPPED);
    assert(bk7259_provision_begin() == 0); /* explicit request already claimed */
    provision_calls++;
    record('Q');
    assert(bk7259_lcd_show_provision(BK7259_PROVISION_ACTIVE) == 0);
    /* Short presses during setup must never toggle the next SDK instance. */
    aosl_atomic_set(&s_conversation_requested, 1);
    if (portal_result == 0) {
        assert(bk7259_provision_try_commit());
        const unsigned char new_wifi[] = {9, 8, 7};
        store(WIFI, new_wifi, sizeof(new_wifi));
        assert(!bk7259_provision_cancel()); /* commit excludes a misleading late cancel */
        assert(bk7259_wifi_portal_stop() == 0);
        network_stopped = false;
    } else if (portal_result == BK7259_PROVISION_CANCELLED) {
        assert(bk7259_provision_cancel());
    }
    if (fail_cleanup) portal_failures = 1;
    if (reset_in_portal) bk7259_factory_reset_request();
    return reset_in_portal ? BK7259_NETWORK_CANCELLED : portal_result;
}
static int restore(const char *device_id) {
    assert(device_id[0] && !sdk_running && sdk_state == MYBOT_STATE_STOPPED);
    assert(!bk7259_provision_busy() && network_stopped);
    assert(provision_screen == BK7259_PROVISION_RECONNECTING);
    restore_calls++;
    record('O');
    if (reset_in_restore) {
        bk7259_factory_reset_request();
        return BK7259_NETWORK_CANCELLED;
    }
    if (restore_result == 0) network_stopped = false;
    return restore_result;
}
static void stop_hook(void) {
    if (cancel_in_stop && bk7259_provision_busy()) {
        assert(bk7259_provision_cancel());
        cancel_in_stop = false;
    }
}
static void owner_fixture(void) {
    fixture();
    provision_calls = restore_calls = 0;
    portal_result = restore_result = 0;
    cancel_in_stop = fail_cleanup = reset_in_portal = reset_in_restore = false;
    new_request_after_finish = false;
    owner_provision_hook = portal;
    owner_restore_hook = restore;
    owner_stop_hook = stop_hook;
}
static void request_on_first_poll(void) {
    if (idle_polls == 1) {
        assert(bk7259_provision_request());
    } else if (idle_polls == 2) {
        longjmp(exit_point, EXIT_IDLE);
    }
}
static void test_success_and_cancel_restart(void) {
    const int results[] = {0, BK7259_PROVISION_CANCELLED, -1};
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        owner_fixture();
        portal_result = results[i];
        owner_poll_hook = request_on_first_poll;
        assert(run_owner(true) == EXIT_IDLE);
        assert(network_calls == 1 && provision_calls == 1 && stop_calls == 1 && start_calls == 2);
        assert(restore_calls == (results[i] != 0));
        assert(provision_screen == BK7259_PROVISION_NONE && sdk_running &&
               sdk_state == MYBOT_STATE_READY && !bk7259_provision_busy());
        assert(!bk7259_key_take_conversation_request() && !conversation_events);
        if (results[i] != 0) {
            assert(strstr(events, "SQPDOA")); /* stop -> portal -> cleanup -> old Wi-Fi -> SDK */
            assert(records[WIFI].bytes[0] == 1);
        } else {
            assert(strstr(events, "SQPA"));
            assert(records[WIFI].bytes[0] == 9);
        }
        assert_account_records_unchanged();
    }
}
static void quick_cancel_on_first_poll(void) {
    if (idle_polls == 1) {
        sdk_state = MYBOT_STATE_IN_CONVERSATION;
        aosl_atomic_set(&s_conversation_requested, 1);
        assert(bk7259_provision_request() && bk7259_provision_cancel());
    } else if (idle_polls == 2) {
        longjmp(exit_point, EXIT_IDLE);
    }
}
static void test_cancel_before_and_during_stop(void) {
    owner_fixture();
    owner_poll_hook = quick_cancel_on_first_poll;
    assert(run_owner(true) == EXIT_IDLE);
    assert(start_calls == 1 && !stop_calls && !provision_calls && !restore_calls);
    assert(sdk_running && sdk_state == MYBOT_STATE_IN_CONVERSATION &&
           provision_screen == BK7259_PROVISION_NONE && !conversation_events);
    assert_account_records_unchanged();

    owner_fixture();
    cancel_in_stop = true;
    owner_poll_hook = request_on_first_poll;
    assert(run_owner(true) == EXIT_IDLE);
    assert(start_calls == 2 && stop_calls == 1 && !provision_calls && restore_calls == 1);
    assert(!bk7259_provision_busy());
    assert_account_records_unchanged();
}
static void retry_after_failed_stop(void) {
    if (idle_polls == 1) {
        assert(bk7259_provision_request());
    } else if (idle_polls == 2) {
        assert(provision_screen == BK7259_PROVISION_FAILED && !provision_calls && !restore_calls);
        assert(start_calls == 1 && !bk7259_provision_busy());
        assert(bk7259_provision_request());
    } else if (idle_polls == 3) {
        longjmp(exit_point, EXIT_IDLE);
    }
}
static void test_both_stop_guards_and_retry(void) {
    for (unsigned fault = 0; fault < 3; ++fault) {
        owner_fixture();
        stop_failures = 1;
        stop_leaves_running = fault == 1;
        cancel_in_stop = fault == 2;
        owner_poll_hook = retry_after_failed_stop;
        assert(run_owner(true) == EXIT_IDLE);
        assert(provision_calls == 1 && start_calls == 2 && stop_calls == 2 && !restore_calls);
        assert_account_records_unchanged();
    }
}
static void assert_failed_and_wait(void) {
    if (idle_polls == 1) {
        assert(bk7259_provision_request());
    } else {
        assert(provision_screen == BK7259_PROVISION_FAILED && !sdk_running);
        assert(start_calls == 1 && provision_calls == 1);
    }
}
static void test_no_saved_network_does_not_reopen_portal(void) {
    owner_fixture();
    records[WIFI].present = false;
    portal_result = BK7259_PROVISION_CANCELLED;
    restore_result = -1;
    owner_poll_hook = assert_failed_and_wait;
    assert(run_owner(true) == EXIT_IDLE && idle_polls == 4);
    assert(provision_calls == 1 && restore_calls == 1 && start_calls == 1);
    assert_account_records_unchanged();
}
static void retry_after_cleanup_failure(void) {
    if (idle_polls == 1) {
        assert(bk7259_provision_request());
    } else if (idle_polls == 2) {
        assert(!sdk_running && provision_screen == BK7259_PROVISION_FAILED);
        assert(start_calls == 1 && !restore_calls && provision_calls == 1);
        fail_cleanup = false;
        portal_result = 0;
        assert(bk7259_provision_request());
    } else if (idle_polls == 3) {
        longjmp(exit_point, EXIT_IDLE);
    }
}
static void test_cleanup_failure_blocks_restart(void) {
    owner_fixture();
    fail_cleanup = true;
    portal_result = BK7259_PROVISION_CANCELLED;
    owner_poll_hook = retry_after_cleanup_failure;
    assert(run_owner(true) == EXIT_IDLE);
    assert(provision_calls == 2 && !restore_calls && start_calls == 2);
    assert_account_records_unchanged();
}
static void recover_from_runtime_failure(void) {
    if (idle_polls == 1) {
        sdk_state = MYBOT_STATE_FAILED;
    } else if (idle_polls == 2) {
        assert(provision_screen == BK7259_PROVISION_FAILED && !sdk_running);
        assert(bk7259_provision_request());
    } else if (idle_polls == 3) {
        longjmp(exit_point, EXIT_IDLE);
    }
}
static void test_runtime_failed_keeps_setup_entry(void) {
    owner_fixture();
    owner_poll_hook = recover_from_runtime_failure;
    assert(run_owner(true) == EXIT_IDLE);
    assert(start_calls == 2 && provision_calls == 1 && stop_calls == 2);
    assert_account_records_unchanged();
}
static void retry_start_failure(void) {
    if (idle_polls == 1) {
        assert(!sdk_running && provision_screen == BK7259_PROVISION_FAILED);
        fail_start = false;
        assert(bk7259_provision_request());
    } else if (idle_polls == 2) {
        longjmp(exit_point, EXIT_IDLE);
    }
}
static void test_start_failure_keeps_setup_entry(void) {
    owner_fixture();
    fail_start = true;
    owner_poll_hook = retry_start_failure;
    assert(run_owner(true) == EXIT_IDLE);
    assert(start_calls == 2 && provision_calls == 1 && stop_calls == 2 && sdk_running);
    assert_account_records_unchanged();
}
static int request_during_initial_wifi(const char *device_id) {
    assert(device_id[0] && bk7259_provision_request());
    return BK7259_PROVISION_REQUESTED;
}
static void exit_first_poll(void) { longjmp(exit_point, EXIT_IDLE); }
static void test_initial_saved_wait_yields_to_explicit_request(void) {
    owner_fixture();
    owner_ensure_hook = request_during_initial_wifi;
    owner_poll_hook = exit_first_poll;
    assert(run_owner(true) == EXIT_IDLE);
    assert(network_calls == 1 && provision_calls == 1 && start_calls == 1 && stop_calls == 1);
    assert(!restore_calls && !bk7259_provision_busy());
    assert_account_records_unchanged();
}
static int cancel_pending_during_initial_wifi(const char *device_id) {
    assert(device_id[0] && bk7259_provision_request() && bk7259_provision_cancel());
    return BK7259_PROVISION_CANCELLED;
}
static void test_initial_wait_cancelled_pending_request(void) {
    owner_fixture();
    owner_ensure_hook = cancel_pending_during_initial_wifi;
    owner_poll_hook = exit_first_poll;
    assert(run_owner(true) == EXIT_IDLE);
    assert(network_calls == 1 && !provision_calls && restore_calls == 1 && start_calls == 1);
    assert(!stop_calls && !bk7259_provision_busy());
    assert_account_records_unchanged();
}
static void request_between_finish_and_none(void) {
    if (new_request_after_finish && provision_calls == 1 && !bk7259_provision_busy()) {
        new_request_after_finish = false;
        assert(bk7259_provision_request());
        assert(bk7259_lcd_show_provision(BK7259_PROVISION_PREPARING) == 0);
    }
}
static void test_new_request_survives_old_session_finish(void) {
    owner_fixture();
    new_request_after_finish = true;
    owner_workflow_hook = request_between_finish_and_none;
    owner_poll_hook = request_on_first_poll;
    assert(run_owner(true) == EXIT_IDLE);
    assert(!new_request_after_finish && provision_calls == 2 && stop_calls == 2 && start_calls == 2);
    assert(!bk7259_provision_busy());
    assert_account_records_unchanged();
}
static int cancel_automatic_portal(const char *device_id) {
    assert(device_id[0] && bk7259_provision_begin() == 1);
    assert(bk7259_lcd_show_provision(BK7259_PROVISION_ACTIVE) == 0);
    assert(bk7259_provision_cancel());
    return BK7259_PROVISION_CANCELLED;
}
static void test_automatic_portal_cancel_only_restores_saved(void) {
    for (unsigned missing = 0; missing < 2; ++missing) {
        owner_fixture();
        if (missing) records[WIFI].present = false;
        restore_result = missing ? -1 : 0;
        owner_ensure_hook = cancel_automatic_portal;
        owner_poll_hook = exit_first_poll;
        assert(run_owner(true) == EXIT_IDLE);
        assert(network_calls == 1 && !provision_calls && restore_calls == 1);
        assert(start_calls == (missing ? 0U : 1U));
        assert(!bk7259_provision_busy());
        assert_account_records_unchanged();
    }
}
static void test_reset_preempts_setup_and_reconnect(void) {
    owner_fixture();
    assert(bk7259_provision_request());
    bk7259_factory_reset_request();
    assert(run_owner(true) == EXIT_REBOOT);
    assert(!network_calls && !provision_calls && !start_calls && !bk7259_provision_busy());
    assert_erased();
    for (unsigned where = 0; where < 2; ++where) {
        owner_fixture();
        reset_in_portal = where == 0;
        reset_in_restore = where == 1;
        portal_result = BK7259_PROVISION_CANCELLED;
        owner_poll_hook = request_on_first_poll;
        assert(run_owner(true) == EXIT_REBOOT);
        assert(start_calls == 1 && provision_calls == 1 && !bk7259_provision_busy());
        assert(restore_calls == where);
        assert_erased();
    }
}

int main(void) {
    assert(legacy_reset_cases() == 0);
    test_success_and_cancel_restart();
    test_cancel_before_and_during_stop();
    test_both_stop_guards_and_retry();
    test_no_saved_network_does_not_reopen_portal();
    test_cleanup_failure_blocks_restart();
    test_runtime_failed_keeps_setup_entry();
    test_start_failure_keeps_setup_entry();
    test_initial_saved_wait_yields_to_explicit_request();
    test_initial_wait_cancelled_pending_request();
    test_new_request_survives_old_session_finish();
    test_automatic_portal_cancel_only_restores_saved();
    test_reset_preempts_setup_and_reconnect();
    fixture();
    puts("Provision owner: real app/mailbox stop guards, cancellation, old Wi-Fi restore, recovery and reset priority passed");
    return 0;
}
