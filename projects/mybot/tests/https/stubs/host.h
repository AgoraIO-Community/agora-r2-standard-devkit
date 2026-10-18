/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <common/bk_err.h>

#define BK_ERR_TIMEOUT (-1)
#define LWIP_IPV6 0
#define LWIP_SOCKET_OFFSET 0
#define LWIP_SELECT_MAXNFDS FD_SETSIZE
typedef void *beken_mutex_t;
typedef void *beken_semaphore_t;
typedef void *beken_thread_t;
typedef void *beken_thread_arg_t;
typedef int err_t;
typedef struct { uint32_t addr; } ip_addr_t;
#define ERR_OK 0
#define NETCONN_DNS_IPV4_IPV6 0
#define IP_IS_V4(address) 1
#define ip_2_ip4(address) (address)
#define ip4_addr_get_u32(address) ((address)->addr)
#define psram_zalloc(size) calloc(1, size)
#define psram_free(pointer) free(pointer)

bk_err_t rtos_init_mutex(beken_mutex_t *mutex);
bk_err_t rtos_deinit_mutex(beken_mutex_t *mutex);
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_init_semaphore(beken_semaphore_t *semaphore, int max_count);
bk_err_t rtos_deinit_semaphore(beken_semaphore_t *semaphore);
bk_err_t rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout_ms);
bk_err_t rtos_set_semaphore(beken_semaphore_t *semaphore);
bk_err_t rtos_create_psram_thread(beken_thread_t *thread, uint8_t priority,
                                const char *name, void (*entry)(void *),
                                uint32_t stack_size, void *arg);
bk_err_t rtos_delete_thread(beken_thread_t *thread);
bk_err_t rtos_thread_join(beken_thread_t *thread);
uint32_t rtos_get_time(void);
err_t netconn_gethostbyname_addrtype(const char *host, ip_addr_t *address, int type);

int test_socket(int domain, int type, int protocol);
int test_connect(int fd, const struct sockaddr *address, socklen_t address_len);
int test_getsockopt(int fd, int level, int name, void *value, socklen_t *value_len);
int test_getpeername(int fd, struct sockaddr *address, socklen_t *address_len);
int test_select(int max_fd, fd_set *read, fd_set *write, fd_set *except,
                struct timeval *timeout);
ssize_t test_send(int fd, const void *data, size_t len, int flags);
ssize_t test_recv(int fd, void *data, size_t len, int flags);
#define socket(...) test_socket(__VA_ARGS__)
#define connect(...) test_connect(__VA_ARGS__)
#define getsockopt(...) test_getsockopt(__VA_ARGS__)
#define getpeername(...) test_getpeername(__VA_ARGS__)
#define select(...) test_select(__VA_ARGS__)
#define send(...) test_send(__VA_ARGS__)
#define recv(...) test_recv(__VA_ARGS__)
