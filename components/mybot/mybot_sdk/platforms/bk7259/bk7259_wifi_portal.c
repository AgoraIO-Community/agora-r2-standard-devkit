/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_wifi_internal.h"
#include "bk7259_ops.h"
#include "bk7259_factory_reset.h"

#include <common/bk_err.h>
#include "bk7259_platform_log.h"
#include <components/netif.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <modules/wifi.h>
#include <os/mem.h>
#include <os/os.h>

#include "cJSON.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "mybot_portal"
#define PORTAL_HTTP_PORT 80
#define PORTAL_AP_CHANNEL 1
#define PORTAL_HTTP_HEADER_MAX 1024U
#define PORTAL_HTTP_BODY_MAX 512U
#define PORTAL_HTTP_REQUEST_TIMEOUT_MS 7000U
#define PORTAL_HTTP_IO_TIMEOUT_MS 1000U
#define PORTAL_HTTP_POLL_MS 200U
#define PORTAL_CONNECT_TIMEOUT_MS 20000U
#define PORTAL_SCAN_TIMEOUT_MS 15000U
#define PORTAL_EXIT_DELAY_MS 200U
#define PORTAL_MAX_SCAN_RESULTS 24U
#define PORTAL_SCAN_JSON_MAX 6144U

typedef struct {
    char method[8];
    char target[64];
    char body[PORTAL_HTTP_BODY_MAX + 1U];
    size_t body_length;
    bool has_content_length;
    bool content_type_json;
} portal_http_request_t;

typedef struct {
    char ap_ssid[WIFI_SSID_STR_LEN];
} portal_context_t;

/* Only the product task starts/stops the portal. Keep ownership across a
 * failed stop so factory-reset retry can release the same SoftAP. */
static bool s_softap_started;

