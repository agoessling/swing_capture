#include "capture/optical/rgb_swing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture/image/image_quality.h"
#include "capture/optical/led_pulse.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using swing_capture::optical::AnalyzeRgbBrightnessCalibration;
using swing_capture::optical::AnalyzeRgbSwing;
using swing_capture::optical::AnalyzeRgbWhiteImpact;
using swing_capture::optical::BayerRg8FrameView;
using swing_capture::optical::BrightnessCandidateEvidence;
using swing_capture::optical::CameraBrightnessSweep;
using swing_capture::optical::FeatherRgbStep;
using swing_capture::optical::PixelRegion;
using swing_capture::optical::RecommendSharedNonSaturatingRgbBrightness;
using swing_capture::optical::RecommendSharedRgbBrightness;
using swing_capture::optical::Rgb8;
using swing_capture::optical::RgbBrightnessCalibrationInput;
using swing_capture::optical::RgbBrightnessCalibrationOptions;
using swing_capture::optical::RgbFramePhase;
using swing_capture::optical::RgbStateEvidence;
using swing_capture::optical::RgbSwingAnalysis;
using swing_capture::optical::RgbSwingAnalysisOptions;
using swing_capture::optical::RgbSwingCameraInput;
using swing_capture::optical::RgbSwingPhase;
using swing_capture::optical::rgb_swing_internal::FindConnectedOffsetScoreComponent;

constexpr std::uint32_t kWidth = 48;
constexpr std::uint32_t kHeight = 40;
constexpr PixelRegion kLedRegion = {.x = 18, .y = 14, .width = 8, .height = 8};
constexpr PixelRegion kCalibrationLedRegion = {.x = 27, .y = 15, .width = 6, .height = 6};
constexpr auto kFramePeriod = 4ms;
const Clock::time_point kOrigin = Clock::time_point(100s);

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  try {
    std::invoke(std::forward<Function>(function));
    Expect(false, message + ": no exception");
  } catch (const std::invalid_argument &) {
  } catch (const std::exception &error) {
    Expect(false, message + ": wrong exception: " + error.what());
  }
}

struct OwnedCamera {
  std::vector<std::vector<std::byte>> payloads;
  std::vector<BayerRg8FrameView> frames;
  std::vector<Clock::time_point> mapped_host_times;
};

void AddStep(std::vector<FeatherRgbStep> *schedule, Clock::duration begin, Clock::duration end,
             Rgb8 color, std::uint8_t brightness, RgbSwingPhase phase) {
  schedule->push_back({
      .start = kOrigin + begin,
      .end_exclusive = kOrigin + end,
      .color = color,
      .brightness = brightness,
      .phase = phase,
  });
}

std::vector<FeatherRgbStep> MakeSchedule() {
  const Rgb8 black{};
  const Rgb8 white{.red = 255, .green = 255, .blue = 255};
  std::vector<FeatherRgbStep> schedule;
  AddStep(&schedule, 0ms, 24ms, black, 0, RgbSwingPhase::kBaseline);
  AddStep(&schedule, 24ms, 44ms, white, 12, RgbSwingPhase::kBrightnessProbe);
  AddStep(&schedule, 44ms, 52ms, black, 0, RgbSwingPhase::kBaseline);
  AddStep(&schedule, 52ms, 72ms, white, 48, RgbSwingPhase::kBrightnessProbe);
  AddStep(&schedule, 72ms, 80ms, black, 0, RgbSwingPhase::kBaseline);
  AddStep(&schedule, 80ms, 100ms, white, 120, RgbSwingPhase::kBrightnessProbe);
  AddStep(&schedule, 100ms, 120ms, {.red = 255}, 48, RgbSwingPhase::kPreImpact);
  AddStep(&schedule, 120ms, 140ms, {.green = 255}, 48, RgbSwingPhase::kPreImpact);
  AddStep(&schedule, 140ms, 160ms, {.blue = 255}, 48, RgbSwingPhase::kPreImpact);
  AddStep(&schedule, 160ms, 173200us, white, 48, RgbSwingPhase::kImpact);
  AddStep(&schedule, 173200us, 193200us, {.green = 255, .blue = 255}, 48,
          RgbSwingPhase::kPostImpact);
  AddStep(&schedule, 193200us, 213200us, {.red = 255, .blue = 255}, 48, RgbSwingPhase::kPostImpact);
  return schedule;
}

std::vector<FeatherRgbStep> MakeCalibrationSchedule() {
  const Rgb8 white{.red = 255, .green = 255, .blue = 255};
  std::vector<FeatherRgbStep> schedule;
  AddStep(&schedule, 0ms, 140ms, {}, 0, RgbSwingPhase::kBaseline);
  constexpr std::array<std::uint8_t, 8> kBrightnesses = {8, 16, 24, 32, 48, 64, 96, 120};
  auto begin = 140ms;
  for (const std::uint8_t brightness : kBrightnesses) {
    AddStep(&schedule, begin, begin + 70ms, white, brightness, RgbSwingPhase::kBrightnessProbe);
    begin += 70ms;
  }
  return schedule;
}

std::vector<FeatherRgbStep> MakeSwingOnlySchedule() {
  const Rgb8 white{.red = 255, .green = 255, .blue = 255};
  std::vector<FeatherRgbStep> schedule;
  AddStep(&schedule, 0ms, 24ms, {}, 0, RgbSwingPhase::kBaseline);
  AddStep(&schedule, 24ms, 44ms, {.red = 255}, 48, RgbSwingPhase::kPreImpact);
  AddStep(&schedule, 44ms, 64ms, {.green = 255}, 48, RgbSwingPhase::kPreImpact);
  AddStep(&schedule, 64ms, 84ms, {.blue = 255}, 48, RgbSwingPhase::kPreImpact);
  AddStep(&schedule, 84ms, 97200us, white, 48, RgbSwingPhase::kImpact);
  AddStep(&schedule, 97200us, 117200us, {.green = 255, .blue = 255}, 48,
          RgbSwingPhase::kPostImpact);
  AddStep(&schedule, 117200us, 137200us, {.red = 255, .blue = 255}, 48, RgbSwingPhase::kPostImpact);
  return schedule;
}

