/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_OGG_PCM_H_
#define BK7259_OGG_PCM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bk7259_ogg_pcm_s {
    int16_t *pcm;     /* decoded s16le mono samples, freed by bk7259_ogg_pcm_free() */
    int frames;       /* number of samples */
    int sample_rate;  /* decode/output rate */
} bk7259_ogg_pcm_t;

/* Decode an in-memory Ogg/Opus asset to s16le mono PCM at out_rate
 * (8000/12000/16000/24000/48000). The compressed asset remains in flash;
 * only the decoded PCM is allocated in PSRAM. */
int bk7259_ogg_pcm_load_memory(const char *path, const uint8_t *data, size_t size,
                              int out_rate, bk7259_ogg_pcm_t *out);

/* Release the PCM buffer returned by bk7259_ogg_pcm_load_memory(). Safe on a zeroed
 * struct (no-op). */
void bk7259_ogg_pcm_free(bk7259_ogg_pcm_t *ogg);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_OGG_PCM_H_ */
