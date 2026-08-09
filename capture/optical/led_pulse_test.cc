#include "capture/optical/led_pulse.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using swing_capture::optical::AnalyzeLedPulse;
using swing_capture::optical::BayerRg8FrameView;
using swing_capture::optical::LedPulseOptions;
using swing_capture::optical::PixelRegion;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;
constexpr std::uint64_t kTicksPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kFrameIntervalTicks = 4'405'286ULL;

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void ExpectNear(double actual, double expected, double tolerance, const std::string &message) {
  Expect(std::abs(actual - expected) <= tolerance,
         message + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}

struct OwnedSequence {
  std::vector<std::vector<std::byte>> pixels;
  std::vector<BayerRg8FrameView> frames;
};

enum class GlobalFlicker {
  kAdditive,
  kMultiplicative,
};

OwnedSequence MakeSequence(std::uint8_t baseline, std::size_t pulse_start,
                           std::size_t pulse_end_exclusive, bool gap_in_pulse = false,
                           int pulse_delta = 75) {
  constexpr std::size_t kFrameCount = 22;
  OwnedSequence sequence;
  sequence.pixels.resize(kFrameCount);
  sequence.frames.reserve(kFrameCount);
  for (std::size_t frame_index = 0; frame_index < kFrameCount; ++frame_index) {
    const int noise = static_cast<int>(frame_index % 3U) - 1;
    const int noisy_baseline = std::clamp(static_cast<int>(baseline) + noise, 0, 255);
    auto &pixels = sequence.pixels[frame_index];
    pixels.assign(static_cast<std::size_t>(kWidth) * kHeight,
                  std::byte{static_cast<std::uint8_t>(noisy_baseline)});
    const bool pulse_on = frame_index >= pulse_start && frame_index < pulse_end_exclusive &&
                          !(gap_in_pulse && frame_index == pulse_start + 4U);
    if (pulse_on) {
      for (std::uint32_t y = 16; y < 28; ++y) {
        for (std::uint32_t x = 28; x < 40; ++x) {
          if ((x & 1U) == 0U && (y & 1U) == 0U) {
            const int illuminated = std::min(noisy_baseline + pulse_delta, 255);
            pixels[static_cast<std::size_t>(y) * kWidth + x] =
                std::byte{static_cast<std::uint8_t>(illuminated)};
          }
        }
      }
    }
    sequence.frames.push_back({
        .image =
            {
                .pixels = pixels,
                .width = kWidth,
                .height = kHeight,
            },
        .frame_id = 100U + frame_index,
        .device_timestamp = 2'000'000'000ULL + frame_index * kFrameIntervalTicks,
    });
  }
  return sequence;
}

OwnedSequence MakeGlobalFlickerSequence(GlobalFlicker flicker, bool include_dim_pulse) {
  constexpr std::size_t kFrameCount = 22;
  constexpr std::size_t kPulseStart = 8;
  constexpr std::size_t kPulseEnd = 18;
  constexpr int kPulseDelta = 10;
  constexpr std::array<int, kFrameCount> kAdditiveOffsets = {
      0, 0, 0, 0, 0, 18, 31, 12, 27, 8, 35, 16, 30, 10, 26, 14, 33, 9, 22, 31, 11, 28,
  };
  constexpr std::array<double, kFrameCount> kMultiplicativeScales = {
      1.00, 1.00, 1.00, 1.00, 1.00, 1.13, 1.29, 1.08, 1.24, 1.06, 1.31,
      1.15, 1.27, 1.09, 1.22, 1.12, 1.30, 1.07, 1.19, 1.28, 1.10, 1.25,
  };

  OwnedSequence sequence;
  sequence.pixels.resize(kFrameCount);
  sequence.frames.reserve(kFrameCount);
  for (std::size_t frame_index = 0; frame_index < kFrameCount; ++frame_index) {
    auto &pixels = sequence.pixels[frame_index];
    pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        const int spatial_baseline = 24 + static_cast<int>((7U * x + 11U * y) % 96U);
        const int temporal_noise =
            static_cast<int>((x + 2U * y + static_cast<std::uint32_t>(frame_index)) % 3U) - 1;
        const double transformed =
            flicker == GlobalFlicker::kAdditive
                ? static_cast<double>(spatial_baseline + kAdditiveOffsets[frame_index])
                : kMultiplicativeScales[frame_index] * spatial_baseline;
        const bool pulse_on = include_dim_pulse && frame_index >= kPulseStart &&
                              frame_index < kPulseEnd && x >= 28U && x < 40U && y >= 16U &&
                              y < 28U && (x & 1U) == 0U && (y & 1U) == 0U;
        const int sample = std::clamp(static_cast<int>(std::lround(transformed)) + temporal_noise +
                                          (pulse_on ? kPulseDelta : 0),
                                      0, 255);
        pixels[static_cast<std::size_t>(y) * kWidth + x] =
            std::byte{static_cast<std::uint8_t>(sample)};
      }
    }
    sequence.frames.push_back({
        .image =
            {
                .pixels = pixels,
                .width = kWidth,
                .height = kHeight,
            },
        .frame_id = 100U + frame_index,
        .device_timestamp = 2'000'000'000ULL + frame_index * kFrameIntervalTicks,
    });
  }
  return sequence;
}