const FeatherRgbStep *StepAt(std::span<const FeatherRgbStep> schedule, Clock::time_point time) {
  const auto step = std::ranges::find_if(schedule, [time](const FeatherRgbStep &candidate) {
    return time >= candidate.start && time < candidate.end_exclusive;
  });
  return step == schedule.end() ? nullptr : &*step;
}

std::uint8_t ColorComponent(Rgb8 color, std::uint32_t x, std::uint32_t y) {
  if ((y & 1U) == 0U) {
    return (x & 1U) == 0U ? color.red : color.green;
  }
  return (x & 1U) == 0U ? color.green : color.blue;
}

std::size_t BayerChannelIndex(std::uint32_t x, std::uint32_t y) {
  if ((y & 1U) == 0U) {
    return (x & 1U) == 0U ? 0U : 1U;
  }
  return (x & 1U) == 0U ? 1U : 2U;
}

bool InLedRegion(std::uint32_t x, std::uint32_t y) {
  return x >= kLedRegion.x && x < kLedRegion.x + kLedRegion.width && y >= kLedRegion.y &&
         y < kLedRegion.y + kLedRegion.height;
}

bool InLedAnnulus(std::uint32_t x, std::uint32_t y) {
  return !InLedRegion(x, y) && x >= 12U && x < 32U && y >= 8U && y < 28U;
}

bool InRegion(const PixelRegion &region, std::uint32_t x, std::uint32_t y) {
  return x >= region.x && y >= region.y && x - region.x < region.width &&
         y - region.y < region.height;
}

int DistractorStrength(std::uint8_t brightness) {
  switch (brightness) {
    case 8:
      return 180;
    case 16:
      return 20;
    case 24:
      return 160;
    case 32:
      return 10;
    case 48:
      return 140;
    case 64:
      return 5;
    case 96:
      return 120;
    case 120:
      return 5;
    default:
      return 0;
  }
}

OwnedCamera RenderCalibrationCamera(std::span<const FeatherRgbStep> schedule,
                                    std::array<double, 3> channel_response,
                                    bool include_scheduled_led = true) {
  constexpr auto kPreviewPeriod = 33ms;
  constexpr PixelRegion kScheduledDistractor = {.x = 3, .y = 4, .width = 7, .height = 7};
  constexpr PixelRegion kStaticBrightPatch = {.x = 38, .y = 29, .width = 7, .height = 7};
  const std::size_t frame_count = static_cast<std::size_t>(
      (schedule.back().end_exclusive - kOrigin - 10ms) / kPreviewPeriod + 1);
  OwnedCamera camera;
  camera.payloads.resize(frame_count);
  camera.frames.reserve(frame_count);
  camera.mapped_host_times.reserve(frame_count);
  for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
    const Clock::time_point time = kOrigin + 10ms + kPreviewPeriod * frame_index;
    const FeatherRgbStep *step = StepAt(schedule, time);
    const std::uint8_t brightness = step == nullptr ? 0U : step->brightness;
    const bool probe = step != nullptr && step->phase == RgbSwingPhase::kBrightnessProbe;
    const int noise = static_cast<int>(frame_index % 3U) - 1;
    std::vector<std::byte> &pixels = camera.payloads[frame_index];
    pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        const std::size_t channel = BayerChannelIndex(x, y);
        int value = 20 + static_cast<int>((x + 2U * y) % 3U) + noise;
        if (probe && include_scheduled_led && InRegion(kCalibrationLedRegion, x, y)) {
          value += static_cast<int>(
              std::lround(static_cast<double>(brightness) * 2.0 * channel_response[channel]));
        } else if (probe && include_scheduled_led && brightness >= 96U && x >= 23U && x < 37U &&
                   y >= 11U && y < 25U) {
          value += static_cast<int>(std::lround(35.0 * channel_response[channel]));
        }
        if (probe && InRegion(kScheduledDistractor, x, y)) {
          value += DistractorStrength(brightness);
        }
        if (InRegion(kStaticBrightPatch, x, y)) {
          value += 180;
        }
        value = std::clamp(value, 0, 255);
        pixels[static_cast<std::size_t>(y) * kWidth + x] =
            std::byte{static_cast<std::uint8_t>(value)};
      }
    }
    camera.frames.push_back({
        .image = {.pixels = pixels, .width = kWidth, .height = kHeight},
        .frame_id = 2'000U + frame_index * 8U,
        .device_timestamp = 7'000'000'000ULL + frame_index * 33'000'000ULL,
    });
    camera.mapped_host_times.push_back(time);
  }
  return camera;
}

void ShiftMappedHostTimes(OwnedCamera *camera, Clock::duration shift) {
  for (Clock::time_point &time : camera->mapped_host_times) {
    time += shift;
  }
}