static const char s_portal_html[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' "
    "content='width=device-width,initial-scale=1'><title>Wi-Fi setup</title><style>"
    "*{box-sizing:border-box}body{font:15px system-ui;margin:0;background:#f3f5f7;color:#17202a}"
    "main{max-width:440px;margin:auto;padding:24px 16px}section{background:#fff;border:1px solid "
    "#dfe4e8;border-radius:8px;padding:18px}h1{font-size:22px;margin:0 0 16px}label{display:block;"
    "margin:10px 0 5px}select,input,button{width:100%;min-height:42px;padding:9px;border:1px solid "
    "#aeb8c2;border-radius:6px;background:#fff}button{margin-top:12px;background:#1769aa;color:#fff;"
    "border:0;font-weight:600}button:disabled{background:#8c9aa6}#status{min-height:20px;color:#5b6670}"
    ".error{color:#b3261e}</style></head><body><main><section><h1>Wi-Fi setup</h1><form id='f'>"
    "<label for='nets'>Network</label><select id='nets'><option value=''>Scanning...</option></select>"
    "<label for='ssid'>SSID</label><input id='ssid' maxlength='32' required><label for='password'>"
    "Password</label><input id='password' type='password' maxlength='64'><button id='submit'>"
    "Connect</button></form><p id='status'></p></section></main><script>const q=s=>document.querySelector(s);"
    "async function scan(){try{const d=await fetch('/scan',{cache:'no-store'}).then(r=>{if(!r.ok)"
    "throw Error();return r.json()}),n=q('#nets');n.innerHTML='<option value=\"\">Manual entry</option>';"
    "d.aps.forEach(a=>{const o=document.createElement('option');o.value=a.ssid;o.textContent="
    "a.ssid+' ('+a.rssi+' dBm)';n.appendChild(o)})}catch(e){q('#nets').innerHTML="
    "'<option value=\"\">Manual entry</option>'}}q('#nets').onchange=e=>{if(e.target.value)"
    "q('#ssid').value=e.target.value};q('#f').onsubmit=async e=>{e.preventDefault();const b=q('#submit'),"
    "s=q('#status');b.disabled=true;s.className='';s.textContent='Connecting...';try{const r=await "
    "fetch('/submit',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({"
    "ssid:q('#ssid').value,password:q('#password').value})}),d=await r.json();if(!r.ok||!d.success)"
    "throw Error(d.error||'Connection failed');s.textContent='Connected'}catch(x){s.className='error';"
    "s.textContent=x.message;b.disabled=false}};scan()</script></body></html>";

static void secure_zero(void *memory, size_t length) {
    volatile unsigned char *cursor = memory;
    while (length-- > 0) {
        *cursor++ = 0;
    }
}

static size_t bounded_strlen(const char *value, size_t capacity) {
    size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool contains_json_nul_escape(const char *value, size_t length) {
    for (size_t i = 0; i + 5U < length; ++i) {
        if (value[i] != '\\' || value[i + 1U] != 'u' || value[i + 2U] != '0' ||
            value[i + 3U] != '0' || value[i + 4U] != '0' || value[i + 5U] != '0') {
            continue;
        }

        size_t slash_count = 1;
        for (size_t j = i; j > 0 && value[j - 1U] == '\\'; --j) {
            ++slash_count;
        }
        if ((slash_count & 1U) != 0) {
            return true;
        }
    }
    return false;
}

static bool timeout_elapsed(uint32_t start, uint32_t timeout_ms) {
    return (uint32_t)(rtos_get_time() - start) >= timeout_ms;
}

static void build_ap_ssid(char output[WIFI_SSID_STR_LEN], const char *device_id) {
    uint8_t mac[WIFI_MAC_LEN] = {0};
    if (bk_wifi_ap_get_mac(mac) == BK_OK) {
        (void)snprintf(output, WIFI_SSID_STR_LEN, "mybot-%02x%02x", mac[4], mac[5]);
        return;
    }

    uint32_t hash = 2166136261U;
    for (size_t i = 0; device_id[i] != '\0'; ++i) {
        hash = (hash ^ (uint8_t)device_id[i]) * 16777619U;
    }
    (void)snprintf(output, WIFI_SSID_STR_LEN, "mybot-%04x", (unsigned)(hash & 0xffffU));
}

static int start_softap(portal_context_t *ctx) {
    if (bk7259_factory_reset_requested()) {
        return BK7259_NETWORK_CANCELLED;
    }
    netif_ip4_config_t ip4 = {0};
    (void)snprintf(ip4.ip, sizeof(ip4.ip), "%s", "192.168.4.1");
    (void)snprintf(ip4.mask, sizeof(ip4.mask), "%s", "255.255.255.0");
    (void)snprintf(ip4.gateway, sizeof(ip4.gateway), "%s", "192.168.4.1");
    (void)snprintf(ip4.dns, sizeof(ip4.dns), "%s", "192.168.4.1");
    if (bk_netif_set_ip4_config(NETIF_IF_AP, &ip4) != BK_OK) {
        MYBOT_LOGE(TAG, "failed to configure SoftAP IPv4");
        return -1;
    }

    wifi_ap_config_t config = {0};
    (void)snprintf(config.ssid, sizeof(config.ssid), "%s", ctx->ap_ssid);
    config.channel = PORTAL_AP_CHANNEL;
    config.security = WIFI_SECURITY_NONE;
    config.max_con = 2;
    config.disable_dns_server = 0;
    if (bk_wifi_ap_set_config(&config) != BK_OK || bk_wifi_ap_start() != BK_OK) {
        MYBOT_LOGE(TAG, "failed to configure or start SoftAP");
        (void)bk_wifi_ap_stop();
        return -1;
    }

    s_softap_started = true;
    bk7259_lcd_set_provisioning_ssid(ctx->ap_ssid);
    MYBOT_LOGI(TAG, "SoftAP started");
    return 0;
}

int bk7259_wifi_portal_stop(void) {
    if (!s_softap_started) {
        return 0;
    }
    if (bk_wifi_ap_stop() != BK_OK) {
        MYBOT_LOGE(TAG, "failed to stop SoftAP");
        return -1;
    }
    s_softap_started = false;
    bk7259_lcd_set_provisioning_ssid("");
    MYBOT_LOGI(TAG, "SoftAP stopped");
    return 0;
}

static int wait_socket(int fd, bool writable, uint32_t timeout_ms) {
    if (bk7259_factory_reset_requested()) {
        return BK7259_NETWORK_CANCELLED;
    }
    fd_set read_fds;
    fd_set write_fds;
    struct timeval timeout = {
        .tv_sec = (long)(timeout_ms / 1000U),
        .tv_usec = (long)((timeout_ms % 1000U) * 1000U),
    };

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    if (writable) {
        FD_SET(fd, &write_fds);
    } else {
        FD_SET(fd, &read_fds);
    }
    int result = select(fd + 1, writable ? NULL : &read_fds,
                        writable ? &write_fds : NULL, NULL, &timeout);
    return bk7259_factory_reset_requested()
               ? BK7259_NETWORK_CANCELLED
               : result;
}

static int send_all(int fd, const void *data, size_t length) {
    const uint8_t *cursor = data;
    uint32_t started = rtos_get_time();

    while (length > 0 && !timeout_elapsed(started, PORTAL_HTTP_IO_TIMEOUT_MS)) {
        int ready = wait_socket(fd, true, PORTAL_HTTP_POLL_MS);
        if (ready == BK7259_NETWORK_CANCELLED) {
            return ready;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ready == 0) {
            continue;
        }

        int sent = send(fd, cursor, length, 0);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (sent <= 0) {
            return -1;
        }
        cursor += (size_t)sent;
        length -= (size_t)sent;
    }
    return bk7259_factory_reset_requested()
               ? BK7259_NETWORK_CANCELLED
               : (length == 0 ? 0 : -1);
}

static int send_http_response(int fd, const char *status, const char *content_type,
                              const char *extra_headers, const char *body,
                              size_t body_length) {
    char header[256];
    int header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                 "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                                 status, content_type, (unsigned)body_length,
                                 extra_headers ? extra_headers : "");
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return -1;
    }
    int result = send_all(fd, header, (size_t)header_length);
    return result < 0 || body_length == 0 ? result : send_all(fd, body, body_length);
}