LedPulseOptions TestOptions() {
  LedPulseOptions options;
  options.baseline_frame_count = 5;
  options.region_stride = 4;
  options.minimum_red_sample_delta = 24;
  options.minimum_changed_red_samples = 6;
  options.minimum_changed_red_fraction = 0.08;
  options.minimum_mean_positive_red_delta = 6.0;
  options.minimum_active_frames = 8;
  options.maximum_pulse_span_frames = 12;
  return options;
}

LedPulseOptions DimPulseOptions() {
  LedPulseOptions options = TestOptions();
  options.minimum_red_sample_delta = 3;
  options.minimum_changed_red_samples = 3;
  options.minimum_changed_red_fraction = 0.04;
  options.minimum_mean_positive_red_delta = 4.0;
  return options;
}

bool RegionIntersectsLed(const swing_capture::optical::PixelRegion &region) {
  return region.x < 40U && region.x + region.width > 28U && region.y < 28U &&
         region.y + region.height > 16U;
}

void IlluminateLedRedSamples(OwnedSequence *sequence, std::size_t frame_index,
                             std::size_t sample_count, int delta) {
  std::size_t illuminated = 0;
  for (std::uint32_t y = 16; y < 28 && illuminated < sample_count; y += 2U) {
    for (std::uint32_t x = 28; x < 40 && illuminated < sample_count; x += 2U) {
      auto &sample = sequence->pixels[frame_index][static_cast<std::size_t>(y) * kWidth + x];
      const int baseline = std::to_integer<std::uint8_t>(sample);
      sample = std::byte{static_cast<std::uint8_t>(std::min(255, baseline + delta))};
      ++illuminated;
    }
  }
  Expect(illuminated == sample_count, "synthetic LED sample count fits the test region");
}

LedPulseOptions MatchedPulseOptions() {
  LedPulseOptions options = TestOptions();
  options.locked_region = PixelRegion{.x = 24, .y = 12, .width = 16, .height = 16};
  options.expected_start_frame_index = 8;
  options.minimum_changed_red_fraction = 0.10;
  options.minimum_support_changed_red_fraction = 0.10;
  options.minimum_support_mean_positive_red_delta = 2.0;
  options.minimum_matched_contiguous_supported_frames = 9;
  options.minimum_matched_active_frames = 3;
  return options;
}

OwnedSequence MakeSupportedPulse(std::size_t start, std::size_t end_exclusive,
                                 std::span<const std::size_t> high_confidence_frames,
                                 std::optional<std::size_t> unsupported_frame = std::nullopt) {
  auto sequence = MakeSequence(60, 0, 0);
  for (std::size_t frame_index = start; frame_index < end_exclusive; ++frame_index) {
    if (unsupported_frame == frame_index) {
      continue;
    }
    const bool high_confidence =
        std::ranges::find(high_confidence_frames, frame_index) != high_confidence_frames.end();
    IlluminateLedRedSamples(&sequence, frame_index, high_confidence ? 8U : 7U,
                            high_confidence ? 60 : 30);
  }
  return sequence;
}

void DetectsDimTenFramePulseAboveMeasuredDarkNoise() {
  const auto sequence = MakeSequence(1, 8, 18, false, 8);
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, DimPulseOptions());

  Expect(result.detected, "dim ten-frame LED pulse detected above dark-frame noise");
  Expect(result.pulse_active_frame_count == 10, "dim pulse active frame count");
  Expect(result.pulse_span_frame_count == 10, "dim pulse span frame count");
}