OwnedCamera MakeOffsetAmbiguousCalibrationCamera(std::span<const FeatherRgbStep> schedule,
                                                 std::array<double, 3> channel_response) {
  const OwnedCamera source = RenderCalibrationCamera(schedule, channel_response);
  constexpr std::array<std::size_t, 11> kSourceIndices = {0, 1, 2, 5, 7, 9, 11, 13, 15, 18, 20};
  OwnedCamera sparse;
  sparse.payloads.resize(kSourceIndices.size());
  sparse.frames.reserve(kSourceIndices.size());
  sparse.mapped_host_times.reserve(kSourceIndices.size());
  for (std::size_t output_index = 0; output_index < kSourceIndices.size(); ++output_index) {
    const std::size_t source_index = kSourceIndices[output_index];
    sparse.payloads[output_index] = source.payloads[source_index];
    sparse.frames.push_back({
        .image = {.pixels = sparse.payloads[output_index], .width = kWidth, .height = kHeight},
        .frame_id = source.frames[source_index].frame_id,
        .device_timestamp = source.frames[source_index].device_timestamp,
    });
    if (output_index < 3U) {
      sparse.mapped_host_times.push_back(kOrigin + 40ms + 30ms * output_index);
    } else {
      const FeatherRgbStep &step = schedule[output_index - 2U];
      sparse.mapped_host_times.push_back(step.start + (step.end_exclusive - step.start) / 2);
    }
  }
  return sparse;
}

OwnedCamera RenderCamera(std::span<const FeatherRgbStep> schedule, double optical_gain,
                         bool swap_first_preimpact_color = false,
                         std::array<double, 3> channel_response = {1.0, 1.0, 1.0},
                         bool constant_middle_probe_spill = false,
                         int baseline_global_noise_amplitude = 0,
                         int baseline_roi_noise_amplitude = 0) {
  constexpr std::size_t kFrameCount = 55;
  OwnedCamera camera;
  camera.payloads.resize(kFrameCount);
  camera.frames.reserve(kFrameCount);
  camera.mapped_host_times.reserve(kFrameCount);
  for (std::size_t frame_index = 0; frame_index < kFrameCount; ++frame_index) {
    const Clock::time_point time = kOrigin + kFramePeriod * frame_index;
    const FeatherRgbStep *step = StepAt(schedule, time);
    Rgb8 color = step == nullptr ? Rgb8{} : step->color;
    const std::uint8_t brightness = step == nullptr ? 0U : step->brightness;
    if (swap_first_preimpact_color && step != nullptr && step->start == kOrigin + 100ms) {
      color = {.green = 255};
    }
    const int temporal_noise = static_cast<int>(frame_index % 3U) - 1;
    const int baseline_noise_sign = frame_index % 2U == 0U ? 1 : -1;
    const int baseline_global_noise = step != nullptr && step->phase == RgbSwingPhase::kBaseline
                                          ? baseline_noise_sign * baseline_global_noise_amplitude
                                          : 0;
    std::vector<std::byte> &pixels = camera.payloads[frame_index];
    pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        int value =
            20 + static_cast<int>((x + 2U * y) % 3U) + temporal_noise + baseline_global_noise;
        if (step != nullptr && step->phase == RgbSwingPhase::kBaseline && InLedRegion(x, y)) {
          value += baseline_noise_sign * baseline_roi_noise_amplitude;
        }
        const double component = static_cast<double>(ColorComponent(color, x, y)) / 255.0;
        if (InLedRegion(x, y)) {
          value +=
              static_cast<int>(std::lround(static_cast<double>(brightness) * optical_gain *
                                           component * channel_response[BayerChannelIndex(x, y)]));
        } else if (constant_middle_probe_spill && step != nullptr &&
                   step->phase == RgbSwingPhase::kBrightnessProbe && brightness == 48U &&
                   InLedAnnulus(x, y)) {
          value += 24;
        } else if (brightness >= 100U && component > 0.0 && x >= 12U && x < 32U && y >= 8U &&
                   y < 28U) {
          value += static_cast<int>(std::lround(35.0 * optical_gain * component));
        }
        value = std::clamp(value, 0, 255);
        pixels[static_cast<std::size_t>(y) * kWidth + x] =
            std::byte{static_cast<std::uint8_t>(value)};
      }
    }
    camera.frames.push_back({
        .image = {.pixels = pixels, .width = kWidth, .height = kHeight},
        .frame_id = 1'000U + frame_index,
        .device_timestamp = 5'000'000'000ULL + frame_index * 4'000'000ULL,
    });
    camera.mapped_host_times.push_back(time);
  }
  return camera;
}

RgbSwingCameraInput Input(
    std::string camera_id, const OwnedCamera &camera, std::span<const FeatherRgbStep> schedule,
    std::optional<swing_capture::optical::BayerColorCalibration> color_calibration = std::nullopt) {
  return {
      .camera_id = std::move(camera_id),
      .frames = camera.frames,
      .mapped_host_times = camera.mapped_host_times,
      .fixture_neopixel_region = kLedRegion,
      .schedule = schedule,
      .color_calibration = color_calibration,
  };
}

RgbSwingAnalysisOptions TestOptions() {
  return {
      .exposure_duration = 500us,
      .schedule_timing_uncertainty = 0us,
      .frame_delivery_latency_bound = 0us,
      .background_margin = 6,
      .minimum_baseline_frames = 3,
      .minimum_stable_state_frames = 2,
      .minimum_stable_impact_frames = 2,
      .minimum_brightness_probe_frames = 2,
      .minimum_state_matching_fraction = 0.75,
      .maximum_color_distance = 0.38,
      .minimum_signal_delta = 60.0,
      .minimum_signal_to_background_noise = 5.0,
      .saturation_threshold = 250,
      .maximum_saturated_fraction = 0.08,
      .bloom_delta_threshold = 18,
      .maximum_bloom_fraction = 0.08,
  };
}

RgbBrightnessCalibrationOptions CalibrationOptions() {
  RgbSwingAnalysisOptions optical = TestOptions();
  optical.schedule_timing_uncertainty = 2ms;
  optical.minimum_signal_delta = 20.0;
  return {
      .optical = optical,
      .region_width = 12,
      .region_height = 12,
      .region_stride = 4,
      .minimum_probe_levels = 3,
      .minimum_frames_per_probe = 1,
      .minimum_channel_delta = 2.0,
      .minimum_monotonic_fraction = 0.75,
  };
}

