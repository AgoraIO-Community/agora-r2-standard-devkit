/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_WIFI_INTERNAL_H_
#define BK7259_WIFI_INTERNAL_H_

#include <modules/wifi_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int bk7259_wifi_portal_run(const char *device_id);
/* Retry a SoftAP teardown that did not complete in portal_run(). */
int bk7259_wifi_portal_stop(void);

/* Candidate connection, scan and credential persistence return the provision
 * abort reason (-2 reset, -3 cancel, -4 new request). Driver teardown failures
 * return -1 and retain ownership even if cancellation was accepted. */
int bk7259_wifi_manager_connect_candidate(const char *ssid, const char *password,
                                          uint32_t timeout_ms);
int bk7259_wifi_manager_disconnect(void);
int bk7259_wifi_manager_save_credentials(const char *ssid, const char *password);
/* A failed credential rollback must end the current portal without accepting
 * cancellation as though the previous saved network were intact. Owner only. */
bool bk7259_wifi_manager_storage_fault(void);
int bk7259_wifi_manager_scan_sync(wifi_scan_ap_info_t *results, size_t capacity,
                                  size_t *result_count, uint32_t timeout_ms);

#endif /* BK7259_WIFI_INTERNAL_H_ */