void DetectsDimPulseThroughGlobalFlicker(GlobalFlicker flicker, const std::string &name) {
  const auto sequence = MakeGlobalFlickerSequence(flicker, true);
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, DimPulseOptions());

  Expect(result.detected, "dim pulse detected through " + name + " global flicker");
  Expect(result.pulse_start_frame_index == 8, name + " pulse start frame index");
  Expect(result.pulse_end_frame_index == 17, name + " pulse end frame index");
  Expect(result.pulse_active_frame_count == 10, name + " pulse active frame count");
  Expect(RegionIntersectsLed(result.selected_region), name + " selected region intersects LED");
  Expect(result.frames[8].local_mean_positive_red_delta >
             result.frames[8].background_mean_positive_red_delta,
         name + " diagnostic separates local response from background");
  Expect(result.frames[8].mean_positive_red_delta > 4.0,
         name + " normalized pulse response clears the dim threshold");
}

void RejectsGlobalFlickerWithoutLocalizedPulse(GlobalFlicker flicker, const std::string &name) {
  const auto sequence = MakeGlobalFlickerSequence(flicker, false);
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, DimPulseOptions());

  Expect(!result.detected, name + " global flicker alone is rejected");
  Expect(
      std::ranges::none_of(result.frames, [](const auto &diagnostic) { return diagnostic.active; }),
      name + " global flicker does not produce an active local region");
}

void CorrectsAdditiveAndMultiplicativeGlobalFlicker() {
  DetectsDimPulseThroughGlobalFlicker(GlobalFlicker::kAdditive, "additive");
  RejectsGlobalFlickerWithoutLocalizedPulse(GlobalFlicker::kAdditive, "additive");
  DetectsDimPulseThroughGlobalFlicker(GlobalFlicker::kMultiplicative, "multiplicative");
  RejectsGlobalFlickerWithoutLocalizedPulse(GlobalFlicker::kMultiplicative, "multiplicative");
}

void DetectsTenFramePulseAcrossDifferentCameraBaselines() {
  for (const std::uint8_t baseline : {std::uint8_t{20}, std::uint8_t{155}}) {
    const auto sequence = MakeSequence(baseline, 8, 18);
    const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, TestOptions());

    Expect(result.detected, "ten-frame pulse detected at baseline " + std::to_string(baseline));
    Expect(RegionIntersectsLed(result.selected_region), "selected region intersects LED");
    Expect(result.pulse_start_frame_index == 8, "pulse start frame index");
    Expect(result.pulse_end_frame_index == 17, "pulse end frame index");
    Expect(result.pulse_active_frame_count == 10, "pulse active frame count");
    Expect(result.pulse_span_frame_count == 10, "pulse span frame count");
    Expect(result.nominal_frame_interval_ticks == kFrameIntervalTicks, "nominal device interval");
    Expect(result.duration_ticks == 10U * kFrameIntervalTicks, "device-local duration ticks");
    ExpectNear(result.duration_seconds,
               static_cast<double>(10U * kFrameIntervalTicks) / kTicksPerSecond, 1e-12,
               "device-local duration seconds");
    Expect(result.frames.size() == sequence.frames.size(), "per-frame diagnostics retained");
    Expect(result.frames[4].baseline_frame && !result.frames[4].active,
           "baseline diagnostic is never active");
    Expect(result.frames[8].active && result.frames[17].active, "pulse diagnostics are active");
    Expect(result.diagnostic.find("device-local duration") != std::string::npos,
           "success diagnostic describes clock domain");
  }
}

void BridgesOneThresholdDropWithoutChangingTheTimedSpan() {
  const auto sequence = MakeSequence(60, 8, 18, true);
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, TestOptions());

  Expect(result.detected, "one internal inactive frame is tolerated");
  Expect(result.pulse_active_frame_count == 9, "active count excludes threshold drop");
  Expect(result.pulse_span_frame_count == 10, "pulse span includes threshold drop");
  Expect(result.duration_ticks == 10U * kFrameIntervalTicks, "gap does not shorten pulse timing");
}