RgbBrightnessCalibrationInput CalibrationInput(std::string camera_id, const OwnedCamera &camera,
                                               std::span<const FeatherRgbStep> schedule) {
  return {
      .camera_id = std::move(camera_id),
      .frames = camera.frames,
      .mapped_host_times = camera.mapped_host_times,
      .schedule = schedule,
  };
}

bool RegionsIntersect(const PixelRegion &left, const PixelRegion &right) {
  return left.x < right.x + right.width && right.x < left.x + left.width &&
         left.y < right.y + right.height && right.y < left.y + left.height;
}

const BrightnessCandidateEvidence &Candidate(
    std::span<const BrightnessCandidateEvidence> candidates, std::uint8_t brightness) {
  const auto candidate =
      std::ranges::find(candidates, brightness, &BrightnessCandidateEvidence::brightness);
  if (candidate == candidates.end()) {
    throw std::runtime_error("test brightness candidate is missing");
  }
  return *candidate;
}

void DecodesSwingAndExposureTransitions() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 2.0);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbWhiteImpact(Input("down-the-line", camera, schedule), TestOptions());
  Expect(analysis.passed, "synthetic RGB swing passes");
  Expect(
      analysis.first_impact_frame_index.has_value() && analysis.last_impact_frame_index.has_value(),
      "white impact interval is located");
  if (analysis.first_impact_frame_index.has_value() &&
      analysis.last_impact_frame_index.has_value()) {
    Expect(*analysis.last_impact_frame_index - *analysis.first_impact_frame_index + 1U == 3U,
           "13.2 ms impact interval has three stable frames at 250 fps fixture cadence");
    for (std::size_t index = *analysis.first_impact_frame_index;
         index <= *analysis.last_impact_frame_index; ++index) {
      Expect(analysis.frames[index].decoded_color == Rgb8{.red = 255, .green = 255, .blue = 255},
             "impact frame decodes deterministic white");
    }
  }
  Expect(analysis.states.size() == 1U && analysis.states.front().passed &&
             analysis.states.front().required_for_swing,
         "returned state evidence contains only the qualified white impact");
  Expect(std::ranges::any_of(
             analysis.frames,
             [](const auto &frame) { return frame.phase == RgbFramePhase::kExposureTransition; }),
         "schedule-edge frames are retained as exposure transitions");
  Expect(analysis.frames[25].phase == RgbFramePhase::kExposureTransition,
         "frame exactly on first swing step edge is not decoded as stable");
  Expect(analysis.transitions.empty(), "pre/post color transition evidence is not evaluated");
}

void SelectsLowestSharedSafeBrightness() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera first_camera = RenderCamera(schedule, 2.0);
  const OwnedCamera second_camera = RenderCamera(schedule, 1.35);
  const RgbSwingAnalysis first =
      AnalyzeRgbSwing(Input("down-the-line", first_camera, schedule), TestOptions());
  const RgbSwingAnalysis second =
      AnalyzeRgbSwing(Input("face-on", second_camera, schedule), TestOptions());

  Expect(!Candidate(first.brightness_candidates, 12).safe_for_camera,
         "dim candidate fails local-background gate");
  Expect(Candidate(first.brightness_candidates, 48).safe_for_camera,
         "middle candidate is safe in first camera");
  Expect(Candidate(second.brightness_candidates, 48).safe_for_camera,
         "middle candidate is safe in second camera");
  Expect(!Candidate(first.brightness_candidates, 120).safe_for_camera,
         "bright candidate fails bounded saturation or bloom");

  const std::array sweeps = {
      CameraBrightnessSweep{.camera_id = first.camera_id,
                            .candidates = first.brightness_candidates},
      CameraBrightnessSweep{.camera_id = second.camera_id,
                            .candidates = second.brightness_candidates},
  };
  const auto recommendation = RecommendSharedRgbBrightness(sweeps);
  Expect(recommendation.available && recommendation.brightness == 48,
         "lowest candidate safe in both cameras is selected");
  Expect(recommendation.candidates.size() == 3U,
         "shared recommendation preserves every candidate decision");
}

void SeparatesConstantOpticalSpillFromTemporalNoise() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 2.0, false, {1.0, 1.0, 1.0}, true);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbSwing(Input("constant-spill", camera, schedule), TestOptions());
  const BrightnessCandidateEvidence &candidate = Candidate(analysis.brightness_candidates, 48U);
  const auto has_reason = [&candidate](const std::string &text) {
    return std::ranges::any_of(candidate.rejection_reasons, [&text](const std::string &reason) {
      return reason.find(text) != std::string::npos;
    });
  };
  Expect(!candidate.safe_for_camera && has_reason("bloom"),
         "constant illumination-correlated annulus spill is rejected by the bloom gate");
  Expect(!has_reason("signal-to-background-noise") &&
             candidate.minimum_signal_to_background_noise >
                 TestOptions().minimum_signal_to_background_noise,
         "constant optical spill does not masquerade as temporal background noise");

  const auto probe_frame = std::ranges::find_if(analysis.frames, [&schedule](const auto &frame) {
    return frame.phase == RgbFramePhase::kStableStep && frame.expected_step_index.has_value() &&
           schedule[*frame.expected_step_index].phase == RgbSwingPhase::kBrightnessProbe &&
           schedule[*frame.expected_step_index].brightness == 48U;
  });
  Expect(probe_frame != analysis.frames.end() &&
             probe_frame->red.background_positive_delta > 20.0 &&
             probe_frame->red.background_noise_rms < 2.0,
         "spill remains explicit current-frame background evidence while OFF noise stays low");
}