static char *trim_header_value(char *value) {
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) {
        value[--length] = '\0';
    }
    return value;
}

static int parse_content_length(const char *value, size_t *output) {
    if (!value || *value < '0' || *value > '9') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed > PORTAL_HTTP_BODY_MAX) {
        return -1;
    }
    *output = (size_t)parsed;
    return 0;
}

static int parse_http_header(char *buffer, char *header_end,
                             portal_http_request_t *request, size_t *content_length) {
    char *request_line_end = strstr(buffer, "\r\n");
    char version[16] = {0};
    char extra = '\0';
    if (!request_line_end || request_line_end >= header_end) {
        return -1;
    }
    *request_line_end = '\0';
    if (sscanf(buffer, "%7s %63s %15s %c", request->method, request->target,
               version, &extra) != 3 || strncmp(version, "HTTP/1.", 7) != 0 ||
        (version[7] != '0' && version[7] != '1') || version[8] != '\0') {
        return -1;
    }

    bool have_content_type = false;
    char *line = request_line_end + 2;
    while (line < header_end) {
        char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > header_end || line_end == line) {
            return -1;
        }
        *line_end = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line) {
            return -1;
        }
        *colon = '\0';
        char *value = trim_header_value(colon + 1);

        if (strcasecmp(line, "Content-Length") == 0) {
            if (request->has_content_length || parse_content_length(value, content_length) < 0) {
                return -1;
            }
            request->has_content_length = true;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            if (have_content_type) {
                return -1;
            }
            have_content_type = true;
            request->content_type_json = strcasecmp(value, "application/json") == 0;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0 ||
                   strcasecmp(line, "Expect") == 0) {
            return -1;
        }
        line = line_end + 2;
    }
    return 0;
}

