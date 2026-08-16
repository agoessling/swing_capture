#include "android/dual_hil/rgb_swing_analysis.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using swing_capture::android::dual_hil::RgbFrameTiming;
using swing_capture::android::dual_hil::RgbSwingSequenceAnalyzer;
using swing_capture::android::dual_hil::RgbSwingSequenceConfiguration;

constexpr std::uint32_t kWidth = 80U;
constexpr std::uint32_t kHeight = 48U;
constexpr std::size_t kFrameCount = 49U;

std::vector<RgbFrameTiming> Timings(std::int64_t optical_offset_us = 0) {
  std::vector<RgbFrameTiming> timings;
  timings.reserve(kFrameCount);
  for (std::size_t index = 0; index < kFrameCount; ++index) {
    const std::int64_t media_time = static_cast<std::int64_t>(index) * 4167;
    timings.push_back({
        .media_time_us = media_time,
        .time_from_impact_us = media_time - 100000 - optical_offset_us,
    });
  }
  return timings;
}

std::vector<std::uint8_t> Frame(bool white_led, bool global_flash = false,
                                bool bright_post_color = false) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 3U,
                                   global_flash ? 120U : 40U);
  for (std::uint32_t y = 16U; y < 24U; ++y) {
    for (std::uint32_t x = 48U; x < 56U; ++x) {
      const std::size_t pixel = (static_cast<std::size_t>(y) * kWidth + x) * 3U;
      pixels[pixel] = 120U;
      pixels[pixel + 1U] = white_led ? 120U : (bright_post_color ? 90U : 10U);
      pixels[pixel + 2U] = white_led ? 120U : (bright_post_color ? 90U : 10U);
    }
  }
  return pixels;
}

void NominalWhiteImpactPasses() {
  const auto timings = Timings();
  RgbSwingSequenceAnalyzer analyzer(
      RgbSwingSequenceConfiguration{.width = kWidth, .height = kHeight, .timings = timings});
  for (const auto &timing : timings) {
    const bool white = timing.time_from_impact_us >= 0 && timing.time_from_impact_us < 20000;
    analyzer.Append(Frame(white));
  }
  const auto result = analyzer.Finish();
  assert(result.detected);
  assert(result.first_white_frame_index == 24U);
  assert(result.last_white_frame_index == 28U);
  assert(result.maximum_white_delta > 100.0);
  assert(result.optical_to_audio_offset_us == 8L);
  assert(result.optical_onset_lower_bound_us == -4159L);
  assert(result.optical_onset_upper_bound_us == 4175L);
  assert(result.representative_luminance.size() == static_cast<std::size_t>(kWidth) * kHeight);
  assert(result.diagnostic_luminance_frames.size() > 40U);
  assert(result.localized_response_tile_count == 1U);
  assert(result.post_sequence_baseline_shift < 1.0);
}

void ColoredPostSequenceIsNotTreatedAsAnOffBaseline() {
  const auto timings = Timings();
  RgbSwingSequenceAnalyzer analyzer(
      RgbSwingSequenceConfiguration{.width = kWidth, .height = kHeight, .timings = timings});
  for (const auto &timing : timings) {
    const bool white = timing.time_from_impact_us >= 0 && timing.time_from_impact_us < 20000;
    const bool bright_post =
        timing.time_from_impact_us >= 45000 && timing.time_from_impact_us <= 100000;
    analyzer.Append(Frame(white, false, bright_post));
  }
  const auto result = analyzer.Finish();
  assert(result.detected);
  assert(result.post_sequence_baseline_shift > 50.0);
}

void GlobalFlashIsNotAStableLocalizedLed() {
  const auto timings = Timings();
  RgbSwingSequenceAnalyzer analyzer(
      RgbSwingSequenceConfiguration{.width = kWidth, .height = kHeight, .timings = timings});
  for (const auto &timing : timings) {
    const bool flash = timing.time_from_impact_us >= 0 && timing.time_from_impact_us < 20000;
    analyzer.Append(Frame(false, flash));
  }
  const auto result = analyzer.Finish();
  assert(!result.detected);
  assert(result.localized_response_tile_count > 12U);
}

void MissingAndLateImpactFail() {
  const auto timings = Timings();
  RgbSwingSequenceAnalyzer missing(
      RgbSwingSequenceConfiguration{.width = kWidth, .height = kHeight, .timings = timings});
  for (std::size_t index = 0; index < timings.size(); ++index) {
    missing.Append(Frame(false));
  }
  assert(!missing.Finish().detected);

  const auto late_timings = Timings(25000);
  RgbSwingSequenceAnalyzer late(
      RgbSwingSequenceConfiguration{.width = kWidth, .height = kHeight, .timings = late_timings});
  for (const auto &timing : late_timings) {
    const bool white = timing.time_from_impact_us >= 25000 && timing.time_from_impact_us < 45000;
    late.Append(Frame(white));
  }
  assert(!late.Finish().detected);
}

void IncompleteDecodeFails() {
  const auto timings = Timings();
  RgbSwingSequenceAnalyzer analyzer(
      RgbSwingSequenceConfiguration{.width = kWidth, .height = kHeight, .timings = timings});
  analyzer.Append(Frame(false));
  try {
    static_cast<void>(analyzer.Finish());
  } catch (const std::runtime_error &) {
    return;
  }
  assert(false);
}

}  // namespace

int main() {
  NominalWhiteImpactPasses();
  ColoredPostSequenceIsNotTreatedAsAnOffBaseline();
  MissingAndLateImpactFail();
  GlobalFlashIsNotAStableLocalizedLed();
  IncompleteDecodeFails();
}