void MatchesTenSupportedFramesWithThreeHighConfidenceAnchors() {
  constexpr std::array<std::size_t, 3> kHighConfidenceFrames = {8, 12, 17};
  const auto sequence = MakeSupportedPulse(8, 18, kHighConfidenceFrames);
  const LedPulseOptions options = MatchedPulseOptions();
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, options);

  Expect(result.detected, "continuous supported pulse with three high-confidence anchors passes");
  Expect(result.matched_window_used, "scheduled pulse records matched-window method");
  Expect(result.selected_region.x == 24 && result.selected_region.y == 12,
         "qualification remains locked to locator region");
  Expect(result.pulse_start_frame_index == 8 && result.pulse_end_frame_index == 17,
         "matched pulse selects the exact supported interval");
  Expect(result.matched_supported_frame_count == 10,
         "matched pulse reports all ten supported frames");
  Expect(result.matched_longest_contiguous_supported_frame_count == 10,
         "matched pulse reports ten contiguous supported frames");
  Expect(result.matched_high_confidence_frame_count == 3,
         "matched pulse reports exactly three high-confidence anchors");
  Expect(result.pulse_active_frame_count == result.matched_high_confidence_frame_count,
         "legacy active count remains the high-confidence count");
  for (std::size_t frame_index = 8; frame_index < 18; ++frame_index) {
    Expect(result.frames[frame_index].supported,
           "every scheduled pulse frame exposes per-frame support");
  }
  Expect(!result.frames[7].supported && !result.frames[18].supported,
         "frames surrounding the scheduled pulse are unsupported");
  Expect(std::ranges::count_if(result.frames, [](const auto &frame) { return frame.active; }) == 3,
         "only the high-confidence anchors retain strict active evidence");
  Expect(result.matched_mean_positive_red_delta >= options.minimum_matched_mean_positive_red_delta,
         "matched pulse clears integrated red response threshold");
}

void AcceptsNineContiguousSupportedFramesWithPartialEdges() {
  constexpr std::array<std::size_t, 3> kHighConfidenceFrames = {9, 13, 17};
  auto sequence = MakeSequence(60, 0, 0);
  for (std::size_t frame_index = 9; frame_index < 18; ++frame_index) {
    const bool high_confidence =
        std::ranges::find(kHighConfidenceFrames, frame_index) != kHighConfidenceFrames.end();
    IlluminateLedRedSamples(&sequence, frame_index, 16, high_confidence ? 40 : 25);
  }
  LedPulseOptions options = MatchedPulseOptions();
  options.expected_start_frame_index = 8;
  options.expected_start_tolerance_frames = 0;
  options.minimum_matched_pulse_frames = 11;
  options.maximum_matched_pulse_frames = 11;
  options.minimum_mean_positive_red_delta = 8.0;
  options.minimum_matched_mean_positive_red_delta = 3.0;
  options.minimum_matched_changed_red_fraction = 0.20;
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, options);

  Expect(result.detected,
         "nine supported frames pass with two partial exposure edges: " + result.diagnostic);
  Expect(result.pulse_start_frame_index == 8 && result.pulse_end_frame_index == 18,
         "partial-edge fixture retains the scheduled eleven-frame window");
  Expect(result.matched_supported_frame_count == 9,
         "partial-edge fixture reports nine supported frames");
  Expect(result.matched_longest_contiguous_supported_frame_count == 9,
         "partial-edge fixture reports nine contiguous supported frames");
  Expect(result.matched_high_confidence_frame_count == 3,
         "partial-edge fixture retains three high-confidence anchors");
  Expect(!result.frames[8].supported && !result.frames[18].supported,
         "leading and trailing partial exposure frames may remain unsupported");
}

void RejectsDiscontinuousMatchedSupport() {
  constexpr std::array<std::size_t, 3> kHighConfidenceFrames = {8, 12, 17};
  const auto sequence = MakeSupportedPulse(8, 19, kHighConfidenceFrames, 13);
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, MatchedPulseOptions());

  Expect(!result.detected, "ten noncontiguous supported frames do not qualify as a pulse");
  Expect(result.matched_supported_frame_count == 10,
         "discontinuous failure retains its total supported-frame count");
  Expect(result.matched_longest_contiguous_supported_frame_count == 5,
         "discontinuous failure exposes its longest supported run");
  Expect(result.matched_high_confidence_frame_count == 3,
         "discontinuous failure still records its high-confidence anchors");
}

void RejectsInsufficientContiguousMatchedSupport() {
  constexpr std::array<std::size_t, 3> kHighConfidenceFrames = {8, 12, 15};
  const auto sequence = MakeSupportedPulse(8, 16, kHighConfidenceFrames);
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, MatchedPulseOptions());

  Expect(!result.detected, "eight contiguous supported frames are insufficient");
  Expect(result.matched_supported_frame_count == 8,
         "insufficient-support failure retains its supported-frame count");
  Expect(result.matched_longest_contiguous_supported_frame_count == 8,
         "insufficient-support failure exposes its longest supported run");
  Expect(result.matched_high_confidence_frame_count == 3,
         "insufficient-support failure clears anchors but not continuity");
}

