#ifndef SWING_CAPTURE_EMBEDDED_PROP_MAKER_PCM_CLIP_H_
#define SWING_CAPTURE_EMBEDDED_PROP_MAKER_PCM_CLIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "embedded/prop_maker/hil_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum swing_hil_pcm_result {
  SWING_HIL_PCM_OK = 0,
  SWING_HIL_PCM_INVALID_STATE,
  SWING_HIL_PCM_WRONG_REQUEST,
  SWING_HIL_PCM_OFFSET,
  SWING_HIL_PCM_OVERFLOW,
  SWING_HIL_PCM_INCOMPLETE,
  SWING_HIL_PCM_CHECKSUM,
  SWING_HIL_PCM_NOT_COMMITTED,
  SWING_HIL_PCM_MARKER,
} swing_hil_pcm_result;

typedef struct swing_hil_pcm_clip {
  int16_t samples[SWING_HIL_PCM_MAX_SAMPLES];
  uint32_t request_id;
  uint32_t expected_sample_count;
  uint32_t expected_crc32;
  uint32_t received_bytes;
  bool uploading;
  bool committed;
} swing_hil_pcm_clip;

void swing_hil_pcm_reset(swing_hil_pcm_clip *clip);
swing_hil_pcm_result swing_hil_pcm_begin(swing_hil_pcm_clip *clip, uint32_t request_id,
                                         uint32_t sample_count, uint32_t expected_crc32);
swing_hil_pcm_result swing_hil_pcm_append(swing_hil_pcm_clip *clip, uint32_t request_id,
                                          uint32_t byte_offset, const uint8_t *bytes,
                                          uint32_t byte_count);
swing_hil_pcm_result swing_hil_pcm_commit(swing_hil_pcm_clip *clip, uint32_t request_id);
swing_hil_pcm_result swing_hil_pcm_abort(swing_hil_pcm_clip *clip, uint32_t request_id);
swing_hil_pcm_result swing_hil_pcm_validate_play(const swing_hil_pcm_clip *clip,
                                                 uint32_t marker_sample);
uint32_t swing_hil_pcm_crc32(const uint8_t *bytes, size_t byte_count);
int16_t swing_hil_pcm_scale_sample(int16_t sample, uint32_t gain_permille);
uint32_t swing_hil_pcm_scaled_crc32(const int16_t *samples, size_t sample_count,
                                    uint32_t gain_permille);
const char *swing_hil_pcm_result_code(swing_hil_pcm_result result);

#ifdef __cplusplus
}
#endif

#endif  // SWING_CAPTURE_EMBEDDED_PROP_MAKER_PCM_CLIP_H_
