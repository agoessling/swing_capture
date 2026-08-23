#include "embedded/prop_maker/pcm_clip.h"

#include <stddef.h>
#include <stdint.h>

#include "embedded/prop_maker/hil_protocol.h"

static void invalidate(swing_hil_pcm_clip *clip) {
  clip->request_id = 0U;
  clip->expected_sample_count = 0U;
  clip->expected_crc32 = 0U;
  clip->received_bytes = 0U;
  clip->uploading = false;
  clip->committed = false;
}

void swing_hil_pcm_reset(swing_hil_pcm_clip *clip) { invalidate(clip); }

// Protocol fields intentionally retain their wire-order scalar types.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
swing_hil_pcm_result swing_hil_pcm_begin(swing_hil_pcm_clip *clip, uint32_t request_id,
                                         uint32_t sample_count, uint32_t expected_crc32) {
  invalidate(clip);
  if (request_id == 0U || sample_count == 0U || sample_count > SWING_HIL_PCM_MAX_SAMPLES) {
    return SWING_HIL_PCM_OVERFLOW;
  }
  clip->request_id = request_id;
  clip->expected_sample_count = sample_count;
  clip->expected_crc32 = expected_crc32;
  clip->uploading = true;
  return SWING_HIL_PCM_OK;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// Protocol fields intentionally retain their wire-order scalar types.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
swing_hil_pcm_result swing_hil_pcm_append(swing_hil_pcm_clip *clip, uint32_t request_id,
                                          uint32_t byte_offset, const uint8_t *bytes,
                                          uint32_t byte_count) {
  if (!clip->uploading || clip->committed) {
    invalidate(clip);
    return SWING_HIL_PCM_INVALID_STATE;
  }
  if (request_id != clip->request_id) {
    invalidate(clip);
    return SWING_HIL_PCM_WRONG_REQUEST;
  }
  if (byte_offset != clip->received_bytes) {
    invalidate(clip);
    return SWING_HIL_PCM_OFFSET;
  }
  const uint32_t expected_bytes = clip->expected_sample_count * 2U;
  if (byte_count == 0U || byte_count > expected_bytes - clip->received_bytes) {
    invalidate(clip);
    return SWING_HIL_PCM_OVERFLOW;
  }
  uint8_t *destination = (uint8_t *)clip->samples;
  for (uint32_t index = 0U; index < byte_count; ++index) {
    destination[clip->received_bytes + index] = bytes[index];
  }
  clip->received_bytes += byte_count;
  return SWING_HIL_PCM_OK;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

uint32_t swing_hil_pcm_crc32(const uint8_t *bytes, size_t byte_count) {
  uint32_t checksum = UINT32_MAX;
  for (size_t index = 0U; index < byte_count; ++index) {
    checksum ^= bytes[index];
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (checksum & 1U);
      checksum = (checksum >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return checksum ^ UINT32_MAX;
}

int16_t swing_hil_pcm_scale_sample(int16_t sample, uint32_t gain_permille) {
  return (int16_t)(((int32_t)sample * (int32_t)gain_permille) / 1000);
}

// Count and gain retain the scalar types used by the bounded firmware buffers and protocol.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
uint32_t swing_hil_pcm_scaled_crc32(const int16_t *samples, size_t sample_count,
                                    uint32_t gain_permille) {
  uint32_t checksum = UINT32_MAX;
  for (size_t index = 0U; index < sample_count; ++index) {
    const uint16_t scaled = (uint16_t)swing_hil_pcm_scale_sample(samples[index], gain_permille);
    const uint8_t bytes[2] = {(uint8_t)(scaled & 0xffU), (uint8_t)(scaled >> 8U)};
    for (uint32_t byte_index = 0U; byte_index < 2U; ++byte_index) {
      checksum ^= bytes[byte_index];
      for (uint32_t bit = 0U; bit < 8U; ++bit) {
        const uint32_t mask = 0U - (checksum & 1U);
        checksum = (checksum >> 1U) ^ (0xedb88320U & mask);
      }
    }
  }
  return checksum ^ UINT32_MAX;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

swing_hil_pcm_result swing_hil_pcm_commit(swing_hil_pcm_clip *clip, uint32_t request_id) {
  if (!clip->uploading || clip->committed) {
    invalidate(clip);
    return SWING_HIL_PCM_INVALID_STATE;
  }
  if (request_id != clip->request_id) {
    invalidate(clip);
    return SWING_HIL_PCM_WRONG_REQUEST;
  }
  if (clip->received_bytes != clip->expected_sample_count * 2U) {
    invalidate(clip);
    return SWING_HIL_PCM_INCOMPLETE;
  }
  const uint32_t checksum =
      swing_hil_pcm_crc32((const uint8_t *)clip->samples, clip->received_bytes);
  if (checksum != clip->expected_crc32) {
    invalidate(clip);
    return SWING_HIL_PCM_CHECKSUM;
  }
  clip->uploading = false;
  clip->committed = true;
  return SWING_HIL_PCM_OK;
}

swing_hil_pcm_result swing_hil_pcm_abort(swing_hil_pcm_clip *clip, uint32_t request_id) {
  if (clip->request_id != 0U && request_id != clip->request_id) {
    invalidate(clip);
    return SWING_HIL_PCM_WRONG_REQUEST;
  }
  invalidate(clip);
  return SWING_HIL_PCM_OK;
}

swing_hil_pcm_result swing_hil_pcm_validate_play(const swing_hil_pcm_clip *clip,
                                                 uint32_t marker_sample) {
  if (!clip->committed) {
    return SWING_HIL_PCM_NOT_COMMITTED;
  }
  if (marker_sample >= clip->expected_sample_count) {
    return SWING_HIL_PCM_MARKER;
  }
  return SWING_HIL_PCM_OK;
}

const char *swing_hil_pcm_result_code(swing_hil_pcm_result result) {
  switch (result) {
    case SWING_HIL_PCM_OK:
      return "ok";
    case SWING_HIL_PCM_INVALID_STATE:
      return "pcm_state";
    case SWING_HIL_PCM_WRONG_REQUEST:
      return "pcm_request";
    case SWING_HIL_PCM_OFFSET:
      return "pcm_offset";
    case SWING_HIL_PCM_OVERFLOW:
      return "pcm_overflow";
    case SWING_HIL_PCM_INCOMPLETE:
      return "pcm_incomplete";
    case SWING_HIL_PCM_CHECKSUM:
      return "pcm_checksum";
    case SWING_HIL_PCM_NOT_COMMITTED:
      return "pcm_not_committed";
    case SWING_HIL_PCM_MARKER:
      return "pcm_marker";
  }
  return "pcm_internal";
}
