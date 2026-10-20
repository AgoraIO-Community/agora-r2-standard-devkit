/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
typedef struct {
  unsigned unused;
} mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 6
const mbedtls_md_info_t *mbedtls_md_info_from_type(int);
int mbedtls_md_hmac(const mbedtls_md_info_t *, const unsigned char *, size_t,
                    const unsigned char *, size_t, unsigned char *);
