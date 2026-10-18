/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_tls_roots.h"

#include <mybot/platform/mybot_https.h>

#include <common/bk_err.h>
#include "bk7259_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <lwip/api.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/sockets.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_https"
#define DNS_THREAD_PRIORITY 2
#define DNS_THREAD_STACK_SIZE 4096

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt ca_chain;
} https_connection_t;

typedef struct {
    beken_mutex_t lock;
    beken_semaphore_t done;
    beken_thread_t worker;
    ip_addr_t address;
    err_t error;
    bool finished;
    bool caller_attached;
    char host[];
} dns_request_t;

static uint32_t tick_ms(void) {
    return (uint32_t)rtos_get_time();
}

static int deadline_remaining_ms(uint32_t start, uint32_t timeout_ms) {
    uint32_t elapsed = tick_ms() - start;
    return elapsed < timeout_ms ? (int)(timeout_ms - elapsed) : 0;
}

static void log_tls_error(const char *operation, int error) {
    char description[96];
    mbedtls_strerror(error, description, sizeof(description));
    MYBOT_LOGE(TAG, "%s failed (-0x%04x: %s)", operation,
            (unsigned int)(error < 0 ? -error : error), description);
}

static void dns_request_destroy(dns_request_t *request) {
    if (request->done) {
        rtos_deinit_semaphore(&request->done);
    }
    if (request->lock) {
        rtos_deinit_mutex(&request->lock);
    }
    psram_free(request);
}

static void dns_worker(beken_thread_arg_t arg) {
    dns_request_t *request = arg;
    ip_addr_t address;
    err_t error = netconn_gethostbyname_addrtype(request->host, &address,
                                                 NETCONN_DNS_IPV4_IPV6);
    bool orphaned = false;

    if (rtos_lock_mutex(&request->lock) == BK_OK) {
        request->error = error;
        if (error == ERR_OK) {
            request->address = address;
        }
        request->finished = true;
        orphaned = !request->caller_attached;
        rtos_unlock_mutex(&request->lock);
    }

    rtos_set_semaphore(&request->done);
    if (orphaned) {
        dns_request_destroy(request);
    }
    rtos_delete_thread(NULL);
}

static int resolve_host(ip_addr_t *address, const char *host, uint32_t start,
                        uint32_t timeout_ms) {
    int remaining_ms = deadline_remaining_ms(start, timeout_ms);
    if (remaining_ms <= 0) {
        return -1;
    }

    size_t host_size = strlen(host) + 1;
    if (host_size == 0 || host_size > SIZE_MAX - sizeof(dns_request_t)) {
        return -1;
    }

    dns_request_t *request = psram_zalloc(sizeof(*request) + host_size);
    if (!request) {
        return -1;
    }
    memcpy(request->host, host, host_size);
    request->caller_attached = true;

    if (rtos_init_mutex(&request->lock) != BK_OK ||
        rtos_init_semaphore(&request->done, 1) != BK_OK ||
        rtos_create_psram_thread(&request->worker, DNS_THREAD_PRIORITY, "mybot_dns",
                                 dns_worker, DNS_THREAD_STACK_SIZE, request) != BK_OK) {
        dns_request_destroy(request);
        return -1;
    }

    remaining_ms = deadline_remaining_ms(start, timeout_ms);
    bk_err_t wait_result = remaining_ms > 0
                               ? rtos_get_semaphore(&request->done, (uint32_t)remaining_ms)
                               : BK_ERR_TIMEOUT;
    if (rtos_lock_mutex(&request->lock) != BK_OK) {
        rtos_thread_join(&request->worker);
        dns_request_destroy(request);
        return -1;
    }

    bool finished = request->finished;
    err_t error = request->error;
    if (finished && error == ERR_OK) {
        *address = request->address;
    }
    request->caller_attached = false;
    rtos_unlock_mutex(&request->lock);

    if (!finished) {
        MYBOT_LOGE(TAG, "DNS timeout, host=%s", host);
        return -1;
    }

    rtos_thread_join(&request->worker);
    dns_request_destroy(request);
    return wait_result == BK_OK && error == ERR_OK ? 0 : -1;
}

static int wait_for_socket(mbedtls_net_context *net, uint32_t event, uint32_t start,
                           uint32_t timeout_ms) {
    if (!net || net->fd < LWIP_SOCKET_OFFSET || net->fd >= LWIP_SELECT_MAXNFDS ||
        (event != MBEDTLS_NET_POLL_READ && event != MBEDTLS_NET_POLL_WRITE)) {
        return -1;
    }
    int remaining_ms = deadline_remaining_ms(start, timeout_ms);
    if (remaining_ms <= 0) {
        return -1;
    }

    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(net->fd, &ready);
    struct timeval timeout = {
        .tv_sec = remaining_ms / 1000,
        .tv_usec = (remaining_ms % 1000) * 1000,
    };
    int ret = select(net->fd + 1,
                     event == MBEDTLS_NET_POLL_READ ? &ready : NULL,
                     event == MBEDTLS_NET_POLL_WRITE ? &ready : NULL,
                     NULL, &timeout);
    return ret > 0 && FD_ISSET(net->fd, &ready) ? 0 : -1;
}

