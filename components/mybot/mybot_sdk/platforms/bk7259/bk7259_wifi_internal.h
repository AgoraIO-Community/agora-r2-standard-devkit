/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_WIFI_INTERNAL_H_
#define BK7259_WIFI_INTERNAL_H_

#include <modules/wifi_types.h>

#include <stddef.h>
#include <stdint.h>

int bk7259_wifi_portal_run(const char *device_id);
/* Retry a SoftAP teardown that did not complete in portal_run(). */
int bk7259_wifi_portal_stop(void);

/* Candidate connection, scan and credential persistence return
 * BK7259_NETWORK_CANCELLED when factory reset takes precedence. */
int bk7259_wifi_manager_connect_candidate(const char *ssid, const char *password,
                                          uint32_t timeout_ms);
int bk7259_wifi_manager_disconnect(void);
int bk7259_wifi_manager_save_credentials(const char *ssid, const char *password);
int bk7259_wifi_manager_scan_sync(wifi_scan_ap_info_t *results, size_t capacity,
                                  size_t *result_count, uint32_t timeout_ms);

#endif /* BK7259_WIFI_INTERNAL_H_ */