static int receive_http_request(int fd, portal_http_request_t *request) {
    const size_t capacity = PORTAL_HTTP_HEADER_MAX + PORTAL_HTTP_BODY_MAX;
    char *buffer = os_malloc(capacity + 1U);
    if (!buffer) {
        return -1;
    }

    int result = -1;
    size_t received = 0;
    size_t required = 0;
    size_t header_length = 0;
    uint32_t started = rtos_get_time();
    while (received < capacity &&
           !timeout_elapsed(started, PORTAL_HTTP_REQUEST_TIMEOUT_MS)) {
        int ready = wait_socket(fd, false, PORTAL_HTTP_POLL_MS);
        if (ready == BK7259_NETWORK_CANCELLED) {
            result = ready;
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }

        int count = recv(fd, buffer + received, capacity - received, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        received += (size_t)count;
        buffer[received] = '\0';

        if (header_length == 0) {
            char *header_end = strstr(buffer, "\r\n\r\n");
            if (!header_end) {
                if (received >= PORTAL_HTTP_HEADER_MAX) {
                    break;
                }
                continue;
            }
            header_length = (size_t)(header_end - buffer) + 4U;
            size_t content_length = 0;
            if (header_length > PORTAL_HTTP_HEADER_MAX ||
                parse_http_header(buffer, header_end, request, &content_length) < 0) {
                break;
            }
            required = header_length + content_length;
        }

        if (header_length != 0 && received >= required) {
            if (received != required) {
                break;
            }
            request->body_length = required - header_length;
            if (request->body_length > 0) {
                memcpy(request->body, buffer + header_length, request->body_length);
            }
            request->body[request->body_length] = '\0';
            result = 0;
            break;
        }
    }

    secure_zero(buffer, received);
    os_free(buffer);
    return bk7259_factory_reset_requested()
               ? BK7259_NETWORK_CANCELLED
               : result;
}

static int security_to_authmode(wifi_security_t security) {
    switch (security) {
    case WIFI_SECURITY_NONE:
        return 0;
    case WIFI_SECURITY_WEP:
        return 1;
    case WIFI_SECURITY_WPA_TKIP:
    case WIFI_SECURITY_WPA_AES:
    case WIFI_SECURITY_WPA_MIXED:
        return 2;
    case WIFI_SECURITY_WPA2_TKIP:
    case WIFI_SECURITY_WPA2_AES:
    case WIFI_SECURITY_WPA2_MIXED:
        return 3;
    case WIFI_SECURITY_EAP:
        return 5;
    case WIFI_SECURITY_WPA3_SAE:
        return 6;
    case WIFI_SECURITY_WPA3_WPA2_MIXED:
        return 7;
    case WIFI_SECURITY_OWE:
        return 9;
    default:
        return 3;
    }
}

static char *build_scan_json(const wifi_scan_ap_info_t *results, size_t result_count) {
    cJSON *root = cJSON_CreateObject();
    cJSON *aps = NULL;
    if (!root || !cJSON_AddBoolToObject(root, "support_5g", false) ||
        !(aps = cJSON_AddArrayToObject(root, "aps"))) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < result_count; ++i) {
        size_t ssid_length = bounded_strlen(results[i].ssid, sizeof(results[i].ssid));
        if (ssid_length == 0 || ssid_length >= sizeof(results[i].ssid)) {
            continue;
        }
        cJSON *ap = cJSON_CreateObject();
        if (!ap || !cJSON_AddStringToObject(ap, "ssid", results[i].ssid) ||
            !cJSON_AddNumberToObject(ap, "rssi", results[i].rssi) ||
            !cJSON_AddNumberToObject(ap, "authmode",
                                     security_to_authmode(results[i].security)) ||
            !cJSON_AddItemToArray(aps, ap)) {
            cJSON_Delete(ap);
            cJSON_Delete(root);
            return NULL;
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && strlen(json) > PORTAL_SCAN_JSON_MAX) {
        cJSON_free(json);
        return NULL;
    }
    return json;
}

static int handle_scan(int fd) {
    MYBOT_LOGI(TAG, "Wi-Fi scan requested");
    wifi_scan_ap_info_t *results =
        os_zalloc(PORTAL_MAX_SCAN_RESULTS * sizeof(*results));
    if (!results) {
        MYBOT_LOGE(TAG, "Wi-Fi scan result allocation failed");
        return send_http_response(fd, "500 Internal Server Error", "application/json",
                                  NULL, "{\"aps\":[]}", sizeof("{\"aps\":[]}") - 1U);
    }

    size_t result_count = 0;
    int scan_result = bk7259_wifi_manager_scan_sync(
        results, PORTAL_MAX_SCAN_RESULTS, &result_count, PORTAL_SCAN_TIMEOUT_MS);
    if (result_count > PORTAL_MAX_SCAN_RESULTS) {
        scan_result = -1;
    }
    char *json = scan_result == 0 ? build_scan_json(results, result_count) : NULL;
    os_free(results);
    if (scan_result == BK7259_NETWORK_CANCELLED ||
        bk7259_factory_reset_requested()) {
        cJSON_free(json);
        return BK7259_NETWORK_CANCELLED;
    }
    if (!json) {
        MYBOT_LOGW(TAG, "Wi-Fi scan failed: result=%d count=%u", scan_result,
                (unsigned)result_count);
        return send_http_response(fd, scan_result == 0 ? "500 Internal Server Error"
                                                       : "503 Service Unavailable",
                                  "application/json", NULL, "{\"aps\":[]}",
                                  sizeof("{\"aps\":[]}") - 1U);
    }

    int result =
        send_http_response(fd, "200 OK", "application/json", NULL, json, strlen(json));
    cJSON_free(json);
    MYBOT_LOGI(TAG, "Wi-Fi scan complete: aps=%u response=%d",
            (unsigned)result_count, result);
    return result;
}

static int parse_submit_json(const char *body, size_t body_length,
                             char ssid[WIFI_SSID_STR_LEN],
                             char password[WIFI_PASSWORD_LEN]) {
    ssid[0] = '\0';
    password[0] = '\0';
    if (!body || body_length == 0 || memchr(body, '\0', body_length) ||
        contains_json_nul_escape(body, body_length)) {
        return -1;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, body_length + 1U, &parse_end, true);
    if (!cJSON_IsObject(root) || !parse_end || parse_end != body + body_length) {
        cJSON_Delete(root);
        return -1;
    }

    bool have_ssid = false;
    bool have_password = false;
    int result = -1;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!item->string || !cJSON_IsString(item) || !item->valuestring) {
            goto done;
        }

        char *output = NULL;
        size_t capacity = 0;
        bool *present = NULL;
        if (strcmp(item->string, "ssid") == 0) {
            output = ssid;
            capacity = WIFI_SSID_STR_LEN;
            present = &have_ssid;
        } else if (strcmp(item->string, "password") == 0) {
            output = password;
            capacity = WIFI_PASSWORD_LEN;
            present = &have_password;
        } else {
            goto done;
        }
        if (*present) {
            goto done;
        }

        size_t length = strlen(item->valuestring);
        if (length >= capacity) {
            goto done;
        }
        memcpy(output, item->valuestring, length + 1U);
        *present = true;
    }

    if (have_ssid && have_password && ssid[0] != '\0') {
        result = 0;
    }