/* The vendor socket errno is global. Read the connection's own SO_ERROR instead,
 * so simultaneous control/background HTTPS requests cannot change this result. */
static int socket_io_result(mbedtls_net_context *net, int result, int want,
                            int failure) {
    if (result >= 0) {
        return result;
    }

    int error = 0;
    socklen_t error_len = sizeof(error);
    if (getsockopt(net->fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0) {
        return failure;
    }
    if (error == 0 || error == EAGAIN || error == EINPROGRESS || error == EINTR) {
        return want;
    }
    return error == EPIPE || error == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : failure;
}

static int socket_send(void *context, const unsigned char *data, size_t len) {
    mbedtls_net_context *net = context;
    if (!net || net->fd < 0 || (!data && len > 0) || len > INT_MAX) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    int ret = (int)send(net->fd, data, len, 0);
    return socket_io_result(net, ret, MBEDTLS_ERR_SSL_WANT_WRITE,
                             MBEDTLS_ERR_NET_SEND_FAILED);
}

static int socket_recv(void *context, unsigned char *data, size_t len) {
    mbedtls_net_context *net = context;
    if (!net || net->fd < 0 || (!data && len > 0) || len > INT_MAX) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    int ret = (int)recv(net->fd, data, len, 0);
    return socket_io_result(net, ret, MBEDTLS_ERR_SSL_WANT_READ,
                             MBEDTLS_ERR_NET_RECV_FAILED);
}

static int tcp_connect(mbedtls_net_context *net, const char *host, uint16_t port,
                       uint32_t start, uint32_t timeout_ms) {
    ip_addr_t address;
    if (resolve_host(&address, host, start, timeout_ms) < 0 ||
        deadline_remaining_ms(start, timeout_ms) <= 0) {
        return -1;
    }

    struct sockaddr_storage storage;
    memset(&storage, 0, sizeof(storage));
    socklen_t address_len;
    if (IP_IS_V4(&address)) {
        struct sockaddr_in *target = (struct sockaddr_in *)&storage;
        target->sin_family = AF_INET;
        target->sin_port = htons(port);
        target->sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&address));
        address_len = sizeof(*target);
    }
#if LWIP_IPV6
    else if (IP_IS_V6(&address)) {
        struct sockaddr_in6 *target = (struct sockaddr_in6 *)&storage;
        target->sin6_family = AF_INET6;
        target->sin6_port = htons(port);
        memcpy(&target->sin6_addr, ip_2_ip6(&address)->addr, sizeof(target->sin6_addr));
        target->sin6_scope_id = ip6_addr_zone(ip_2_ip6(&address));
        address_len = sizeof(*target);
    }
#endif
    else {
        return -1;
    }

    int fd = socket(((struct sockaddr *)&storage)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    net->fd = fd;
    if (mbedtls_net_set_nonblock(net) != 0) {
        mbedtls_net_free(net);
        return -1;
    }

    int ret = connect(fd, (const struct sockaddr *)&storage, address_len);
    if (ret < 0 &&
        wait_for_socket(net, MBEDTLS_NET_POLL_WRITE, start, timeout_ms) < 0) {
        mbedtls_net_free(net);
        return -1;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
        socket_error != 0 ||
        getpeername(fd, (struct sockaddr *)&storage, &address_len) != 0) {
        mbedtls_net_free(net);
        return -1;
    }
    return 0;
}

static int wait_for_tls(https_connection_t *connection, int error, uint32_t start,
                        uint32_t timeout_ms) {
    if (error == MBEDTLS_ERR_SSL_WANT_READ) {
        return wait_for_socket(&connection->net, MBEDTLS_NET_POLL_READ, start, timeout_ms);
    }
    if (error == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return wait_for_socket(&connection->net, MBEDTLS_NET_POLL_WRITE, start, timeout_ms);
    }
    return -1;
}

static int tls_handshake(https_connection_t *connection, uint32_t start,
                         uint32_t timeout_ms) {
    for (;;) {
        if (deadline_remaining_ms(start, timeout_ms) <= 0) {
            return -1;
        }
        int ret = mbedtls_ssl_handshake(&connection->ssl);
        if (ret == 0) {
            return 0;
        }
        if (wait_for_tls(connection, ret, start, timeout_ms) < 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                log_tls_error("TLS handshake", ret);
            }
            return -1;
        }
    }
}

static void https_close(void *opaque_connection) {
    https_connection_t *connection = opaque_connection;
    if (!connection) {
        return;
    }

    if (connection->net.fd >= 0) {
        int ret = mbedtls_ssl_close_notify(&connection->ssl);
        if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_tls_error("TLS close", ret);
        }
    }
    mbedtls_net_free(&connection->net);
    mbedtls_ssl_free(&connection->ssl);
    mbedtls_ssl_config_free(&connection->config);
    mbedtls_ctr_drbg_free(&connection->ctr_drbg);
    mbedtls_entropy_free(&connection->entropy);
    mbedtls_x509_crt_free(&connection->ca_chain);
    psram_free(connection);
}

