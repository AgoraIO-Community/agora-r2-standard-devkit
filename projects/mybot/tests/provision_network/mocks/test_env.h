/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_TEST_ENV_H_
#define MYBOT_WIFI_TEST_ENV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_wifi.h>

#define MYBOT_BK7259_OPS_H_
#define BK7259_PLATFORM_LOG_H_
#define MYBOT_LOGI(...) test_log(__VA_ARGS__)
#define MYBOT_LOGW(...) test_log(__VA_ARGS__)
#define MYBOT_LOGE(...) test_log(__VA_ARGS__)
#define MYBOT_LOGD(...) test_log(__VA_ARGS__)
#define BK_OK 0
#define BK_FAIL (-1)
#define BK_ERR_EVENT_NO_CB (-3)
#define BEKEN_NO_WAIT 0
#define WIFI_SSID_STR_LEN 33
#define WIFI_PASSWORD_LEN 65
#define WIFI_MAC_LEN 6
#define EF_NO_ERR 0
#define NETIF_IF_STA 0
#define NETIF_IF_AP 1
#define EVENT_MOD_NETIF 1
#define EVENT_MOD_WIFI 2
#define EVENT_NETIF_GOT_IP4 3
#define EVENT_WIFI_STA_DISCONNECTED 4
#define EVENT_WIFI_SCAN_DONE 5
#define WIFI_LINKSTATE_STA_GOT_IP 1

typedef int bk_err_t;
typedef int event_module_t;
typedef int EfErrCode;
typedef void *beken_mutex_t;
typedef void *beken_semaphore_t;
typedef bk_err_t (*event_cb_t)(void *, event_module_t, int, void *);
typedef enum {
  WIFI_SECURITY_NONE,
  WIFI_SECURITY_WEP,
  WIFI_SECURITY_WPA_TKIP,
  WIFI_SECURITY_WPA_AES,
  WIFI_SECURITY_WPA_MIXED,
  WIFI_SECURITY_WPA2_TKIP,
  WIFI_SECURITY_WPA2_AES,
  WIFI_SECURITY_WPA2_MIXED,
  WIFI_SECURITY_EAP,
  WIFI_SECURITY_WPA3_SAE,
  WIFI_SECURITY_WPA3_WPA2_MIXED,
  WIFI_SECURITY_OWE,
  WIFI_SECURITY_AUTO,
} wifi_security_t;
typedef struct {
  char ssid[WIFI_SSID_STR_LEN];
  char password[WIFI_PASSWORD_LEN];
  wifi_security_t security;
  int auto_reconnect_count;
  int auto_reconnect_timeout;
  bool disable_auto_reconnect_after_disconnect;
} wifi_sta_config_t;
typedef struct {
  char ssid[WIFI_SSID_STR_LEN];
  int state;
} wifi_link_status_t;
typedef struct {
  char ssid[WIFI_SSID_STR_LEN];
  int rssi;
  wifi_security_t security;
} wifi_scan_ap_info_t;
typedef struct {
  int ap_num;
  wifi_scan_ap_info_t *aps;
} wifi_scan_result_t;
typedef struct {
  char ssid[WIFI_SSID_STR_LEN];
  int channel, security, max_con, disable_dns_server;
} wifi_ap_config_t;
typedef struct {
  int netif_if;
} netif_event_got_ip4_t;
typedef struct {
  char ip[16], mask[16], gateway[16], dns[16];
} netif_ip4_config_t;

typedef struct {
  bool reset, connected;
  int sta_start_result, sta_stop_result, scan_stop_result;
  int ap_start_result, ap_stop_result, save_result;
  int write_faults[4];
  bool delete_fails;
  int deletes;
  int sta_starts, sta_stops, scan_starts, scan_stops;
  int ap_starts, ap_stops, saves, socket_closes, socket_reads, socket_writes;
  int selects, allocations, listener_timeout_ms;
  uint32_t time_ms;
  char connected_ssid[WIFI_SSID_STR_LEN];
  wifi_sta_config_t sta_config;
  unsigned char saved_credentials[112];
  size_t saved_length;
  const char *http_request;
  bool request_sent;
  void (*on_delay)(void);
  void (*on_sta_start)(void);
  void (*on_scan_start)(void);
  void (*on_link_status)(void);
  void (*on_save)(void);
  void (*on_ap_start)(void);
  void (*on_select)(void);
  void (*on_send)(void);
  void (*on_wait)(void);
  void (*on_recv)(void);
} wifi_test_env_t;
extern wifi_test_env_t test_env;
void test_env_reset(void);
void test_request_reset(void);
void test_log(const char *, const char *, ...);

int rtos_init_mutex(beken_mutex_t *);
int rtos_deinit_mutex(beken_mutex_t *);
int rtos_lock_mutex(beken_mutex_t *);
int rtos_unlock_mutex(beken_mutex_t *);
int rtos_init_semaphore(beken_semaphore_t *, int);
int rtos_deinit_semaphore(beken_semaphore_t *);
int rtos_get_semaphore(beken_semaphore_t *, uint32_t);
int rtos_set_semaphore(beken_semaphore_t *);
uint32_t rtos_get_time(void);
int rtos_delay_milliseconds(uint32_t);
int easyflash_init(void);
size_t ef_get_env_blob(const char *, void *, size_t, size_t *);
int ef_set_env_blob(const char *, const void *, size_t);
int ef_del_env(const char *);
int bk_event_register_cb(event_module_t, int, event_cb_t, void *);
int bk_event_unregister_cb(event_module_t, int, event_cb_t);
int bk_wifi_sta_get_link_status(wifi_link_status_t *);
int bk_wifi_sta_set_config(const wifi_sta_config_t *);
int bk_wifi_sta_start(void);
int bk_wifi_sta_stop(void);
int bk_wifi_scan_start(const void *);
int bk_wifi_scan_stop(void);
int bk_wifi_scan_get_result(wifi_scan_result_t *);
void bk_wifi_scan_free_result(wifi_scan_result_t *);
int bk_wifi_ap_get_mac(uint8_t *);
int bk_wifi_ap_set_config(const wifi_ap_config_t *);
int bk_wifi_ap_start(void);
int bk_wifi_ap_stop(void);
int bk_netif_set_ip4_config(int, const netif_ip4_config_t *);
int bk7259_lcd_show_screen(mybot_lcd_screen_t);
void bk7259_lcd_set_provisioning_ssid(const char *);
int bk7259_prompt_play_provisioning(void);
int bk7259_prompt_play_success(void);
void *test_os_malloc(size_t);
void *test_os_zalloc(size_t);
void test_os_free(void *);
int test_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int test_socket(int, int, int);
int test_setsockopt(int, int, int, const void *, socklen_t);
int test_bind(int, const struct sockaddr *, socklen_t);
int test_listen(int, int);
int test_accept(int, struct sockaddr *, socklen_t *);
ssize_t test_send(int, const void *, size_t, int);
ssize_t test_recv(int, void *, size_t, int);
int test_shutdown(int, int);
int test_close(int);

#endif