void CancelsCorrelatedGlobalOffFluctuations() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 1.35, false, {1.0, 1.0, 1.0}, false, 18);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbSwing(Input("global-baseline-fluctuation", camera, schedule), TestOptions());
  const BrightnessCandidateEvidence &candidate = Candidate(analysis.brightness_candidates, 48U);
  Expect(candidate.safe_for_camera && candidate.minimum_signal_to_background_noise >
                                          TestOptions().minimum_signal_to_background_noise,
         "spatially correlated global OFF fluctuations cancel in the ROI-minus-annulus statistic");

  const auto probe_frame = std::ranges::find_if(analysis.frames, [&schedule](const auto &frame) {
    return frame.phase == RgbFramePhase::kStableStep && frame.expected_step_index.has_value() &&
           schedule[*frame.expected_step_index].phase == RgbSwingPhase::kBrightnessProbe &&
           schedule[*frame.expected_step_index].brightness == 48U;
  });
  Expect(probe_frame != analysis.frames.end() && probe_frame->red.background_noise_rms < 1.0,
         "frame-level aggregate noise retains common-mode cancellation");
}

void RejectsNoisyRoiMinusAnnulusAggregateBySignalToNoise() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 1.35, false, {1.0, 1.0, 1.0}, false, 0, 20);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbSwing(Input("differential-baseline-noise", camera, schedule), TestOptions());
  const BrightnessCandidateEvidence &candidate = Candidate(analysis.brightness_candidates, 48U);
  const bool snr_rejection =
      std::ranges::any_of(candidate.rejection_reasons, [](const std::string &reason) {
        return reason.find("signal-to-background-noise") != std::string::npos;
      });
  Expect(candidate.minimum_signal_delta >= TestOptions().minimum_signal_delta,
         "differential-noise fixture retains enough local signal to isolate the SNR gate");
  Expect(!candidate.safe_for_camera && snr_rejection,
         "temporal ROI-minus-annulus variation rejects an otherwise bright probe by SNR");

  const auto probe_frame = std::ranges::find_if(analysis.frames, [&schedule](const auto &frame) {
    return frame.phase == RgbFramePhase::kStableStep && frame.expected_step_index.has_value() &&
           schedule[*frame.expected_step_index].phase == RgbSwingPhase::kBrightnessProbe &&
           schedule[*frame.expected_step_index].brightness == 48U;
  });
  Expect(probe_frame != analysis.frames.end() && probe_frame->red.background_noise_rms > 15.0 &&
             probe_frame->signal_to_background_noise <
                 TestOptions().minimum_signal_to_background_noise,
         "frame-level Welford variance carries differential OFF evidence into probe SNR");
}

void AcceptsBrightnessOneLikeSignalWithAggregateNoiseUnits() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 1.25);
  RgbSwingAnalysisOptions options = TestOptions();
  options.minimum_signal_delta = 20.0;
  const RgbSwingAnalysis analysis =
      AnalyzeRgbSwing(Input("brightness-one-like", camera, schedule), options);
  const BrightnessCandidateEvidence &candidate = Candidate(analysis.brightness_candidates, 12U);
  Expect(candidate.minimum_signal_delta >= 24.0 && candidate.minimum_signal_delta <= 30.0,
         "low-brightness fixture reproduces the observed 25-28 count signal range");
  Expect(candidate.safe_for_camera && candidate.minimum_signal_to_background_noise >=
                                          options.minimum_signal_to_background_noise,
         "clean brightness-one-like signal passes unchanged signal and SNR thresholds");
}

void ReportsPerCameraEvidenceWhenNoSharedBrightnessIsSafe() {
  const auto evidence = [](std::uint8_t brightness, bool safe, std::string reason) {
    BrightnessCandidateEvidence candidate;
    candidate.brightness = brightness;
    candidate.stable_frame_count = 7U;
    candidate.minimum_signal_delta = static_cast<double>(brightness);
    candidate.minimum_signal_to_background_noise = static_cast<double>(brightness) / 4.0;
    candidate.maximum_saturated_fraction = static_cast<double>(brightness) / 1000.0;
    candidate.maximum_bloom_fraction = static_cast<double>(brightness) / 2000.0;
    candidate.safe_for_camera = safe;
    if (!reason.empty()) {
      candidate.rejection_reasons.push_back(std::move(reason));
    }
    return candidate;
  };
  std::vector<BrightnessCandidateEvidence> down_the_line = {
      evidence(16U, false, "signal is not clearly above the local background"),
      evidence(32U, true, {}),
      evidence(48U, false, "ROI saturation exceeds the safe bound"),
      evidence(64U, true, {}),
  };
  std::vector<BrightnessCandidateEvidence> face_on = {
      evidence(16U, true, {}),
      evidence(32U, false, "background-annulus bloom exceeds the safe bound"),
      evidence(48U, false, "probe color does not decode consistently"),
  };
  const std::array sweeps = {
      CameraBrightnessSweep{.camera_id = "down-the-line", .candidates = down_the_line},
      CameraBrightnessSweep{.camera_id = "face-on", .candidates = face_on},
  };

  const auto recommendation = RecommendSharedRgbBrightness(sweeps);
  Expect(!recommendation.available && !recommendation.brightness.has_value(),
         "disjoint camera-safe sets do not produce a shared brightness");
  Expect(recommendation.diagnostic.find("shared_intersection=[]") != std::string::npos,
         "failure diagnostic explicitly reports the empty shared intersection");
  Expect(recommendation.diagnostic.find(
             "down-the-line:[16{eligible=false,stable_frames=7,min_signal=16.000") !=
             std::string::npos,
         "failure diagnostic reports per-camera eligibility and measured evidence");
  Expect(recommendation.diagnostic.find("32{eligible=true,stable_frames=7,min_signal=32.000") !=
             std::string::npos,
         "failure diagnostic retains camera-local safe candidates");
  Expect(recommendation.diagnostic.find(
             "face-on: background-annulus bloom exceeds the safe bound") != std::string::npos,
         "shared candidate diagnostic attributes rejection reasons to a camera");
  Expect(recommendation.diagnostic.find(
             "64{eligible=false,observed=1/2,reasons=face-on: candidate was not observed}") !=
             std::string::npos,
         "shared candidate diagnostic reports missing cross-camera observations");
}

