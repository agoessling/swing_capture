#include "capture/service/synthetic_swing_station_workflow.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/application/capture_controller.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/preview/latest_frame_sampler.h"

namespace {

using swing_capture::application::CapturedCameraWindow;
using swing_capture::application::CapturedSession;
using swing_capture::application::SessionIdentity;
using swing_capture::service::SyntheticSwingCameraAnalysisProfile;
using swing_capture::service::SyntheticSwingCameraCalibrationSource;
using swing_capture::service::SyntheticSwingStationWorkflow;
using swing_capture::service::SyntheticSwingStationWorkflowHooks;
using swing_capture::service::synthetic_swing_workflow_internal::ConstrainReceiptTimelineCorrection;
using swing_capture::service::synthetic_swing_workflow_internal::
    FormatBrightnessRecommendationFailure;

using namespace std::chrono_literals;

std::shared_ptr<const swing_capture::preview::SampledPreviewFrame> NoFrame() { return {}; }

SyntheticSwingCameraAnalysisProfile DefaultAnalysisProfile() {
  return {.exposure_duration = 500us, .gain_decibels = 24.0};
}

SyntheticSwingCameraCalibrationSource PassiveSource(std::string role) {
  return {
      .role = std::move(role),
      .device_ticks_per_second = 1'000'000,
      .latest_frame = NoFrame,
      .analysis_profile = DefaultAnalysisProfile,
      .sampling_interval = [] { return std::chrono::steady_clock::duration(33ms); },
      .set_sampling_interval =
          [](std::chrono::steady_clock::duration interval) {
            assert(interval > std::chrono::steady_clock::duration::zero());
          },
  };
}

class CadenceProbe final {
 public:
  explicit CadenceProbe(bool fail_override = false) : fail_override_(fail_override) {}

  std::chrono::steady_clock::duration Get() const {
    const std::scoped_lock lock(mutex_);
    return current_;
  }

  void Set(std::chrono::steady_clock::duration interval) {
    const std::scoped_lock lock(mutex_);
    history_.push_back(interval);
    if (fail_override_ && interval == 8ms) {
      throw std::runtime_error("injected cadence override failure");
    }
    current_ = interval;
  }

  bool SawOverrideAndRestore() const {
    const std::scoped_lock lock(mutex_);
    return history_ == std::vector<std::chrono::steady_clock::duration>{8ms, 33ms} &&
           current_ == 33ms;
  }