done:
    cJSON_ArrayForEach(item, root) {
        if (item->string && strcmp(item->string, "password") == 0 &&
            cJSON_IsString(item) && item->valuestring) {
            secure_zero(item->valuestring, strlen(item->valuestring));
        }
    }
    cJSON_Delete(root);
    if (result < 0) {
        secure_zero(password, WIFI_PASSWORD_LEN);
        ssid[0] = '\0';
    }
    return result;
}

static int send_json_literal(int fd, const char *status, const char *json) {
    return send_http_response(fd, status, "application/json", NULL, json, strlen(json));
}

/* The probe URLs the phone operating systems fetch right after joining a
 * network to decide whether it is a captive portal. With the provisioning
 * SoftAP's DNS hijack every name resolves here, so answering these with a
 * redirect is what makes the browser open the configuration page on its own. */
static bool target_is_captive_probe(const char *target) {
    static const char *const probes[] = {
        "/hotspot-detect.html",      "/generate_204", "/mobile/status.php",
        "/check_network_status.txt", "/ncsi.txt",     "/fwlink/",
        "/connectivity-check.html",  "/success.txt",  "/portal.html",
        "/library/test/success.html",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        if (strncmp(target, probes[i], strlen(probes[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* Returns one only after verified credentials are saved, or CANCELLED when
 * factory reset takes precedence over the submitted network. */
static int handle_submit(int fd, const portal_http_request_t *request) {
    char ssid[WIFI_SSID_STR_LEN] = {0};
    char password[WIFI_PASSWORD_LEN] = {0};
    int result = 0;
    if (bk7259_factory_reset_requested()) {
        result = BK7259_NETWORK_CANCELLED;
        goto done;
    }

    if (!request->has_content_length || !request->content_type_json ||
        parse_submit_json(request->body, request->body_length, ssid, password) < 0) {
        MYBOT_LOGW(TAG, "Wi-Fi submit rejected: invalid request");
        result = send_json_literal(fd, "400 Bad Request",
                                   "{\"success\":false,\"error\":\"Invalid request\"}");
        goto done;
    }

    MYBOT_LOGI(TAG, "testing submitted Wi-Fi network");
    int connect_result = bk7259_wifi_manager_connect_candidate(
        ssid, password, PORTAL_CONNECT_TIMEOUT_MS);
    if (connect_result == BK7259_NETWORK_CANCELLED ||
        bk7259_factory_reset_requested()) {
        result = BK7259_NETWORK_CANCELLED;
        goto done;
    }
    if (connect_result < 0) {
        MYBOT_LOGW(TAG, "Wi-Fi submit rejected: connection failed");
        result = send_json_literal(
            fd, "409 Conflict",
            "{\"success\":false,\"error\":\"Unable to connect\"}");
        goto done;
    }
    int save_result = bk7259_wifi_manager_save_credentials(ssid, password);
    if (save_result == BK7259_NETWORK_CANCELLED ||
        bk7259_factory_reset_requested()) {
        result = BK7259_NETWORK_CANCELLED;
        goto done;
    }
    if (save_result < 0) {
        (void)bk7259_wifi_manager_disconnect();
        MYBOT_LOGE(TAG, "Wi-Fi submit failed: credential save failed");
        result = send_json_literal(
            fd, "500 Internal Server Error",
            "{\"success\":false,\"error\":\"Unable to save credentials\"}");
        goto done;
    }

    /* The APSTA channel may change as the STA joins the selected AP, so the
     * phone can leave our SoftAP before it receives this best-effort reply. */
    (void)send_json_literal(fd, "200 OK", "{\"success\":true}");
    if (bk7259_factory_reset_requested()) {
        result = BK7259_NETWORK_CANCELLED;
        goto done;
    }
    result = 1;
    MYBOT_LOGI(TAG, "Wi-Fi submit accepted");

done:
    secure_zero(password, sizeof(password));
    return result;
}

static int handle_http_client(int fd) {
    portal_http_request_t request = {0};
    int result = 0;
    int receive_result = receive_http_request(fd, &request);
    if (receive_result == BK7259_NETWORK_CANCELLED) {
        result = receive_result;
        goto done;
    }
    if (receive_result < 0) {
        (void)send_http_response(fd, "400 Bad Request", "text/plain", NULL,
                                 "Bad Request", sizeof("Bad Request") - 1U);
        goto done;
    }

    if (strcmp(request.method, "GET") == 0 && strcmp(request.target, "/") == 0 &&
        request.body_length == 0) {
        result = send_http_response(fd, "200 OK", "text/html; charset=utf-8", NULL,
                                    s_portal_html, sizeof(s_portal_html) - 1U);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.target, "/scan") == 0 &&
               request.body_length == 0) {
        result = handle_scan(fd);
    } else if (strcmp(request.method, "POST") == 0 &&
               strcmp(request.target, "/submit") == 0) {
        result = handle_submit(fd, &request);
    } else if (strcmp(request.method, "GET") == 0 && target_is_captive_probe(request.target)) {
        result = send_http_response(fd, "302 Found", "text/html",
                                    "Location: http://192.168.4.1/\r\n", NULL, 0);
    } else {
        result = send_http_response(fd, "404 Not Found", "text/plain", NULL,
                                    "Not Found", sizeof("Not Found") - 1U);
    }

done:
    secure_zero(request.body, sizeof(request.body));
    return bk7259_factory_reset_requested()
               ? BK7259_NETWORK_CANCELLED
               : result;
}

static int create_http_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    int reuse_address = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_HTTP_PORT),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int configure_client_socket(int fd) {
    /* select() and the socket I/O both have a short cancellation bound; a
     * readable/writable descriptor can still block in recv()/send(). */
    struct timeval timeout = {
        .tv_sec = (long)(PORTAL_HTTP_POLL_MS / 1000U),
        .tv_usec = (long)((PORTAL_HTTP_POLL_MS % 1000U) * 1000U),
    };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0
               ? 0
               : -1;
}

int bk7259_wifi_portal_run(const char *device_id) {
    if (!device_id || device_id[0] == '\0') {
        MYBOT_LOGE(TAG, "portal start rejected: missing device id");
        return -1;
    }
    if (bk7259_factory_reset_requested()) {
        return BK7259_NETWORK_CANCELLED;
    }
    if (bk7259_wifi_portal_stop() < 0) {
        MYBOT_LOGE(TAG, "portal start rejected: previous SoftAP still owned");
        return -1;
    }

    portal_context_t ctx = {0};
    build_ap_ssid(ctx.ap_ssid, device_id);
    int result = start_softap(&ctx);
    if (result < 0) {
        return result;
    }

    result = -1;
    int listen_fd = create_http_server();
    if (listen_fd < 0) {
        MYBOT_LOGE(TAG, "failed to start HTTP server, errno=%d", errno);
        goto done;
    }
    MYBOT_LOGI(TAG, "APSTA portal ready: ssid=%s url=http://192.168.4.1",
            ctx.ap_ssid);

    for (;;) {
        int ready = wait_socket(listen_fd, false, PORTAL_HTTP_POLL_MS);
        if (ready == BK7259_NETWORK_CANCELLED) {
            result = ready;
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            MYBOT_LOGE(TAG, "HTTP server select failed, errno=%d", errno);
            break;
        }
        if (ready == 0) {
            continue;
        }

        struct sockaddr_in client_address = {0};
        socklen_t client_length = sizeof(client_address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_address,
                               &client_length);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            MYBOT_LOGE(TAG, "HTTP server accept failed, errno=%d", errno);
            break;
        }

        int request_result = -1;
        if (configure_client_socket(client_fd) == 0) {
            request_result = handle_http_client(client_fd);
        }
        (void)shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        if (request_result == BK7259_NETWORK_CANCELLED ||
            bk7259_factory_reset_requested()) {
            result = BK7259_NETWORK_CANCELLED;
            break;
        }
        if (request_result == 1) {
            (void)rtos_delay_milliseconds(PORTAL_EXIT_DELAY_MS);
            result = bk7259_factory_reset_requested()
                         ? BK7259_NETWORK_CANCELLED
                         : 0;
            break;
        }
    }

    close(listen_fd);

done:
    if (bk7259_wifi_portal_stop() < 0) {
        result = -1;
    }
    if (bk7259_factory_reset_requested()) {
        (void)bk7259_wifi_manager_disconnect();
        result = BK7259_NETWORK_CANCELLED;
    }
    MYBOT_LOGI(TAG, "APSTA portal stopped: result=%d", result);
    return result;
}