void SelectsNonSaturatingFallbackAcrossShortExposurePwmDropout() {
  const auto evidence = [](std::uint8_t brightness, std::size_t stable_frames, double signal,
                           double saturation, double bloom, bool strictly_safe) {
    return BrightnessCandidateEvidence{
        .brightness = brightness,
        .stable_frame_count = stable_frames,
        .minimum_signal_delta = signal,
        .minimum_signal_to_background_noise = signal / 2.0,
        .maximum_saturated_fraction = saturation,
        .maximum_bloom_fraction = bloom,
        .safe_for_camera = strictly_safe,
        .rejection_reasons =
            strictly_safe
                ? std::vector<std::string>{}
                : std::vector<std::string>{"signal is not clearly above the local background"},
    };
  };
  std::vector<BrightnessCandidateEvidence> down_the_line = {
      evidence(1U, 7U, 34.0, 0.05, 0.01, true),
      evidence(2U, 7U, 190.0, 0.49, 0.09, false),
  };
  std::vector<BrightnessCandidateEvidence> face_on = {
      evidence(1U, 7U, 1.6, 0.02, 0.003, false),
      evidence(2U, 7U, 32.0, 0.32, 0.04, false),
  };
  const std::array sweeps = {
      CameraBrightnessSweep{.camera_id = "down-the-line", .candidates = down_the_line},
      CameraBrightnessSweep{.camera_id = "face-on", .candidates = face_on},
  };

  const auto strict = RecommendSharedRgbBrightness(sweeps);
  Expect(!strict.available,
         "one short-exposure PWM dropout prevents strict every-frame signal qualification");
  const auto fallback = RecommendSharedNonSaturatingRgbBrightness(sweeps);
  Expect(fallback.available && fallback.brightness == 1U,
         "lowest repeatedly sampled non-saturating level survives a short PWM OFF sample");
  Expect(!fallback.candidates[1].safe_for_every_camera,
         "brighter level remains rejected by physical saturation and bloom evidence");

  face_on.front().stable_frame_count = 2U;
  const auto insufficient = RecommendSharedNonSaturatingRgbBrightness(sweeps);
  Expect(!insufficient.available,
         "fallback does not select a level without multi-frame evidence in every camera");
}

void LocatesPreviewSweepAndCalibratesBayerColor() {
  const std::vector<FeatherRgbStep> calibration_schedule = MakeCalibrationSchedule();
  constexpr std::array<double, 3> kFirstResponse = {0.55, 1.0, 0.35};
  constexpr std::array<double, 3> kSecondResponse = {0.7, 0.9, 0.5};
  const OwnedCamera first_frames = RenderCalibrationCamera(calibration_schedule, kFirstResponse);
  const OwnedCamera second_frames = RenderCalibrationCamera(calibration_schedule, kSecondResponse);
  const auto first = AnalyzeRgbBrightnessCalibration(
      CalibrationInput("down-the-line", first_frames, calibration_schedule), CalibrationOptions());
  const auto second = AnalyzeRgbBrightnessCalibration(
      CalibrationInput("face-on", second_frames, calibration_schedule), CalibrationOptions());

  Expect(first.located && RegionsIntersect(first.selected_region, kCalibrationLedRegion),
         "unknown RGB ROI locator selects the scheduled NeoPixel response: " + first.diagnostic +
             " score=" + std::to_string(first.schedule_offset.fit_score) +
             " next=" + std::to_string(first.schedule_offset.next_best_score));
  Expect(first.schedule_offset.available &&
             std::chrono::abs(first.schedule_offset.mapped_time_correction) <=
                 first.schedule_offset.uncertainty,
         "unshifted preview timeline estimates a correction interval containing zero: correction=" +
             std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                first.schedule_offset.mapped_time_correction)
                                .count()) +
             "us uncertainty=" +
             std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                first.schedule_offset.uncertainty)
                                .count()) +
             "us");
  Expect(first.missing_frame_ids > 0U, "preview-sampler frame-ID gaps are accepted and reported");
  Expect(first.color_calibration.automatic && first.color_calibration.source_brightness == 64U,
         "strongest usable white probe derives camera-local Bayer normalization");
  Expect(first.color_calibration.blue_scale > first.color_calibration.green_scale,
         "white reference compensates the fixture's weaker blue response");
  Expect(first.brightness_candidates.size() == 8U,
         "every commanded calibration brightness retains evidence");
  Expect(Candidate(first.brightness_candidates, 48).safe_for_camera,
         "middle calibration level is safe after automatic ROI selection");
  const auto &bright_candidate = Candidate(first.brightness_candidates, 120);
  Expect(!bright_candidate.safe_for_camera,
         "bright calibration level is rejected for saturation or bloom");
  Expect(bright_candidate.maximum_saturated_fraction >
             CalibrationOptions().optical.maximum_saturated_fraction,
         "bright calibration fixture exercises the saturation bound");
  Expect(
      bright_candidate.maximum_bloom_fraction > CalibrationOptions().optical.maximum_bloom_fraction,
      "bright calibration fixture exercises the bloom bound");
  const auto &bright_reasons = bright_candidate.rejection_reasons;
  Expect(std::ranges::any_of(bright_reasons,
                             [](const std::string &reason) {
                               return reason.find("saturation") != std::string::npos ||
                                      reason.find("bloom") != std::string::npos;
                             }),
         "bright calibration rejection reports saturation or bloom evidence");

  const OwnedCamera distractor_only_frames =
      RenderCalibrationCamera(calibration_schedule, kFirstResponse, false);
  const auto distractor_only = AnalyzeRgbBrightnessCalibration(
      CalibrationInput("distractors-only", distractor_only_frames, calibration_schedule),
      CalibrationOptions());
  Expect(!distractor_only.located,
         "static and schedule-correlated nonmonotonic distractors do not produce an LED ROI");

  const std::array sweeps = {
      CameraBrightnessSweep{.camera_id = first.camera_id,
                            .candidates = first.brightness_candidates},
      CameraBrightnessSweep{.camera_id = second.camera_id,
                            .candidates = second.brightness_candidates},
  };
  const auto shared = RecommendSharedRgbBrightness(sweeps);
  Expect(shared.available && shared.brightness == 48U,
         "preview calibration chooses one safe shared level for both cameras");

  const std::vector<FeatherRgbStep> swing_schedule = MakeSwingOnlySchedule();
  const OwnedCamera swing_frames = RenderCamera(swing_schedule, 2.0, false, kFirstResponse);
  RgbSwingAnalysisOptions swing_options = TestOptions();
  swing_options.minimum_signal_delta = 25.0;
  const RgbSwingAnalysis calibrated = AnalyzeRgbSwing(
      Input("down-the-line", swing_frames, swing_schedule, first.color_calibration), swing_options);
  const RgbSwingAnalysis raw =
      AnalyzeRgbSwing(Input("down-the-line", swing_frames, swing_schedule), swing_options);
  Expect(calibrated.passed, "pre-arm Bayer calibration decodes the later swing-only schedule");
  Expect(!raw.passed, "skewed raw Bayer chroma is not silently treated as calibrated");
}