 private:
  mutable std::mutex mutex_;
  std::chrono::steady_clock::duration current_ = 33ms;
  std::vector<std::chrono::steady_clock::duration> history_;
  bool fail_override_ = false;
};

SyntheticSwingCameraCalibrationSource ProbedSource(
    std::string role, CadenceProbe *probe,
    std::function<std::shared_ptr<const swing_capture::preview::SampledPreviewFrame>()>
        latest_frame = NoFrame) {
  return {
      .role = std::move(role),
      .device_ticks_per_second = 1'000'000,
      .latest_frame = std::move(latest_frame),
      .analysis_profile = DefaultAnalysisProfile,
      .sampling_interval = [probe] { return probe->Get(); },
      .set_sampling_interval =
          [probe](std::chrono::steady_clock::duration interval) { probe->Set(interval); },
  };
}

SyntheticSwingStationWorkflow MakeWorkflow() {
  return SyntheticSwingStationWorkflow(
      std::array{PassiveSource("down_the_line"), PassiveSource("face_on")},
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather = [] { return swing_capture::hil::FeatherCalibrationReceipt{}; },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
}

void TestReceiptTimelineCorrectionRejectsNoncausalPositiveOffsets() {
  assert(ConstrainReceiptTimelineCorrection(11ms) == 0ms);
  assert(ConstrainReceiptTimelineCorrection(-3500us) == -3500us);
  assert(ConstrainReceiptTimelineCorrection(0ms) == 0ms);
}

void TestCalibrationCadenceRestoresOnCancellation() {
  CadenceProbe first;
  CadenceProbe second;
  SyntheticSwingStationWorkflow workflow(
      std::array{ProbedSource("down_the_line", &first), ProbedSource("face_on", &second)},
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather = [] { return swing_capture::hil::FeatherCalibrationReceipt{}; },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try {
    static_cast<void>(workflow.CalibrateBrightness(stop.get_token()));
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  assert(cancelled);
  assert(first.SawOverrideAndRestore());
  assert(second.SawOverrideAndRestore());
}

void TestCalibrationCadenceRestoresBeforeAnalysis() {
  CadenceProbe first;
  CadenceProbe second;
  SyntheticSwingStationWorkflow workflow(
      std::array{ProbedSource("down_the_line", &first), ProbedSource("face_on", &second)},
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather =
              [&] {
                assert(first.Get() == 8ms);
                assert(second.Get() == 8ms);
                return swing_capture::hil::FeatherCalibrationReceipt{};
              },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
  bool analysis_failed = false;
  try {
    static_cast<void>(workflow.CalibrateBrightness({}));
  } catch (const std::runtime_error &) {
    analysis_failed = true;
  }
  assert(analysis_failed);
  assert(first.SawOverrideAndRestore());
  assert(second.SawOverrideAndRestore());
}

void TestCalibrationCadenceRestoresOnFeatherFailure() {
  CadenceProbe first;
  CadenceProbe second;
  SyntheticSwingStationWorkflow workflow(
      std::array{ProbedSource("down_the_line", &first), ProbedSource("face_on", &second)},
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather = []() -> swing_capture::hil::FeatherCalibrationReceipt {
            throw std::runtime_error("injected Feather failure");
          },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
  bool failed = false;
  try {
    static_cast<void>(workflow.CalibrateBrightness({}));
  } catch (const std::runtime_error &) {
    failed = true;
  }
  assert(failed);
  assert(first.SawOverrideAndRestore());
  assert(second.SawOverrideAndRestore());
}

void TestCadenceOverrideFailuresRestoreCompletedOverrides() {
  const auto hooks = SyntheticSwingStationWorkflowHooks{
      .calibrate_feather = [] { return swing_capture::hil::FeatherCalibrationReceipt{}; },
      .run_feather_swing = [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
  };
  {
    CadenceProbe failing_first(true);
    CadenceProbe second;
    SyntheticSwingStationWorkflow workflow(
        std::array{ProbedSource("down_the_line", &failing_first), ProbedSource("face_on", &second)},
        hooks);
    bool failed = false;
    try {
      static_cast<void>(workflow.CalibrateBrightness({}));
    } catch (const std::runtime_error &) {
      failed = true;
    }
    assert(failed);
    assert(failing_first.Get() == 33ms);
    assert(second.Get() == 33ms);
  }
  {
    CadenceProbe first;
    CadenceProbe failing_second(true);
    SyntheticSwingStationWorkflow workflow(
        std::array{ProbedSource("down_the_line", &first), ProbedSource("face_on", &failing_second)},
        hooks);
    bool failed = false;
    try {
      static_cast<void>(workflow.CalibrateBrightness({}));
    } catch (const std::runtime_error &) {
      failed = true;
    }
    assert(failed);
    assert(first.SawOverrideAndRestore());
    assert(failing_second.Get() == 33ms);
  }
}

class UnlimitedFrameSource final {
 public:
  std::shared_ptr<const swing_capture::preview::SampledPreviewFrame> Next() {
    const auto sequence = ++sequence_;
    return std::make_shared<const swing_capture::preview::SampledPreviewFrame>(
        swing_capture::preview::SampledPreviewFrame{
            .metadata =
                swing_capture::FrameMetadata{
                    .frame_id = sequence,
                    .device_timestamp = sequence * 1000U,
                    .host_received_at = std::chrono::steady_clock::now(),
                    .width = 2,
                    .height = 2,
                    .complete = true,
                },
            .preview_sequence = sequence,
            .bayer_pixels = std::vector<std::byte>(4),
        });
  }

 private:
  std::uint64_t sequence_ = 0;
};

void TestCalibrationSampleCapRestoresCadence() {
  CadenceProbe first;
  CadenceProbe second;
  UnlimitedFrameSource first_frames;
  UnlimitedFrameSource second_frames;
  SyntheticSwingStationWorkflow workflow(
      std::array{
          ProbedSource("down_the_line", &first, [&] { return first_frames.Next(); }),
          ProbedSource("face_on", &second, [&] { return second_frames.Next(); }),
      },
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather =
              [] {
                std::this_thread::sleep_for(500ms);
                return swing_capture::hil::FeatherCalibrationReceipt{};
              },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
  bool capped = false;
  try {
    static_cast<void>(workflow.CalibrateBrightness({}));
  } catch (const std::runtime_error &error) {
    capped = std::string(error.what()).find("sample limit") != std::string::npos;
  }
  assert(capped);
  assert(first.SawOverrideAndRestore());
  assert(second.SawOverrideAndRestore());
}

class CalibrationScheduleFixture final {
 public:
  void Start(std::chrono::steady_clock::time_point base) {
    const std::scoped_lock lock(mutex_);
    base_ = base;
  }

  bool started() const {
    const std::scoped_lock lock(mutex_);
    return base_.has_value();
  }

  std::optional<std::chrono::steady_clock::time_point> base() const {
    const std::scoped_lock lock(mutex_);
    return base_;
  }

  std::uint8_t BrightnessAt(std::chrono::steady_clock::time_point time) const {
    constexpr std::array<std::uint8_t, 8> kCandidates = {1, 2, 3, 4, 6, 8, 12, 16};
    const std::scoped_lock lock(mutex_);
    if (!base_.has_value() || time < *base_ + 20ms) {
      return 0;
    }
    const auto index = static_cast<std::size_t>((time - (*base_ + 20ms)) / 70ms);
    return index < kCandidates.size() ? kCandidates[index] : 0U;
  }

  swing_capture::hil::FeatherCalibrationReceipt Run() {
    constexpr std::array<std::uint32_t, 8> kCandidates = {1, 2, 3, 4, 6, 8, 12, 16};
    const auto base = std::chrono::steady_clock::now();
    Start(base);
    swing_capture::hil::FeatherCalibrationReceipt receipt;
    receipt.accepted_device_microseconds = 1'000'000;
    receipt.start_scheduled_device_microseconds = 1'020'000;
    receipt.step_microseconds = 70'000;
    receipt.candidates = std::vector<std::uint32_t>(kCandidates.begin(), kCandidates.end());
    receipt.host_command_sent = base;
    receipt.host_acknowledgement_received = base;
    receipt.steps.reserve(kCandidates.size());
    for (std::size_t index = 0; index < kCandidates.size(); ++index) {
      swing_capture::hil::FeatherCalibrationStep step;
      step.index = static_cast<std::uint32_t>(index);
      step.brightness = kCandidates[index];
      step.scheduled_device_microseconds = 1'020'000U + index * 70'000U;
      receipt.steps.push_back(std::move(step));
    }
    std::this_thread::sleep_until(base + 585ms);
    return receipt;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<std::chrono::steady_clock::time_point> base_;
};

class CalibrationImageSource final {
 public:
  CalibrationImageSource(CalibrationScheduleFixture *schedule, CadenceProbe *cadence,
                         bool cluster_probe_samples = false)
      : schedule_(schedule), cadence_(cadence), cluster_probe_samples_(cluster_probe_samples) {}

  std::shared_ptr<const swing_capture::preview::SampledPreviewFrame> Latest() {
    const auto now = std::chrono::steady_clock::now();
    auto sample_time = now;
    if (const auto base = schedule_->base(); base.has_value() && !cluster_probe_samples_) {
      constexpr std::array kSampleOffsets = {3ms, 35ms, 67ms};
      if (probe_sample_index_ == 8U * kSampleOffsets.size()) {
        return latest_;
      }
      const std::size_t step = probe_sample_index_ / kSampleOffsets.size();
      const std::size_t within_step = probe_sample_index_ % kSampleOffsets.size();
      const auto target = *base + 20ms + 70ms * step + kSampleOffsets[within_step];
      if (now < target) {
        return latest_;
      }
      sample_time = target;
      ++probe_sample_index_;
    } else {
      const auto interval = schedule_->started() ? 33ms : cadence_->Get();
      if (latest_ != nullptr && now - last_published_at_ < interval) {
        return latest_;
      }
      last_published_at_ = now;
    }
    ++sequence_;
    constexpr std::uint32_t kWidth = 48;
    constexpr std::uint32_t kHeight = 40;
    const std::uint8_t brightness = schedule_->BrightnessAt(sample_time);
    std::vector<std::byte> pixels(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        int value = 20 + static_cast<int>((x + 2U * y) % 3U) + static_cast<int>(sequence_ % 3U) - 1;
        if (x >= 16U && x < 32U && y >= 8U && y < 24U) {
          value += static_cast<int>(brightness) * 10;
        }
        pixels[static_cast<std::size_t>(y) * kWidth + x] =
            std::byte{static_cast<std::uint8_t>(std::clamp(value, 0, 255))};
      }
    }
    latest_ = std::make_shared<const swing_capture::preview::SampledPreviewFrame>(
        swing_capture::preview::SampledPreviewFrame{
            .metadata =
                swing_capture::FrameMetadata{
                    .frame_id = 10'000U + sequence_ * 2U,
                    .device_timestamp = sequence_ * 8'000U,
                    .host_received_at = sample_time,
                    .width = kWidth,
                    .height = kHeight,
                    .complete = true,
                },
            .preview_sequence = sequence_,
            .bayer_pixels = std::move(pixels),
        });
    return latest_;
  }

 private:
  CalibrationScheduleFixture *schedule_;
  CadenceProbe *cadence_;
  bool cluster_probe_samples_ = false;
  std::chrono::steady_clock::time_point last_published_at_;
  std::size_t probe_sample_index_ = 0;
  std::uint64_t sequence_ = 0;
  std::shared_ptr<const swing_capture::preview::SampledPreviewFrame> latest_;
};

void TestSuccessfulCalibrationRestoresCadence() {
  CadenceProbe first;
  CadenceProbe second;
  CalibrationScheduleFixture schedule;
  CalibrationImageSource first_frames(&schedule, &first);
  CalibrationImageSource second_frames(&schedule, &second);
  SyntheticSwingStationWorkflow workflow(
      std::array{
          ProbedSource("down_the_line", &first, [&] { return first_frames.Latest(); }),
          ProbedSource("face_on", &second, [&] { return second_frames.Latest(); }),
      },
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather = [&] { return schedule.Run(); },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
  assert(workflow.CalibrateBrightness({}) != 0U);
  assert(first.SawOverrideAndRestore());
  assert(second.SawOverrideAndRestore());
}

void TestClusteredProbeSamplingCannotCalibrate() {
  CadenceProbe first;
  CadenceProbe second;
  CalibrationScheduleFixture schedule;
  CalibrationImageSource first_frames(&schedule, &first, true);
  CalibrationImageSource second_frames(&schedule, &second, true);
  SyntheticSwingStationWorkflow workflow(
      std::array{
          ProbedSource("down_the_line", &first, [&] { return first_frames.Latest(); }),
          ProbedSource("face_on", &second, [&] { return second_frames.Latest(); }),
      },
      SyntheticSwingStationWorkflowHooks{
          .calibrate_feather = [&] { return schedule.Run(); },
          .run_feather_swing =
              [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
      });
  bool rejected = false;
  try {
    static_cast<void>(workflow.CalibrateBrightness({}));
  } catch (const std::runtime_error &error) {
    const std::string diagnostic = error.what();
    rejected = diagnostic.find("retained only") == std::string::npos &&
               diagnostic.find("frame-ID") == std::string::npos;
  }
  assert(rejected);
  assert(first.SawOverrideAndRestore());
  assert(second.SawOverrideAndRestore());
}

void TestInactiveCaptureIsNotDecorated() {
  auto workflow = MakeWorkflow();
  swing_capture::PooledRawFrameRing first(
      {.active_frame_capacity = 2, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  swing_capture::PooledRawFrameRing second(
      {.active_frame_capacity = 2, .reserve_frame_blocks = 0, .maximum_payload_bytes = 4});
  CapturedSession captured{
      .identity = SessionIdentity{.session_id = "ordinary", .created_at_utc = "now"},
      .trigger = {},
      .cameras =
          std::array{
              CapturedCameraWindow{.role = "down_the_line",
                                   .serial = "DOWN",
                                   .device_ticks_per_second = 1'000'000,
                                   .frames = first.Freeze()},
              CapturedCameraWindow{.role = "face_on",
                                   .serial = "FACE",
                                   .device_ticks_per_second = 1'000'000,
                                   .frames = second.Freeze()},
          },
      .pipeline_timing = {},
  };
  assert(!workflow.AnalyzeCapturedSession(captured).has_value());
  workflow.Reset();
}

void TestIncompleteConfigurationIsRejected() {
  bool rejected = false;
  try {
    static_cast<void>(SyntheticSwingStationWorkflow(
        {}, SyntheticSwingStationWorkflowHooks{
                .calibrate_feather = [] { return swing_capture::hil::FeatherCalibrationReceipt{}; },
                .run_feather_swing =
                    [](std::uint32_t) { return swing_capture::hil::FeatherSwingReceipt{}; },
            }));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

void TestBrightnessFailureIncludesCameraAlignment() {
  std::array<swing_capture::optical::RgbBrightnessCalibration, 2> calibrations;
  calibrations[0].camera_id = "down_the_line";
  calibrations[0].selected_region = {.x = 101, .y = 202, .width = 16, .height = 16};
  calibrations[0].schedule_offset.mapped_time_correction = -10750us;
  calibrations[0].schedule_offset.uncertainty = 3000us;
  calibrations[0].schedule_offset.fit_score = 0.999997;
  calibrations[0].schedule_offset.next_best_score = 0.95;
  calibrations[0].brightness_candidates.push_back({
      .brightness = 3,
      .stable_frame_count = 4,
      .minimum_signal_delta = 18.0,
      .minimum_signal_to_background_noise = 7.0,
      .maximum_saturated_fraction = 0.1,
      .maximum_bloom_fraction = 0.02,
      .safe_for_camera = false,
      .rejection_reasons = {"saturation"},
  });
  calibrations[1].camera_id = "face_on";
  calibrations[1].selected_region = {.x = 303, .y = 404, .width = 16, .height = 16};
  calibrations[1].schedule_offset.mapped_time_correction = 1250us;
  calibrations[1].schedule_offset.uncertainty = 2750us;
  swing_capture::optical::SharedBrightnessRecommendation recommendation;
  recommendation.diagnostic =
      "no brightness candidate was safe in every camera; shared_intersection=[]";
  recommendation.candidates.push_back({
      .brightness = 3,
      .cameras_observed = 2,
      .safe_for_every_camera = false,
      .worst_minimum_signal_delta = 17.0,
      .worst_minimum_signal_to_background_noise = 6.0,
      .worst_maximum_saturated_fraction = 0.1,
      .worst_maximum_bloom_fraction = 0.03,
      .rejection_reasons = {"down_the_line: saturation"},
  });

  const std::string diagnostic =
      FormatBrightnessRecommendationFailure(calibrations, recommendation);
  assert(
      diagnostic.find("down_the_line{roi=101,202,16,16,correction_us=-10750,uncertainty_us=3000") !=
      std::string::npos);
  assert(diagnostic.find("face_on{roi=303,404,16,16,correction_us=1250,uncertainty_us=2750") !=
         std::string::npos);
  assert(diagnostic.find("down_the_line{brightness=3,stable_frames=4,minimum_signal=18") !=
         std::string::npos);
  assert(diagnostic.find("safe=0,reasons=[saturation]") != std::string::npos);
  assert(diagnostic.find("shared_intersection=[]") != std::string::npos);
}

}  // namespace

int main() {
  TestReceiptTimelineCorrectionRejectsNoncausalPositiveOffsets();
  TestInactiveCaptureIsNotDecorated();
  TestIncompleteConfigurationIsRejected();
  TestBrightnessFailureIncludesCameraAlignment();
  TestCalibrationCadenceRestoresOnCancellation();
  TestCalibrationCadenceRestoresBeforeAnalysis();
  TestCalibrationCadenceRestoresOnFeatherFailure();
  TestCadenceOverrideFailuresRestoreCompletedOverrides();
  TestCalibrationSampleCapRestoresCadence();
  TestSuccessfulCalibrationRestoresCadence();
  TestClusteredProbeSamplingCannotCalibrate();
  return 0;
}
