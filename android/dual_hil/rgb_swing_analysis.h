#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_RGB_SWING_ANALYSIS_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_RGB_SWING_ANALYSIS_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swing_capture::android::dual_hil {

struct RgbFrameTiming {
  std::int64_t media_time_us = 0;
  std::int64_t time_from_impact_us = 0;
};

struct RetainedLuminanceFrame {
  std::size_t frame_index = 0;
  std::vector<std::byte> pixels;
};

struct RgbSwingAnalysis {
  bool detected = false;
  std::string diagnostic;
  std::size_t decoded_frame_count = 0;
  std::size_t representative_frame_index = 0;
  std::size_t peak_frame_index = 0;
  std::size_t first_white_frame_index = 0;
  std::size_t last_white_frame_index = 0;
  std::size_t tile_x = 0;
  std::size_t tile_y = 0;
  double maximum_white_delta = 0.0;
  double white_duration_us = 0.0;
  std::int64_t optical_to_audio_offset_us = 0;
  std::int64_t optical_onset_lower_bound_us = 0;
  std::int64_t optical_onset_upper_bound_us = 0;
  std::vector<std::byte> representative_luminance;
  std::vector<RetainedLuminanceFrame> diagnostic_luminance_frames;
  std::size_t localized_response_tile_count = 0;
  // Diagnostic only: the fixture intentionally emits colored post-impact states for the entire
  // retained post-roll, so their brightness is not an "off" acceptance baseline.
  double post_sequence_baseline_shift = 0.0;
};

struct RgbSwingSequenceConfiguration {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::span<const RgbFrameTiming> timings;
};

// Streaming RGB24 analyzer for the Feather synthetic swing. It retains only
// per-tile statistics plus one representative luminance frame, rather than a
// decoded copy of every high-speed frame.
class RgbSwingSequenceAnalyzer final {
 public:
  explicit RgbSwingSequenceAnalyzer(const RgbSwingSequenceConfiguration &configuration);

  void Append(std::span<const std::uint8_t> rgb24);
  [[nodiscard]] RgbSwingAnalysis Finish();

 private:
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<RgbFrameTiming> timings_;
  std::size_t tile_columns_ = 0;
  std::size_t tile_rows_ = 0;
  std::size_t representative_frame_index_ = 0;
  std::size_t appended_frames_ = 0;
  std::vector<float> tile_whiteness_;
  std::vector<std::byte> representative_luminance_;
  std::vector<RetainedLuminanceFrame> diagnostic_luminance_frames_;
};

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_RGB_SWING_ANALYSIS_H_
