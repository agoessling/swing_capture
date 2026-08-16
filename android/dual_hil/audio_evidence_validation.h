#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_AUDIO_EVIDENCE_VALIDATION_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_AUDIO_EVIDENCE_VALIDATION_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "capture/audio/commanded_tone_analyzer.h"

namespace swing_capture::android::dual_hil {

inline constexpr std::size_t kRetainedAudioEvidenceWavBytes = 192046U;
inline constexpr std::uint32_t kRetainedAudioEvidenceSampleRateHz = 48000U;
inline constexpr std::uint64_t kRetainedAudioEvidenceSampleCount = 96001U;
inline constexpr std::uint64_t kRetainedAudioEvidenceStrikeSampleIndex = 72000U;

struct RetainedAudioEvidenceInspection {
  std::string_view report;
  std::string_view manifest;
  std::string_view wav;
  std::uint64_t feather_accepted_device_microseconds = 0;
  std::uint64_t feather_impact_scheduled_device_microseconds = 0;
};

struct RetainedAudioEvidence {
  std::uint64_t first_frame_position = 0;
  std::uint64_t last_frame_position = 0;
  std::uint64_t strike_frame_position = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t strike_sample_index = 0;
  std::uint64_t feather_command_to_impact_microseconds = 0;
  std::uint64_t inferred_command_sample_offset = 0;
  std::uint64_t guarded_background_required_samples = 0;
  std::uint64_t required_end_sample_offset = 0;
  CommandedToneStimulus stimulus;
  CommandedToneThresholds thresholds;
  CommandedToneEvaluation evaluation;
};

// Validates the exact app report/manifest/WAV contract, derives the Feather
// command boundary in the retained sample domain, and evaluates the commanded
// 2 kHz tone. Structural violations throw; signal rejection is represented by
// evaluation.passed == false so its diagnostics can be published by the HIL.
[[nodiscard]] RetainedAudioEvidence AnalyzeRetainedAudioEvidence(
    const RetainedAudioEvidenceInspection &inspection);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_AUDIO_EVIDENCE_VALIDATION_H_