static int https_connect(void **out_connection, const char *host, uint16_t port,
                         int timeout_ms) {
    if (!out_connection || !host || !host[0] || port == 0 || timeout_ms <= 0) {
        return -1;
    }
    *out_connection = NULL;

    uint32_t start = tick_ms();
    uint32_t timeout = (uint32_t)timeout_ms;
    https_connection_t *connection = psram_zalloc(sizeof(*connection));
    if (!connection) {
        return -1;
    }

    mbedtls_net_init(&connection->net);
    mbedtls_ssl_init(&connection->ssl);
    mbedtls_ssl_config_init(&connection->config);
    mbedtls_ctr_drbg_init(&connection->ctr_drbg);
    mbedtls_entropy_init(&connection->entropy);
    mbedtls_x509_crt_init(&connection->ca_chain);

    static const unsigned char personalization[] = "mybot-bk7259-https";
    int ret = mbedtls_ctr_drbg_seed(&connection->ctr_drbg, mbedtls_entropy_func,
                                    &connection->entropy, personalization,
                                    sizeof(personalization) - 1);
    if (ret != 0) {
        log_tls_error("random seed", ret);
        goto fail;
    }

    ret = mbedtls_ssl_config_defaults(&connection->config, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_tls_error("TLS configuration", ret);
        goto fail;
    }
    ret = mbedtls_x509_crt_parse(&connection->ca_chain, MYBOT_BK7259_TLS_ROOTS_PEM,
                                 sizeof(MYBOT_BK7259_TLS_ROOTS_PEM));
    if (ret == 0) {
        ret = mbedtls_x509_crt_parse(&connection->ca_chain,
                                     MYBOT_BK7259_DIGICERT_GLOBAL_ROOT_G2_PEM,
                                     sizeof(MYBOT_BK7259_DIGICERT_GLOBAL_ROOT_G2_PEM));
    }
    if (ret != 0) {
        log_tls_error("CA parse", ret);
        goto fail;
    }

    mbedtls_ssl_conf_min_tls_version(&connection->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ca_chain(&connection->config, &connection->ca_chain, NULL);
    mbedtls_ssl_conf_authmode(&connection->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&connection->config, mbedtls_ctr_drbg_random,
                         &connection->ctr_drbg);

    ret = mbedtls_ssl_setup(&connection->ssl, &connection->config);
    if (ret != 0) {
        log_tls_error("TLS setup", ret);
        goto fail;
    }
    ret = mbedtls_ssl_set_hostname(&connection->ssl, host);
    if (ret != 0) {
        log_tls_error("TLS hostname", ret);
        goto fail;
    }
    if (tcp_connect(&connection->net, host, port, start, timeout) < 0) {
        goto fail;
    }

    mbedtls_ssl_set_bio(&connection->ssl, &connection->net, socket_send,
                        socket_recv, NULL);
    if (tls_handshake(connection, start, timeout) < 0 ||
        mbedtls_ssl_get_verify_result(&connection->ssl) != 0) {
        goto fail;
    }

    *out_connection = connection;
    return 0;

fail:
    https_close(connection);
    return -1;
}

static int https_send(void *opaque_connection, const void *data, size_t len,
                      int timeout_ms) {
    https_connection_t *connection = opaque_connection;
    if (!connection || !data || len == 0 || timeout_ms <= 0) {
        return -1;
    }
    if (len > INT_MAX) {
        len = INT_MAX;
    }

    uint32_t start = tick_ms();
    uint32_t timeout = (uint32_t)timeout_ms;
    for (;;) {
        if (deadline_remaining_ms(start, timeout) <= 0) {
            return -1;
        }
        int ret = mbedtls_ssl_write(&connection->ssl, data, len);
        if (ret > 0) {
            return ret;
        }
        if (wait_for_tls(connection, ret, start, timeout) < 0) {
            return -1;
        }
    }
}

static int https_recv(void *opaque_connection, void *data, size_t capacity,
                      int timeout_ms) {
    https_connection_t *connection = opaque_connection;
    if (!connection || !data || capacity == 0 || timeout_ms <= 0) {
        return -1;
    }
    if (capacity > INT_MAX) {
        capacity = INT_MAX;
    }

    uint32_t start = tick_ms();
    uint32_t timeout = (uint32_t)timeout_ms;
    for (;;) {
        if (deadline_remaining_ms(start, timeout) <= 0) {
            return -1;
        }
        int ret = mbedtls_ssl_read(&connection->ssl, data, capacity);
        if (ret > 0) {
            return ret;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        if (wait_for_tls(connection, ret, start, timeout) < 0) {
            return -1;
        }
    }
}

const mybot_https_ops_t g_mybot_bk7259_https_ops = {
    .connect = https_connect,
    .send = https_send,
    .recv = https_recv,
    .close = https_close,
};