void EstimatesSignedPreviewTimelineOffsetsAndRejectsAmbiguity() {
  const std::vector<FeatherRgbStep> schedule = MakeCalibrationSchedule();
  constexpr std::array<double, 3> kResponse = {0.55, 1.0, 0.35};
  for (const Clock::duration injected_shift : {12ms, -9ms, 35ms}) {
    OwnedCamera camera = RenderCalibrationCamera(schedule, kResponse);
    ShiftMappedHostTimes(&camera, injected_shift);
    const RgbBrightnessCalibrationOptions options = CalibrationOptions();
    const auto calibration = AnalyzeRgbBrightnessCalibration(
        CalibrationInput("shifted-camera", camera, schedule), options);
    const Clock::duration expected_correction = -injected_shift;
    Expect(calibration.located && calibration.schedule_offset.available,
           "bounded offset search accepts a uniquely shifted preview timeline: " +
               calibration.diagnostic);
    Expect(std::chrono::abs(calibration.schedule_offset.mapped_time_correction -
                            expected_correction) <= calibration.schedule_offset.uncertainty,
           "estimated signed correction interval contains the injected inverse shift");
  }

  const OwnedCamera ambiguous = MakeOffsetAmbiguousCalibrationCamera(schedule, kResponse);
  const auto rejected = AnalyzeRgbBrightnessCalibration(
      CalibrationInput("ambiguous-camera", ambiguous, schedule), CalibrationOptions());
  Expect(!rejected.located && !rejected.schedule_offset.available &&
             (rejected.schedule_offset.best_plateau_touches_search_edge ||
              rejected.schedule_offset.uncertainty >
                  CalibrationOptions().maximum_schedule_offset_uncertainty),
         "offset-invariant sparse samples are rejected as a broad or edge-spanning solution: " +
             rejected.diagnostic + " correction_us=" +
             std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                rejected.schedule_offset.mapped_time_correction)
                                .count()) +
             " uncertainty_us=" +
             std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                rejected.schedule_offset.uncertainty)
                                .count()) +
             " fit=" + std::to_string(rejected.schedule_offset.fit_score) +
             " next=" + std::to_string(rejected.schedule_offset.next_best_score));
}

void AcceptsOneBroadOffsetPeakButRejectsRemoteCompetition() {
  const std::vector<FeatherRgbStep> schedule = MakeCalibrationSchedule();
  constexpr std::array<double, 3> kResponse = {0.55, 1.0, 0.35};
  OwnedCamera camera = RenderCalibrationCamera(schedule, kResponse);
  ShiftMappedHostTimes(&camera, 10750us);

  RgbBrightnessCalibrationOptions options = CalibrationOptions();
  const auto broad_peak =
      AnalyzeRgbBrightnessCalibration(CalibrationInput("broad-peak", camera, schedule), options);
  Expect(broad_peak.located && broad_peak.schedule_offset.available,
         "near-equal correction candidates on one continuous shoulder are one physical peak: " +
             broad_peak.schedule_offset.diagnostic +
             " fit=" + std::to_string(broad_peak.schedule_offset.fit_score) +
             " next=" + std::to_string(broad_peak.schedule_offset.next_best_score));
  constexpr std::array<double, 9> kBroadSinglePeak = {
      0.80, 0.9999980, 0.9999992, 0.9999998, 1.0, 0.9999998, 0.9999992, 0.9999980, 0.80,
  };
  const auto broad_component = FindConnectedOffsetScoreComponent(kBroadSinglePeak, 4U, 4U, 1.0e-6);
  Expect(broad_component.first_index == 2U && broad_component.last_index == 6U &&
             broad_component.next_best_score == 0.9999980,
         "near-equal adjacent shoulder candidates form one connected peak");

  constexpr std::array<double, 9> kRemoteCompetingPeak = {
      0.80, 0.9999995, 1.0, 0.9999995, 0.80, 1.0, 0.9999994, 0.80, 0.70,
  };
  const auto separated_component =
      FindConnectedOffsetScoreComponent(kRemoteCompetingPeak, 2U, 2U, 1.0e-6);
  Expect(separated_component.first_index == 1U && separated_component.last_index == 3U &&
             separated_component.next_best_score == 1.0,
         "a near-equal remote peak beyond a score valley remains competing evidence");
}