void RejectsAnIsolatedTransientInTheMatchedWindowAndRetainsItsPeak() {
  auto sequence = MakeSequence(60, 0, 0);
  for (std::uint32_t y = 16; y < 28; ++y) {
    for (std::uint32_t x = 28; x < 40; ++x) {
      if ((x & 1U) == 0U && (y & 1U) == 0U) {
        sequence.pixels[10][static_cast<std::size_t>(y) * kWidth + x] = std::byte{240};
      }
    }
  }
  const LedPulseOptions options = MatchedPulseOptions();
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, options);

  Expect(!result.detected, "one-frame transient cannot satisfy scheduled matched pulse");
  Expect(result.matched_window_used, "rejected scheduled pulse retains matched diagnostics");
  Expect(result.pulse_active_frame_count == 1,
         "rejected matched pulse reports its strict active-frame count");
  Expect(result.strongest_response_frame_index == 10,
         "rejected pulse retains the actual strongest response frame");
}

void RejectsNoiseAndOnePixelTransient() {
  auto sequence = MakeSequence(80, 0, 0);
  sequence.pixels[10][static_cast<std::size_t>(20) * kWidth + 20] = std::byte{255};
  const auto result = AnalyzeLedPulse(sequence.frames, kTicksPerSecond, TestOptions());

  Expect(!result.detected, "single red-site transient is not an LED pulse");
  Expect(result.diagnostic.find("no short LED pulse") != std::string::npos,
         "failure has an explicit diagnostic");
  Expect(result.diagnostic.find("no frame crossed") != std::string::npos,
         "failure diagnostic identifies threshold outcome");
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, message);
}

void RejectsMalformedTimingAndPayload() {
  auto sequence = MakeSequence(40, 8, 18);
  sequence.frames[7].device_timestamp = sequence.frames[6].device_timestamp;
  ExpectInvalidArgument(
      [&] { (void)AnalyzeLedPulse(sequence.frames, kTicksPerSecond, TestOptions()); },
      "nonmonotonic timestamp rejected");

  sequence = MakeSequence(40, 8, 18);
  sequence.frames[3].image.pixels = std::span<const std::byte>(sequence.pixels[3]).first(10);
  ExpectInvalidArgument(
      [&] { (void)AnalyzeLedPulse(sequence.frames, kTicksPerSecond, TestOptions()); },
      "short payload rejected");

  sequence = MakeSequence(40, 8, 18);
  ExpectInvalidArgument([&] { (void)AnalyzeLedPulse(sequence.frames, 0, TestOptions()); },
                        "zero clock frequency rejected");

  sequence = MakeSequence(40, 8, 18);
  auto invalid_support_fraction = TestOptions();
  invalid_support_fraction.minimum_support_changed_red_fraction =
      invalid_support_fraction.minimum_changed_red_fraction + 0.01;
  ExpectInvalidArgument(
      [&] { (void)AnalyzeLedPulse(sequence.frames, kTicksPerSecond, invalid_support_fraction); },
      "support fraction cannot exceed the high-confidence fraction");

  auto invalid_support_mean = TestOptions();
  invalid_support_mean.minimum_support_mean_positive_red_delta =
      invalid_support_mean.minimum_mean_positive_red_delta + 0.1;
  ExpectInvalidArgument(
      [&] { (void)AnalyzeLedPulse(sequence.frames, kTicksPerSecond, invalid_support_mean); },
      "support mean cannot exceed the high-confidence mean");
}

}  // namespace

int main() {
  DetectsTenFramePulseAcrossDifferentCameraBaselines();
  DetectsDimTenFramePulseAboveMeasuredDarkNoise();
  CorrectsAdditiveAndMultiplicativeGlobalFlicker();
  BridgesOneThresholdDropWithoutChangingTheTimedSpan();
  MatchesTenSupportedFramesWithThreeHighConfidenceAnchors();
  AcceptsNineContiguousSupportedFramesWithPartialEdges();
  RejectsDiscontinuousMatchedSupport();
  RejectsInsufficientContiguousMatchedSupport();
  RejectsAnIsolatedTransientInTheMatchedWindowAndRetainsItsPeak();
  RejectsNoiseAndOnePixelTransient();
  RejectsMalformedTimingAndPayload();
  return failures == 0 ? 0 : 1;
}
