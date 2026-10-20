/* SPDX-License-Identifier: Apache-2.0 */
#include "test_env.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

wifi_test_env_t test_env;

void test_env_reset(void) {
  assert(test_env.allocations == 0);
  memset(&test_env, 0, sizeof(test_env));
}

void test_request_reset(void) { test_env.reset = true; }
bool bk7259_factory_reset_requested(void) { return test_env.reset; }
void test_log(const char *tag, const char *format, ...) {
  (void)tag;
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  /* Neither accepted nor failed provisioning may log the password. */
  assert(strstr(buffer, "secret-password") == NULL);
}

int rtos_init_mutex(beken_mutex_t *value) {
  *value = malloc(1);
  return *value ? 0 : -1;
}
int rtos_deinit_mutex(beken_mutex_t *value) {
  free(*value);
  *value = NULL;
  return 0;
}
int rtos_lock_mutex(beken_mutex_t *value) { return *value ? 0 : -1; }
int rtos_unlock_mutex(beken_mutex_t *value) { return *value ? 0 : -1; }
int rtos_init_semaphore(beken_semaphore_t *value, int count) {
  (void)count;
  return rtos_init_mutex(value);
}
int rtos_deinit_semaphore(beken_semaphore_t *value) {
  return rtos_deinit_mutex(value);
}
int rtos_get_semaphore(beken_semaphore_t *value, uint32_t timeout) {
  (void)value;
  test_env.time_ms += timeout;
  if (timeout && test_env.on_wait)
    test_env.on_wait();
  return -1;
}
int rtos_set_semaphore(beken_semaphore_t *value) {
  (void)value;
  return 0;
}
uint32_t rtos_get_time(void) { return test_env.time_ms; }
int rtos_delay_milliseconds(uint32_t ms) {
  test_env.time_ms += ms;
  if (test_env.on_delay)
    test_env.on_delay();
  return 0;
}
int easyflash_init(void) { return 0; }
size_t ef_get_env_blob(const char *key, void *out, size_t cap, size_t *saved) {
  assert(strcmp(key, "mybot.wifi.v1") == 0);
  if (saved)
    *saved = test_env.saved_length;
  size_t copied = cap < test_env.saved_length ? cap : test_env.saved_length;
  if (out)
    memcpy(out, test_env.saved_credentials, copied);
  return copied;
}
int ef_set_env_blob(const char *key, const void *data, size_t size) {
  /* Device authentication and volume storage must never be touched. */
  assert(strcmp(key, "mybot.wifi.v1") == 0);
  assert(size == sizeof(test_env.saved_credentials));
  ++test_env.saves;
  if (test_env.on_save)
    test_env.on_save();
  int fault =
      test_env.saves <= 4 ? test_env.write_faults[test_env.saves - 1] : 0;
  if (fault == 1 || (fault == 0 && test_env.save_result != 0))
    return -1;
  if (fault == 5) {
    test_env.saved_length =
        0; /* Old PRE_DELETE hidden; replacement not valid. */
    return -1;
  }
  if (fault != 4) {
    memcpy(test_env.saved_credentials, data, size);
    test_env.saved_length = size;
    if (fault == 3 || fault == 6)
      test_env.saved_credentials[0] ^= 1;
  }
  return fault == 2 || fault == 3 ? -1 : 0;
}
int ef_del_env(const char *key) {
  assert(strcmp(key, "mybot.wifi.v1") == 0);
  ++test_env.deletes;
  if (test_env.delete_fails)
    return -1;
  test_env.saved_length = 0;
  return 0;
}
int bk_event_register_cb(event_module_t module, int event, event_cb_t cb,
                         void *arg) {
  (void)module;
  (void)event;
  (void)cb;
  (void)arg;
  return 0;
}
int bk_event_unregister_cb(event_module_t module, int event, event_cb_t cb) {
  (void)module;
  (void)event;
  (void)cb;
  return 0;
}
int bk_wifi_sta_get_link_status(wifi_link_status_t *status) {
  if (test_env.on_link_status)
    test_env.on_link_status();
  status->state = test_env.connected ? WIFI_LINKSTATE_STA_GOT_IP : 0;
  snprintf(status->ssid, sizeof(status->ssid), "%s", test_env.connected_ssid);
  return 0;
}
int bk_wifi_sta_set_config(const wifi_sta_config_t *config) {
  test_env.sta_config = *config;
  return 0;
}
int bk_wifi_sta_start(void) {
  ++test_env.sta_starts;
  if (test_env.on_sta_start)
    test_env.on_sta_start();
  return test_env.sta_start_result;
}
int bk_wifi_sta_stop(void) {
  ++test_env.sta_stops;
  if (test_env.sta_stop_result == 0)
    test_env.connected = false;
  return test_env.sta_stop_result;
}
int bk_wifi_scan_start(const void *config) {
  (void)config;
  ++test_env.scan_starts;
  if (test_env.on_scan_start)
    test_env.on_scan_start();
  return 0;
}
int bk_wifi_scan_stop(void) {
  ++test_env.scan_stops;
  return test_env.scan_stop_result;
}
int bk_wifi_scan_get_result(wifi_scan_result_t *scan) {
  *scan = (wifi_scan_result_t){0};
  return 0;
}
void bk_wifi_scan_free_result(wifi_scan_result_t *scan) { (void)scan; }
int bk_wifi_ap_get_mac(uint8_t *mac) {
  memset(mac, 1, 6);
  return 0;
}
int bk_wifi_ap_set_config(const wifi_ap_config_t *config) {
  (void)config;
  return 0;
}
int bk_wifi_ap_start(void) {
  ++test_env.ap_starts;
  if (test_env.on_ap_start)
    test_env.on_ap_start();
  return test_env.ap_start_result;
}
int bk_wifi_ap_stop(void) {
  ++test_env.ap_stops;
  return test_env.ap_stop_result;
}
int bk_netif_set_ip4_config(int iface, const netif_ip4_config_t *ip) {
  (void)iface;
  (void)ip;
  return 0;
}
int bk7259_lcd_show_screen(mybot_lcd_screen_t screen) {
  (void)screen;
  return 0;
}
void bk7259_lcd_set_provisioning_ssid(const char *ssid) { (void)ssid; }
int bk7259_prompt_play_provisioning(void) { return 0; }
int bk7259_prompt_play_success(void) { return 0; }
void *test_os_malloc(size_t size) {
  void *p = malloc(size);
  if (p)
    ++test_env.allocations;
  return p;
}
void test_os_free(void *p) {
  if (p)
    --test_env.allocations;
  free(p);
}
void *test_os_zalloc(size_t size) {
  void *p = test_os_malloc(size);
  if (p)
    memset(p, 0, size);
  return p;
}
int test_select(int nfds, fd_set *read, fd_set *write, fd_set *error,
                struct timeval *timeout) {
  (void)nfds;
  (void)read;
  (void)write;
  (void)error;
  ++test_env.selects;
  test_env.time_ms +=
      (uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
  if (test_env.on_select)
    test_env.on_select();
  return 1;
}
int test_socket(int domain, int type, int protocol) {
  (void)domain;
  (void)type;
  (void)protocol;
  return 5;
}
int test_setsockopt(int fd, int level, int opt, const void *value,
                    socklen_t length) {
  (void)level;
  if (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) {
    assert(length == sizeof(struct timeval));
    const struct timeval *timeout = value;
    int ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    assert(ms > 0 && ms <= 200);
    if (fd == 5 && opt == SO_RCVTIMEO)
      test_env.listener_timeout_ms = ms;
  }
  return 0;
}
int test_bind(int fd, const struct sockaddr *address, socklen_t length) {
  (void)fd;
  (void)address;
  (void)length;
  return 0;
}
int test_listen(int fd, int backlog) {
  (void)fd;
  (void)backlog;
  return 0;
}
int test_accept(int fd, struct sockaddr *address, socklen_t *length) {
  (void)fd;
  (void)address;
  (void)length;
  return 6;
}
ssize_t test_send(int fd, const void *data, size_t size, int flags) {
  (void)fd;
  (void)data;
  (void)flags;
  ++test_env.socket_writes;
  if (test_env.on_send)
    test_env.on_send();
  return (ssize_t)size;
}
ssize_t test_recv(int fd, void *out, size_t size, int flags) {
  (void)fd;
  (void)flags;
  ++test_env.socket_reads;
  if (test_env.on_recv)
    test_env.on_recv();
  if (!test_env.http_request || test_env.request_sent)
    return 0;
  size_t length = strlen(test_env.http_request);
  assert(length <= size);
  memcpy(out, test_env.http_request, length);
  test_env.request_sent = true;
  return (ssize_t)length;
}
int test_shutdown(int fd, int how) {
  (void)fd;
  (void)how;
  return 0;
}
int test_close(int fd) {
  (void)fd;
  ++test_env.socket_closes;
  return 0;
}
