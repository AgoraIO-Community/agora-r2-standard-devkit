/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../../../../components/mybot/mybot_sdk/platforms/bk7259/bk7259_https.c"

typedef struct {
    int io_result;
    int socket_error;
    int option_result;
    int connect_result;
    int peer_result;
    int poll_result;
    unsigned int polls;
    unsigned int option_reads;
    unsigned int closes;
} fake_socket_t;
static fake_socket_t sockets[4];
static uint32_t now_ms;
static uint32_t last_wait_ms;
static int next_fd = 1;
static int unrelated_errno;

static void reset(void) {
    memset(sockets, 0, sizeof(sockets));
    now_ms = 0;
    last_wait_ms = 0;
    next_fd = 1;
    unrelated_errno = 0;
    for (size_t i = 0; i < 4; i++) {
        sockets[i].poll_result = 1;
    }
}

uint32_t rtos_get_time(void) { return now_ms; }
void aosl_log(int level, const char *format, ...) {}
bk_err_t rtos_init_mutex(beken_mutex_t *mutex) { *mutex = (void *)1; return BK_OK; }
bk_err_t rtos_deinit_mutex(beken_mutex_t *mutex) { *mutex = NULL; return BK_OK; }
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex) { return BK_OK; }
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex) { return BK_OK; }
bk_err_t rtos_init_semaphore(beken_semaphore_t *semaphore, int max_count) {
    *semaphore = (void *)1; return BK_OK;
}
bk_err_t rtos_deinit_semaphore(beken_semaphore_t *semaphore) {
    *semaphore = NULL; return BK_OK;
}
bk_err_t rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout_ms) {
    return BK_OK;
}
bk_err_t rtos_set_semaphore(beken_semaphore_t *semaphore) { return BK_OK; }
bk_err_t rtos_create_psram_thread(beken_thread_t *thread, uint8_t priority,
                                const char *name, void (*entry)(void *),
                                uint32_t stack_size, void *arg) {
    *thread = (void *)1;
    entry(arg);
    return BK_OK;
}
bk_err_t rtos_delete_thread(beken_thread_t *thread) { return BK_OK; }
bk_err_t rtos_thread_join(beken_thread_t *thread) { return BK_OK; }
err_t netconn_gethostbyname_addrtype(const char *host, ip_addr_t *address, int type) {
    address->addr = htonl(0x7f000001u);
    return ERR_OK;
}

int test_socket(int domain, int type, int protocol) { return next_fd; }
int test_connect(int fd, const struct sockaddr *address, socklen_t address_len) {
    errno = unrelated_errno;
    return sockets[fd].connect_result;
}
int test_getsockopt(int fd, int level, int name, void *value, socklen_t *value_len) {
    assert(level == SOL_SOCKET && name == SO_ERROR && *value_len == sizeof(int));
    sockets[fd].option_reads++;
    *(int *)value = sockets[fd].socket_error;
    errno = unrelated_errno;
    return sockets[fd].option_result;
}
int test_getpeername(int fd, struct sockaddr *address, socklen_t *address_len) {
    errno = unrelated_errno;
    return sockets[fd].peer_result;
}
int test_select(int max_fd, fd_set *read, fd_set *write, fd_set *except,
                struct timeval *timeout) {
    assert(max_fd > 0 && max_fd < 5);
    assert((read != NULL) != (write != NULL));
    assert(except == NULL && timeout != NULL);
    assert(timeout->tv_sec >= 0 && timeout->tv_usec >= 0 && timeout->tv_usec < 1000000);
    int fd = max_fd - 1;
    assert(FD_ISSET(fd, read ? read : write));
    last_wait_ms = (uint32_t)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    sockets[fd].polls++;
    if (sockets[fd].poll_result == 0) {
        now_ms += last_wait_ms;
    }
    errno = unrelated_errno;
    return sockets[fd].poll_result;
}
ssize_t test_send(int fd, const void *data, size_t len, int flags) {
    errno = unrelated_errno;
    return sockets[fd].io_result;
}
ssize_t test_recv(int fd, void *data, size_t len, int flags) {
    errno = unrelated_errno;
    if (sockets[fd].io_result > 0) {
        assert((size_t)sockets[fd].io_result <= len);
        memset(data, 'x', (size_t)sockets[fd].io_result);
    }
    return sockets[fd].io_result;
}
int mbedtls_net_set_nonblock(mbedtls_net_context *net) { return 0; }
void mbedtls_net_free(mbedtls_net_context *net) {
    sockets[net->fd].closes++;
    net->fd = -1;
}
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *data, size_t len) {
    return socket_send(&((https_connection_t *)ssl->MBEDTLS_PRIVATE(p_bio))->net, data, len);
}
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *data, size_t len) {
    return socket_recv(&((https_connection_t *)ssl->MBEDTLS_PRIVATE(p_bio))->net, data, len);
}

