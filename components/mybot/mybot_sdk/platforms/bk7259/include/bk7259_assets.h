/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_ASSETS_H_
#define BK7259_ASSETS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bk7259_asset_s {
    const uint8_t *data;
    size_t size;
} bk7259_asset_t;

/* Resolve a firmware-embedded asset by its mybot/assets/... path. */
int bk7259_asset_find(const char *path, bk7259_asset_t *asset);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_ASSETS_H_ */