void DoesNotEvaluateWrongDiscretePreImpactColor() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 2.0, true);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbWhiteImpact(Input("wrong-color", camera, schedule), TestOptions());
  Expect(analysis.passed, "wrong pre-impact color remains human context and does not reject HIL");
  Expect(analysis.states.size() == 1U && analysis.states.front().phase == RgbSwingPhase::kImpact,
         "pre-impact hue is absent from qualification evidence");
  const auto pre_impact = std::ranges::find_if(analysis.frames, [](const auto &frame) {
    return frame.phase == RgbFramePhase::kStableStep && frame.expected_step_index == 6U;
  });
  Expect(pre_impact != analysis.frames.end() && !pre_impact->expected_color_match &&
             !pre_impact->decoded_color.has_value(),
         "pre-impact pixels are not color decoded or matched");
  Expect(analysis.first_impact_frame_index.has_value(),
         "white impact evidence remains available after a pre-state mismatch");
}

void AggregatesAContiguousWhiteImpactInterval() {
  std::vector<FeatherRgbStep> schedule = MakeSchedule();
  FeatherRgbStep second_half = schedule[9];
  const auto midpoint = second_half.start + (second_half.end_exclusive - second_half.start) / 2;
  schedule[9].end_exclusive = midpoint;
  second_half.start = midpoint;
  schedule.insert(schedule.begin() + 10, second_half);
  const OwnedCamera camera = RenderCamera(schedule, 2.0);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbWhiteImpact(Input("split-impact", camera, schedule), TestOptions());
  Expect(analysis.passed && analysis.states.size() == 1U &&
             analysis.states.front().matching_frame_count >= 2U,
         "contiguous white schedule steps produce one impact-interval qualification");
}

void RejectsWeakWhiteImpact() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  const OwnedCamera camera = RenderCamera(schedule, 0.2);
  const RgbSwingAnalysis analysis =
      AnalyzeRgbWhiteImpact(Input("weak-impact", camera, schedule), TestOptions());
  const auto impact =
      std::ranges::find(analysis.states, RgbSwingPhase::kImpact, &RgbStateEvidence::phase);
  Expect(impact != analysis.states.end() && impact->required_for_swing && !impact->passed,
         "weak white impact fails the required signal gate");
  Expect(!analysis.passed, "weak white impact rejects HIL even when the schedule is well formed");
}

void ValidatesFramesTimelineScheduleAndSweeps() {
  const std::vector<FeatherRgbStep> schedule = MakeSchedule();
  OwnedCamera camera = RenderCamera(schedule, 2.0);
  camera.mapped_host_times[4] = camera.mapped_host_times[3];
  ExpectInvalidArgument(
      [&] {
        static_cast<void>(AnalyzeRgbSwing(Input("bad-time", camera, schedule), TestOptions()));
      },
      "nonmonotonic mapped timeline rejected");

  camera = RenderCamera(schedule, 2.0);
  camera.frames.erase(camera.frames.begin() + 4);
  camera.mapped_host_times.erase(camera.mapped_host_times.begin() + 4);
  ExpectInvalidArgument(
      [&] {
        static_cast<void>(AnalyzeRgbSwing(Input("gapped-swing", camera, schedule), TestOptions()));
      },
      "retained swing analysis rejects frame-ID gaps even though preview calibration accepts them");

  camera = RenderCamera(schedule, 2.0);
  std::vector<FeatherRgbStep> nonwhite_impact = schedule;
  nonwhite_impact[9].color = {.red = 255};
  ExpectInvalidArgument(
      [&] {
        static_cast<void>(
            AnalyzeRgbSwing(Input("bad-impact", camera, nonwhite_impact), TestOptions()));
      },
      "nonwhite impact schedule rejected");

  const std::array one_sweep = {CameraBrightnessSweep{
      .camera_id = "only-camera",
      .candidates = std::span<const BrightnessCandidateEvidence>{},
  }};
  ExpectInvalidArgument([&] { static_cast<void>(RecommendSharedRgbBrightness(one_sweep)); },
                        "shared brightness requires two camera sweeps");
}

}  // namespace

int main() {
  DecodesSwingAndExposureTransitions();
  SelectsLowestSharedSafeBrightness();
  SeparatesConstantOpticalSpillFromTemporalNoise();
  CancelsCorrelatedGlobalOffFluctuations();
  RejectsNoisyRoiMinusAnnulusAggregateBySignalToNoise();
  AcceptsBrightnessOneLikeSignalWithAggregateNoiseUnits();
  ReportsPerCameraEvidenceWhenNoSharedBrightnessIsSafe();
  SelectsNonSaturatingFallbackAcrossShortExposurePwmDropout();
  LocatesPreviewSweepAndCalibratesBayerColor();
  EstimatesSignedPreviewTimelineOffsetsAndRejectsAmbiguity();
  AcceptsOneBroadOffsetPeakButRejectsRemoteCompetition();
  DoesNotEvaluateWrongDiscretePreImpactColor();
  AggregatesAContiguousWhiteImpactInterval();
  RejectsWeakWhiteImpact();
  ValidatesFramesTimelineScheduleAndSweeps();
  if (failures != 0) {
    std::cerr << failures << " RGB swing optical test(s) failed\n";
    return 1;
  }
  return 0;
}