static void test_independent_errors(void) {
    reset();
    unsigned char data[8] = {0};
    mbedtls_net_context a = {.fd = 1}, b = {.fd = 2};
    sockets[1].io_result = -1;
    sockets[2].io_result = -1;
    sockets[2].socket_error = ENETDOWN;
    unrelated_errno = ECONNRESET;
    assert(socket_send(&a, data, sizeof(data)) == MBEDTLS_ERR_SSL_WANT_WRITE);
    assert(socket_recv(&a, data, sizeof(data)) == MBEDTLS_ERR_SSL_WANT_READ);
    unrelated_errno = EAGAIN;
    assert(socket_send(&b, data, sizeof(data)) == MBEDTLS_ERR_NET_SEND_FAILED);
    assert(socket_recv(&b, data, sizeof(data)) == MBEDTLS_ERR_NET_RECV_FAILED);
    sockets[2].socket_error = ECONNRESET;
    assert(socket_recv(&b, data, sizeof(data)) == MBEDTLS_ERR_NET_CONN_RESET);
    sockets[1].option_result = -1;
    assert(socket_recv(&a, data, sizeof(data)) == MBEDTLS_ERR_NET_RECV_FAILED);
    assert(sockets[1].option_reads == 3 && sockets[2].option_reads == 3);
}

static void test_progress_and_eof(void) {
    reset();
    unsigned char data[8] = {0};
    mbedtls_net_context net = {.fd = 1};
    sockets[1].io_result = 3;
    sockets[1].socket_error = ECONNRESET;
    assert(socket_send(&net, data, sizeof(data)) == 3);
    assert(socket_recv(&net, data, sizeof(data)) == 3 && data[0] == 'x');
    sockets[1].io_result = 0;
    assert(socket_recv(&net, data, sizeof(data)) == 0);
    assert(sockets[1].option_reads == 0);
    assert(socket_send(NULL, data, sizeof(data)) == MBEDTLS_ERR_NET_SEND_FAILED);
    assert(socket_recv(&net, NULL, sizeof(data)) == MBEDTLS_ERR_NET_RECV_FAILED);
    assert(socket_recv(&net, data, (size_t)INT_MAX + 1) == MBEDTLS_ERR_NET_RECV_FAILED);
}

static void test_poll_deadline(void) {
    reset();
    mbedtls_net_context net = {.fd = 1};
    now_ms = 250;
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_READ, 0, 1500) == 0);
    assert(last_wait_ms == 1250);
    unrelated_errno = EINTR;
    sockets[1].poll_result = -1;
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_WRITE, 0, 1500) == -1);
    assert(sockets[1].polls == 2);
    sockets[1].poll_result = 0;
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_READ, 0, 1500) == -1);
    assert(now_ms == 1500);
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_READ, 0, 1500) == -1);
    assert(sockets[1].polls == 3);
    net.fd = FD_SETSIZE;
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_READ, 0, 2000) == -1);
    net.fd = -1;
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_READ, 0, 2000) == -1);
    now_ms = 20;
    net.fd = 1;
    sockets[1].poll_result = 1;
    assert(wait_for_socket(&net, MBEDTLS_NET_POLL_READ, UINT32_MAX - 9, 100) == 0);
    assert(last_wait_ms == 70);
}

static void test_connect_without_errno(void) {
    reset();
    mbedtls_net_context net = {.fd = -1};
    sockets[1].connect_result = -1;
    unrelated_errno = ECONNRESET;
    assert(tcp_connect(&net, "test.local", 443, 0, 500) == 0);
    assert(net.fd == 1 && sockets[1].polls == 1 && sockets[1].closes == 0);
    reset();
    sockets[1].connect_result = -1;
    sockets[1].socket_error = ECONNREFUSED;
    unrelated_errno = EAGAIN;
    assert(tcp_connect(&net, "test.local", 443, 0, 500) == -1);
    assert(net.fd == -1 && sockets[1].closes == 1);
    reset();
    sockets[1].connect_result = -1;
    sockets[1].peer_result = -1;
    assert(tcp_connect(&net, "test.local", 443, 0, 500) == -1);
    assert(sockets[1].closes == 1);
    reset();
    sockets[1].connect_result = -1;
    sockets[1].poll_result = 0;
    assert(tcp_connect(&net, "test.local", 443, 0, 500) == -1);
    assert(now_ms == 500 && sockets[1].closes == 1);
}

static void test_tls_io_deadline(void) {
    reset();
    https_connection_t connection = {0};
    connection.net.fd = 1;
    connection.ssl.MBEDTLS_PRIVATE(p_bio) = &connection;
    unsigned char data[8] = {0};
    sockets[1].io_result = -1;
    sockets[1].poll_result = 0;
    unrelated_errno = ECONNRESET;
    assert(https_send(&connection, data, sizeof(data), 300) == -1);
    assert(now_ms == 300 && sockets[1].polls == 1);
    sockets[1].polls = 0;
    assert(https_recv(&connection, data, sizeof(data), 200) == -1);
    assert(now_ms == 500 && sockets[1].polls == 1);
    sockets[1].io_result = 3;
    assert(https_send(&connection, data, sizeof(data), 100) == 3);
    assert(https_recv(&connection, data, sizeof(data), 100) == 3);
    sockets[1].io_result = 0;
    assert(https_recv(&connection, data, sizeof(data), 100) == 0);
}

int main(void) {
    test_independent_errors();
    test_progress_and_eof();
    test_poll_deadline();
    test_connect_without_errno();
    test_tls_io_deadline();
    puts("BK7259 HTTPS: 5 cases passed");
    return 0;
}
