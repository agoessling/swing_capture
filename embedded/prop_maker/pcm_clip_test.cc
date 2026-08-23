#include "embedded/prop_maker/pcm_clip.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>

namespace {

constexpr std::array<std::uint8_t, 8> kSamples = {0x00, 0x00, 0xff, 0x7f, 0x00, 0x80, 0x34, 0x12};

void Append(swing_hil_pcm_clip *clip, std::uint32_t request_id, std::uint32_t offset,
            std::span<const std::uint8_t> bytes) {
  assert(swing_hil_pcm_append(clip, request_id, offset, bytes.data(), bytes.size()) ==
         SWING_HIL_PCM_OK);
}

void AcceptsOnlyACompleteChecksummedUpload() {
  swing_hil_pcm_clip clip{};
  swing_hil_pcm_reset(&clip);
  const std::uint32_t checksum = swing_hil_pcm_crc32(kSamples.data(), kSamples.size());
  assert(checksum == 0x53c29a84U);
  assert(swing_hil_pcm_begin(&clip, 7U, 4U, checksum) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_validate_play(&clip, 0U) == SWING_HIL_PCM_NOT_COMMITTED);
  Append(&clip, 7U, 0U, std::span(kSamples).first<3>());
  Append(&clip, 7U, 3U, std::span(kSamples).subspan<3>());
  assert(swing_hil_pcm_commit(&clip, 7U) == SWING_HIL_PCM_OK);
  assert(clip.committed);
  assert(!clip.uploading);
  assert(clip.expected_sample_count == 4U);
  assert(clip.expected_crc32 == checksum);
  assert(clip.samples[0] == 0);
  assert(clip.samples[1] == 32767);
  assert(clip.samples[2] == -32768);
  assert(clip.samples[3] == 0x1234);
  assert(swing_hil_pcm_validate_play(&clip, 0U) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_validate_play(&clip, 3U) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_validate_play(&clip, 4U) == SWING_HIL_PCM_MARKER);
}

void FailureInvalidatesThePartialOrCommittedClip() {
  const std::uint32_t checksum = swing_hil_pcm_crc32(kSamples.data(), kSamples.size());

  swing_hil_pcm_clip clip{};
  assert(swing_hil_pcm_begin(&clip, 10U, 4U, checksum) == SWING_HIL_PCM_OK);
  Append(&clip, 10U, 0U, std::span(kSamples).first<2>());
  assert(swing_hil_pcm_append(&clip, 10U, 0U, kSamples.data(), 2U) == SWING_HIL_PCM_OFFSET);
  assert(!clip.uploading && !clip.committed);

  assert(swing_hil_pcm_begin(&clip, 11U, 4U, checksum) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_append(&clip, 12U, 0U, kSamples.data(), 2U) == SWING_HIL_PCM_WRONG_REQUEST);
  assert(!clip.uploading);

  assert(swing_hil_pcm_begin(&clip, 13U, 4U, checksum) == SWING_HIL_PCM_OK);
  Append(&clip, 13U, 0U, std::span(kSamples).first<2>());
  assert(swing_hil_pcm_commit(&clip, 13U) == SWING_HIL_PCM_INCOMPLETE);
  assert(!clip.uploading);

  assert(swing_hil_pcm_begin(&clip, 14U, 4U, checksum + 1U) == SWING_HIL_PCM_OK);
  Append(&clip, 14U, 0U, kSamples);
  assert(swing_hil_pcm_commit(&clip, 14U) == SWING_HIL_PCM_CHECKSUM);
  assert(!clip.committed);

  assert(swing_hil_pcm_begin(&clip, 15U, 4U, checksum) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_append(&clip, 15U, 0U, kSamples.data(), 9U) == SWING_HIL_PCM_OVERFLOW);
  assert(!clip.uploading);
}

void AbortIsIdempotentForAnInactiveUploadAndInvalidatesCommittedData() {
  const std::uint32_t checksum = swing_hil_pcm_crc32(kSamples.data(), kSamples.size());
  swing_hil_pcm_clip clip{};
  assert(swing_hil_pcm_abort(&clip, 1U) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_begin(&clip, 20U, 4U, checksum) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_abort(&clip, 21U) == SWING_HIL_PCM_WRONG_REQUEST);
  assert(!clip.uploading && !clip.committed);
  assert(swing_hil_pcm_begin(&clip, 22U, 4U, checksum) == SWING_HIL_PCM_OK);
  Append(&clip, 22U, 0U, kSamples);
  assert(swing_hil_pcm_commit(&clip, 22U) == SWING_HIL_PCM_OK);
  assert(swing_hil_pcm_abort(&clip, 22U) == SWING_HIL_PCM_OK);
  assert(!clip.uploading && !clip.committed);
}

void RejectsZeroAndOverflowSampleCounts() {
  swing_hil_pcm_clip clip{};
  assert(swing_hil_pcm_begin(&clip, 1U, 0U, 0U) == SWING_HIL_PCM_OVERFLOW);
  assert(swing_hil_pcm_begin(&clip, 1U, SWING_HIL_PCM_MAX_SAMPLES + 1U, 0U) ==
         SWING_HIL_PCM_OVERFLOW);
}

void ComputesPlayedCrcFromGainScaledLittleEndianSamples() {
  constexpr std::array<std::int16_t, 4> samples = {0, 32767, -32768, 0x1234};
  assert(swing_hil_pcm_scale_sample(samples[0], 125U) == 0);
  assert(swing_hil_pcm_scale_sample(samples[1], 125U) == 4095);
  assert(swing_hil_pcm_scale_sample(samples[2], 125U) == -4096);
  assert(swing_hil_pcm_scale_sample(samples[3], 125U) == 582);
  assert(swing_hil_pcm_scaled_crc32(samples.data(), samples.size(), 125U) == 0xfe5f190bU);
}

}  // namespace

int main() {
  AcceptsOnlyACompleteChecksummedUpload();
  FailureInvalidatesThePartialOrCommittedClip();
  AbortIsIdempotentForAnInactiveUploadAndInvalidatesCommittedData();
  RejectsZeroAndOverflowSampleCounts();
  ComputesPlayedCrcFromGainScaledLittleEndianSamples();
  return 0;
}
