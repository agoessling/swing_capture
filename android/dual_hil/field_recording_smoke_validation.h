#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_SMOKE_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_SMOKE_VALIDATION_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

struct FieldRecordingArtifactProbe {
  int head_status = 0;
  std::map<std::string, std::string, std::less<>> head_headers;
  std::size_t head_body_bytes = 0;
  int range_status = 0;
  std::map<std::string, std::string, std::less<>> range_headers;
  std::string range_body;
};

struct FieldRecordingBundleInspection {
  std::string_view listing_json;
  std::string_view manifest_json;
  std::string_view expected_shared_recording_id;
  std::string_view expected_role;
  FieldRecordingArtifactProbe video;
  FieldRecordingArtifactProbe audio;
};

struct FieldRecordingBundleEvidence {
  std::string recording_id;
  std::string shared_recording_id;
  std::string node_id;
  std::string role;
  std::int64_t duration_us = 0;
  std::uint64_t video_bytes = 0;
  std::uint64_t audio_frames = 0;
  std::uint64_t audio_bytes = 0;
};

// Validates the published manifest, its listing summary, and bounded HTTP retrieval evidence.
// Throws std::runtime_error with a field-specific diagnostic when any claim is inconsistent.
[[nodiscard]] FieldRecordingBundleEvidence ValidateFieldRecordingBundle(
    const FieldRecordingBundleInspection &inspection);

// Validates the coordinated identity and complementary camera roles of two published bundles.
void ValidateFieldRecordingPair(const FieldRecordingBundleEvidence &down_the_line,
                                const FieldRecordingBundleEvidence &face_on,
                                std::string_view expected_shared_recording_id);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_FIELD_RECORDING_SMOKE_VALIDATION_H_
