#include "capture/service/synthetic_swing_station_workflow.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "capture/application/capture_controller.h"
#include "capture/application/clip_session_publisher.h"
#include "capture/core/camera_source.h"
#include "capture/core/device_clock_mapper.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/image/image_quality.h"
#include "capture/optical/led_pulse.h"
#include "capture/optical/rgb_swing.h"
#include "capture/preview/latest_frame_sampler.h"
#include "capture/service/synthetic_swing_hil_timeline.h"
#include "embedded/prop_maker/swing_sequence.h"

namespace swing_capture::service {
namespace {

using CalibrationSample = std::shared_ptr<const preview::SampledPreviewFrame>;
using CalibrationSamples = std::array<std::vector<CalibrationSample>, 2>;

constexpr auto kCalibrationBaselineDuration = std::chrono::milliseconds(150);
constexpr auto kCalibrationPollInterval = std::chrono::milliseconds(2);
constexpr auto kCalibrationSamplingInterval = std::chrono::milliseconds(8);
constexpr auto kCalibrationDeadline = std::chrono::seconds(2);
constexpr std::size_t kMaximumCalibrationSamplesPerCamera = 160;
constexpr std::size_t kMinimumCalibrationSamplesPerCamera = 32;
constexpr std::size_t kMinimumFrameIdCoverageDenominator = 5;
constexpr auto kSwingReceiptWait = std::chrono::seconds(3);
constexpr auto kMaximumHilScheduleUncertainty = std::chrono::milliseconds(3);

template <typename Value>
const Value &RequireValue(const std::optional<Value> &value, std::string_view message) {
  if (!value.has_value()) {
    throw std::logic_error(std::string(message));
  }
  return *value;
}

void ValidateSource(const SyntheticSwingCameraCalibrationSource &source) {
  if (source.role.empty() || source.device_ticks_per_second == 0U || !source.latest_frame ||
      !source.analysis_profile || !source.sampling_interval || !source.set_sampling_interval) {
    throw std::invalid_argument("synthetic swing camera calibration source is incomplete");
  }
}

class CalibrationSamplingCadence final {
 public:
  explicit CalibrationSamplingCadence(
      const std::array<SyntheticSwingCameraCalibrationSource, 2> &sources)
      : sources_(sources) {
    for (std::size_t index = 0; index < sources_.size(); ++index) {
      previous_[index] = sources_[index].sampling_interval();
      if (previous_[index] <= std::chrono::steady_clock::duration::zero()) {
        throw std::logic_error("camera reported an invalid latest-frame sampling interval");
      }
    }
    try {
      for (; overridden_ < sources_.size(); ++overridden_) {
        sources_[overridden_].set_sampling_interval(kCalibrationSamplingInterval);
      }
    } catch (...) {
      Restore();
      throw;
    }
  }

  ~CalibrationSamplingCadence() { Restore(); }

  CalibrationSamplingCadence(const CalibrationSamplingCadence &) = delete;
  CalibrationSamplingCadence &operator=(const CalibrationSamplingCadence &) = delete;
  CalibrationSamplingCadence(CalibrationSamplingCadence &&) = delete;
  CalibrationSamplingCadence &operator=(CalibrationSamplingCadence &&) = delete;

 private:
  void Restore() noexcept {
    std::exception_ptr first_error;
    while (overridden_ > 0U) {
      --overridden_;
      try {
        sources_[overridden_].set_sampling_interval(previous_[overridden_]);
      } catch (...) {
        if (first_error == nullptr) {
          first_error = std::current_exception();
        }
      }
    }
    // CameraWorker accepts every positive saved interval. A callback that
    // cannot restore its own previous value violates the source contract, but
    // only after restoration has been attempted for both cameras.
    if (first_error != nullptr) {
      std::terminate();
    }
  }

  const std::array<SyntheticSwingCameraCalibrationSource, 2> &sources_;
  std::array<std::chrono::steady_clock::duration, 2> previous_{};
  std::size_t overridden_ = 0;
};

void AddLatestSamples(const std::array<SyntheticSwingCameraCalibrationSource, 2> &sources,
                      CalibrationSamples *samples) {
  for (std::size_t index = 0; index < sources.size(); ++index) {
    CalibrationSample latest = sources[index].latest_frame();
    if (latest == nullptr) {
      continue;
    }
    std::vector<CalibrationSample> &camera_samples = (*samples)[index];
    if (camera_samples.empty() ||
        camera_samples.back()->preview_sequence != latest->preview_sequence) {
      if (camera_samples.size() >= kMaximumCalibrationSamplesPerCamera) {
        throw std::runtime_error(sources[index].role +
                                 " exceeded the brightness-calibration sample limit of " +
                                 std::to_string(kMaximumCalibrationSamplesPerCamera));
      }
      camera_samples.push_back(std::move(latest));
    }
  }
}

void ValidateSampleCoverage(std::string_view role, std::span<const CalibrationSample> samples) {
  if (samples.size() < kMinimumCalibrationSamplesPerCamera) {
    throw std::runtime_error(std::string(role) + " retained only " +
                             std::to_string(samples.size()) +
                             " brightness-calibration samples; at least " +
                             std::to_string(kMinimumCalibrationSamplesPerCamera) + " are required");
  }
  for (std::size_t index = 1; index < samples.size(); ++index) {
    if (samples[index]->metadata.frame_id <= samples[index - 1U]->metadata.frame_id) {
      throw std::runtime_error(std::string(role) +
                               " brightness-calibration frame IDs are not strictly increasing");
    }
  }
  const std::uint64_t first = samples.front()->metadata.frame_id;
  const std::uint64_t last = samples.back()->metadata.frame_id;
  if (last - first == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error(std::string(role) + " brightness-calibration frame-ID span overflows");
  }
  const std::uint64_t frame_id_span = last - first + 1U;
  if (samples.size() * kMinimumFrameIdCoverageDenominator < frame_id_span) {
    throw std::runtime_error(
        std::string(role) + " retained only " + std::to_string(samples.size()) + " of " +
        std::to_string(frame_id_span) + " spanned brightness-calibration frame IDs");
  }
}

void WaitAndSample(const std::array<SyntheticSwingCameraCalibrationSource, 2> &sources,
                   CalibrationSamples *samples, const std::stop_token &stop_token,
                   std::chrono::steady_clock::time_point deadline) {
  while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    AddLatestSamples(sources, samples);
    std::this_thread::sleep_for(kCalibrationPollInterval);
  }
  if (stop_token.stop_requested()) {
    throw std::runtime_error("synthetic swing HIL stopped during brightness calibration");
  }
}

struct CalibrationCameraViews {
  std::vector<optical::BayerRg8FrameView> frames;
  std::vector<std::chrono::steady_clock::time_point> host_times;
};

CalibrationCameraViews MakeCalibrationViews(std::span<const CalibrationSample> samples) {
  CalibrationCameraViews result;
  result.frames.reserve(samples.size());
  result.host_times.reserve(samples.size());
  for (const CalibrationSample &sample : samples) {
    if (sample == nullptr) {
      throw std::invalid_argument("brightness calibration contains a null frame");
    }
    result.frames.push_back({
        .image =
            image::Raw8ImageView{
                .pixels = sample->bayer_pixels,
                .width = sample->metadata.width,
                .height = sample->metadata.height,
                .row_stride_bytes = 0,
            },
        .frame_id = sample->metadata.frame_id,
        .device_timestamp = sample->metadata.device_timestamp,
    });
    result.host_times.push_back(sample->metadata.host_received_at);
  }
  return result;
}

std::chrono::steady_clock::time_point EarliestSampleTime(
    std::span<const CalibrationSample> samples) {
  if (samples.empty()) {
    throw std::runtime_error("camera produced no preview samples during brightness calibration");
  }
  return samples.front()->metadata.host_received_at - std::chrono::milliseconds(1);
}

struct RetainedCameraViews {
  std::vector<optical::BayerRg8FrameView> frames;
  std::vector<std::chrono::steady_clock::time_point> host_times;
};

void ApplyMappedTimeCorrection(std::chrono::steady_clock::duration correction,
                               std::vector<std::chrono::steady_clock::time_point> *host_times) {
  for (auto &host_time : *host_times) {
    host_time += correction;
  }
}

RetainedCameraViews MakeRetainedViews(const application::CapturedCameraWindow &camera) {
  RetainedCameraViews result;
  result.frames.reserve(camera.frames.size());
  result.host_times.reserve(camera.frames.size());
  DeviceClockMapper mapper(camera.device_ticks_per_second);
  for (const PooledRawFrameHandle &handle : camera.frames.frames()) {
    const FrameMetadata &metadata = handle.metadata();
    mapper.AddSample(metadata.device_timestamp, metadata.host_received_at);
  }
  if (!mapper.ready()) {
    throw std::runtime_error("retained camera timeline cannot be mapped to host time");
  }
  for (const PooledRawFrameHandle &handle : camera.frames.frames()) {
    const FrameView frame = handle.view();
    const auto host_time = mapper.EstimateHostTime(frame.metadata.device_timestamp);
    if (!host_time.has_value()) {
      throw std::runtime_error("retained camera frame cannot be mapped to host time");
    }
    result.frames.push_back({
        .image =
            image::Raw8ImageView{
                .pixels = frame.payload,
                .width = frame.metadata.width,
                .height = frame.metadata.height,
                .row_stride_bytes = 0,
            },
        .frame_id = frame.metadata.frame_id,
        .device_timestamp = frame.metadata.device_timestamp,
    });
    result.host_times.push_back(*host_time);
  }
  return result;
}

std::size_t NearestFrameIndex(std::span<const std::chrono::steady_clock::time_point> host_times,
                              std::chrono::steady_clock::time_point target) {
  if (host_times.empty()) {
    throw std::invalid_argument("cannot select a frame from an empty timeline");
  }
  std::size_t best = 0;
  auto distance = std::chrono::abs(host_times.front() - target);
  for (std::size_t index = 1; index < host_times.size(); ++index) {
    const auto candidate = std::chrono::abs(host_times[index] - target);
    if (candidate < distance) {
      best = index;
      distance = candidate;
    }
  }
  return best;
}

std::string CameraFailure(std::string_view role, const optical::RgbSwingAnalysis &analysis) {
  return std::string(role) + ": " + analysis.diagnostic;
}

std::string CalibrationFailure(std::string_view role,
                               const optical::RgbBrightnessCalibration &calibration) {
  const auto correction_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 calibration.schedule_offset.mapped_time_correction)
                                 .count();
  const auto uncertainty_us =
      std::chrono::duration_cast<std::chrono::microseconds>(calibration.schedule_offset.uncertainty)
          .count();
  return std::string(role) + ": " + calibration.diagnostic +
         " correction_us=" + std::to_string(correction_us) +
         " uncertainty_us=" + std::to_string(uncertainty_us) +
         " fit_score=" + std::to_string(calibration.schedule_offset.fit_score) +
         " next_best_score=" + std::to_string(calibration.schedule_offset.next_best_score);
}

}  // namespace

std::string synthetic_swing_workflow_internal::FormatBrightnessRecommendationFailure(
    std::span<const optical::RgbBrightnessCalibration> calibrations,
    const optical::SharedBrightnessRecommendation &recommendation) {
  const auto append_reasons = [](std::ostringstream &output, std::span<const std::string> reasons) {
    output << '[';
    for (std::size_t index = 0; index < reasons.size(); ++index) {
      if (index != 0U) {
        output << '|';
      }
      output << reasons[index];
    }
    output << ']';
  };
  std::ostringstream message;
  message << "brightness calibration failed; camera_alignment=[";
  for (std::size_t index = 0; index < calibrations.size(); ++index) {
    if (index != 0U) {
      message << ';';
    }
    const optical::RgbBrightnessCalibration &calibration = calibrations[index];
    const optical::PixelRegion &region = calibration.selected_region;
    message << calibration.camera_id << "{roi=" << region.x << ',' << region.y << ','
            << region.width << ',' << region.height << ",correction_us="
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   calibration.schedule_offset.mapped_time_correction)
                   .count()
            << ",uncertainty_us="
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   calibration.schedule_offset.uncertainty)
                   .count()
            << ",fit_score=" << calibration.schedule_offset.fit_score
            << ",next_best_score=" << calibration.schedule_offset.next_best_score << '}';
  }
  message << "]; camera_candidates=[";
  bool first_candidate = true;
  for (const optical::RgbBrightnessCalibration &calibration : calibrations) {
    for (const optical::BrightnessCandidateEvidence &candidate :
         calibration.brightness_candidates) {
      if (!first_candidate) {
        message << ';';
      }
      first_candidate = false;
      message << calibration.camera_id
              << "{brightness=" << static_cast<unsigned int>(candidate.brightness)
              << ",stable_frames=" << candidate.stable_frame_count
              << ",minimum_signal=" << candidate.minimum_signal_delta
              << ",minimum_snr=" << candidate.minimum_signal_to_background_noise
              << ",maximum_saturation=" << candidate.maximum_saturated_fraction
              << ",maximum_bloom=" << candidate.maximum_bloom_fraction
              << ",safe=" << candidate.safe_for_camera << ",reasons=";
      append_reasons(message, candidate.rejection_reasons);
      message << '}';
    }
  }
  message << "]; shared_candidates=[";
  for (std::size_t index = 0; index < recommendation.candidates.size(); ++index) {
    if (index != 0U) {
      message << ';';
    }
    const optical::SharedBrightnessCandidate &candidate = recommendation.candidates[index];
    message << "{brightness=" << static_cast<unsigned int>(candidate.brightness)
            << ",cameras_observed=" << candidate.cameras_observed
            << ",safe=" << candidate.safe_for_every_camera
            << ",worst_minimum_signal=" << candidate.worst_minimum_signal_delta
            << ",worst_minimum_snr=" << candidate.worst_minimum_signal_to_background_noise
            << ",worst_maximum_saturation=" << candidate.worst_maximum_saturated_fraction
            << ",worst_maximum_bloom=" << candidate.worst_maximum_bloom_fraction << ",reasons=";
    append_reasons(message, candidate.rejection_reasons);
    message << '}';
  }
  message << "]; " << recommendation.diagnostic;
  return message.str();
}

SyntheticSwingStationWorkflow::SyntheticSwingStationWorkflow(
    std::array<SyntheticSwingCameraCalibrationSource, 2> cameras,
    SyntheticSwingStationWorkflowHooks hooks)
    : cameras_(std::move(cameras)), hooks_(std::move(hooks)) {
  ValidateSource(cameras_[0]);
  ValidateSource(cameras_[1]);
  if (cameras_[0].role == cameras_[1].role || !hooks_.calibrate_feather ||
      !hooks_.run_feather_swing) {
    throw std::invalid_argument("synthetic swing station workflow configuration is incomplete");
  }
}

std::uint8_t SyntheticSwingStationWorkflow::CalibrateBrightness(const std::stop_token &stop_token) {
  SyntheticSwingCameraAnalysisProfiles camera_profiles;
  for (std::size_t index = 0; index < cameras_.size(); ++index) {
    camera_profiles[index] = cameras_[index].analysis_profile();
  }
  if (std::ranges::any_of(camera_profiles, [](const SyntheticSwingCameraAnalysisProfile &profile) {
        return profile.exposure_duration <= std::chrono::steady_clock::duration::zero() ||
               !std::isfinite(profile.gain_decibels) || profile.gain_decibels < 0.0;
      })) {
    throw std::invalid_argument("synthetic swing camera analysis profiles are invalid");
  }
  Reset();
  CalibrationSamples samples;
  hil::FeatherCalibrationReceipt receipt;
  {
    std::future<hil::FeatherCalibrationReceipt> receipt_future;
    const CalibrationSamplingCadence calibration_sampling(cameras_);
    const auto calibration_deadline = std::chrono::steady_clock::now() + kCalibrationDeadline;
    WaitAndSample(cameras_, &samples, stop_token,
                  std::chrono::steady_clock::now() + kCalibrationBaselineDuration);

    receipt_future = std::async(std::launch::async, hooks_.calibrate_feather);
    while (receipt_future.wait_for(kCalibrationPollInterval) != std::future_status::ready) {
      if (stop_token.stop_requested()) {
        throw std::runtime_error("synthetic swing HIL stopped during brightness calibration");
      }
      if (std::chrono::steady_clock::now() >= calibration_deadline) {
        throw std::runtime_error("brightness calibration exceeded its 2-second sampling deadline");
      }
      AddLatestSamples(cameras_, &samples);
    }
    AddLatestSamples(cameras_, &samples);
    if (std::chrono::steady_clock::now() >= calibration_deadline) {
      throw std::runtime_error("brightness calibration exceeded its 2-second sampling deadline");
    }
    receipt = receipt_future.get();
    if (stop_token.stop_requested()) {
      throw std::runtime_error("synthetic swing HIL stopped during brightness calibration");
    }
  }

  std::array<optical::RgbBrightnessCalibration, 2> analyzed;
  for (std::size_t index = 0; index < cameras_.size(); ++index) {
    ValidateSampleCoverage(cameras_[index].role, samples[index]);
    CalibrationCameraViews views = MakeCalibrationViews(samples[index]);
    const auto schedule = BuildCalibrationRgbSchedule(receipt, EarliestSampleTime(samples[index]));
    optical::RgbBrightnessCalibrationOptions calibration_options;
    calibration_options.minimum_frames_per_probe = 3;
    calibration_options.optical.exposure_duration = camera_profiles[index].exposure_duration;
    analyzed[index] = optical::AnalyzeRgbBrightnessCalibration(
        {
            .camera_id = cameras_[index].role,
            .frames = views.frames,
            .mapped_host_times = views.host_times,
            .schedule = schedule,
        },
        calibration_options);
    if (!analyzed[index].located || !analyzed[index].schedule_offset.available) {
      throw std::runtime_error(CalibrationFailure(cameras_[index].role, analyzed[index]));
    }
    if (analyzed[index].schedule_offset.uncertainty > kMaximumHilScheduleUncertainty) {
      throw std::runtime_error(cameras_[index].role +
                               " schedule-offset uncertainty exceeds the 3 ms HIL bound");
    }
  }
  const std::array sweeps = {
      optical::CameraBrightnessSweep{.camera_id = cameras_[0].role,
                                     .candidates = analyzed[0].brightness_candidates},
      optical::CameraBrightnessSweep{.camera_id = cameras_[1].role,
                                     .candidates = analyzed[1].brightness_candidates},
  };
  const optical::SharedBrightnessRecommendation recommendation =
      optical::RecommendSharedRgbBrightness(sweeps);
  if (!recommendation.available || !recommendation.brightness.has_value()) {
    throw std::runtime_error(
        synthetic_swing_workflow_internal::FormatBrightnessRecommendationFailure(analyzed,
                                                                                 recommendation));
  }

  const std::uint8_t brightness = *recommendation.brightness;
  {
    const std::scoped_lock lock(mutex_);
    selected_brightness_ = brightness;
    calibrations_[0] = std::move(analyzed[0]);
    calibrations_[1] = std::move(analyzed[1]);
    camera_profiles_ = camera_profiles;
  }
  return brightness;
}

void SyntheticSwingStationWorkflow::RunStimulus(std::uint8_t brightness,
                                                const std::stop_token &stop_token) {
  {
    const std::scoped_lock lock(mutex_);
    if (!selected_brightness_.has_value() || *selected_brightness_ != brightness) {
      throw std::logic_error("synthetic swing brightness does not match calibration");
    }
  }
  if (stop_token.stop_requested()) {
    throw std::runtime_error("synthetic swing HIL stopped before stimulus");
  }
  hil::FeatherSwingReceipt receipt = hooks_.run_feather_swing(brightness);
  {
    const std::scoped_lock lock(mutex_);
    swing_receipt_ = std::move(receipt);
    capture_pending_ = true;
  }
  receipt_ready_.notify_all();
}

std::optional<application::SyntheticSwingSessionEvidence>
SyntheticSwingStationWorkflow::AnalyzeCapturedSession(
    const application::CapturedSession &captured) {
  std::array<optical::RgbBrightnessCalibration, 2> calibrations;
  SyntheticSwingCameraAnalysisProfiles camera_profiles;
  hil::FeatherSwingReceipt receipt;
  std::uint8_t brightness = 0;
  {
    std::unique_lock lock(mutex_);
    if (!selected_brightness_.has_value() || !capture_pending_) {
      return std::nullopt;
    }
    if (!receipt_ready_.wait_for(lock, kSwingReceiptWait,
                                 [this] { return swing_receipt_.has_value(); })) {
      throw std::runtime_error("timed out waiting for the Feather SWING receipt");
    }
    if (!calibrations_[0].has_value() || !calibrations_[1].has_value()) {
      throw std::logic_error("synthetic swing calibration evidence is unavailable");
    }
    if (!camera_profiles_.has_value()) {
      throw std::logic_error("synthetic swing camera profile evidence is unavailable");
    }
    brightness = *selected_brightness_;
    receipt = RequireValue(swing_receipt_, "synthetic swing receipt is unavailable");
    calibrations = {
        RequireValue(calibrations_[0], "down-the-line calibration is unavailable"),
        RequireValue(calibrations_[1], "face-on calibration is unavailable"),
    };
    camera_profiles = *camera_profiles_;
    capture_pending_ = false;
  }

  application::SyntheticSwingSessionEvidence evidence{
      .selected_brightness = brightness,
      .step_duration_us = SWING_HIL_SWING_STEP_US,
      .pre_impact_step_count = SWING_HIL_SWING_PRE_STEPS,
      .white_impact_duration_us = SWING_HIL_SWING_IMPACT_WHITE_US,
      .post_impact_step_count = SWING_HIL_SWING_POST_STEPS,
      .tone_duration_us = SWING_HIL_SWING_TONE_DURATION_US,
      .tone_frequency_hz = SWING_HIL_SWING_TONE_FREQUENCY_HZ,
      .cameras = {},
  };
  bool all_passed = true;
  std::string failure;
  for (std::size_t index = 0; index < captured.cameras.size(); ++index) {
    const application::CapturedCameraWindow &camera = captured.cameras[index];
    const auto *const calibration =
        std::ranges::find(calibrations, camera.role, &optical::RgbBrightnessCalibration::camera_id);
    if (calibration == calibrations.end()) {
      throw std::logic_error("captured camera has no synthetic swing calibration");
    }
    RetainedCameraViews views = MakeRetainedViews(camera);
    ApplyMappedTimeCorrection(calibration->schedule_offset.mapped_time_correction,
                              &views.host_times);
    const auto schedule = BuildSyntheticSwingRgbSchedule(
        receipt, views.host_times.front() - std::chrono::milliseconds(1));
    optical::RgbSwingAnalysisOptions analysis_options;
    const auto calibration_index = static_cast<std::size_t>(calibration - calibrations.data());
    analysis_options.exposure_duration = camera_profiles[calibration_index].exposure_duration;
    analysis_options.schedule_timing_uncertainty = calibration->schedule_offset.uncertainty;
    const optical::RgbSwingAnalysis analysis = optical::AnalyzeRgbWhiteImpact(
        {
            .camera_id = camera.role,
            .frames = views.frames,
            .mapped_host_times = views.host_times,
            .fixture_neopixel_region = calibration->selected_region,
            .schedule = schedule,
            .color_calibration = calibration->color_calibration,
        },
        analysis_options);
    const auto impact_step = std::ranges::find(schedule, optical::RgbSwingPhase::kImpact,
                                               &optical::FeatherRgbStep::phase);
    if (impact_step == schedule.end()) {
      throw std::logic_error("synthetic swing schedule has no impact step");
    }
    const auto impact_evidence = std::ranges::find(analysis.states, optical::RgbSwingPhase::kImpact,
                                                   &optical::RgbStateEvidence::phase);
    if (impact_evidence == analysis.states.end()) {
      throw std::logic_error("synthetic swing analysis has no impact evidence");
    }
    const std::size_t impact_index = analysis.first_impact_frame_index.value_or(
        NearestFrameIndex(views.host_times, impact_step->start));
    evidence.cameras[index] = {
        .role = camera.role,
        .optical_white_impact_frame_id = views.frames[impact_index].frame_id,
        .mapped_time_correction_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                         calibration->schedule_offset.mapped_time_correction)
                                         .count(),
        .schedule_uncertainty_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           calibration->schedule_offset.uncertainty)
                                           .count()),
        .optical_white_passed = analysis.passed,
        .stable_frame_count = impact_evidence->stable_frame_count,
        .matching_frame_count = impact_evidence->matching_frame_count,
        .matching_fraction = impact_evidence->matching_fraction,
        .mean_signal_delta = impact_evidence->mean_signal_delta,
        .mean_expected_color_distance = impact_evidence->mean_expected_color_distance,
        .maximum_saturated_fraction = impact_evidence->maximum_saturated_fraction,
        .maximum_bloom_fraction = impact_evidence->maximum_bloom_fraction,
        .exposure_us = std::chrono::duration<double, std::micro>(
                           camera_profiles[calibration_index].exposure_duration)
                           .count(),
        .gain_db = camera_profiles[calibration_index].gain_decibels,
    };
    if (!analysis.passed) {
      if (!failure.empty()) {
        failure += "; ";
      }
      failure += CameraFailure(camera.role, analysis);
      all_passed = false;
    }
  }
  {
    const std::scoped_lock lock(mutex_);
    analyzed_session_id_ = captured.identity.session_id;
    analyzed_optical_passed_ = all_passed;
    analysis_diagnostic_ = std::move(failure);
  }
  return evidence;
}

void SyntheticSwingStationWorkflow::ValidatePublishedSession(std::string_view session_id) const {
  const std::scoped_lock lock(mutex_);
  if (!analyzed_session_id_.has_value() || *analyzed_session_id_ != session_id) {
    throw std::runtime_error("published session has no matching synthetic swing analysis");
  }
  if (!analyzed_optical_passed_) {
    throw std::runtime_error(analysis_diagnostic_.empty()
                                 ? "synthetic swing white-impact validation failed"
                                 : analysis_diagnostic_);
  }
}

void SyntheticSwingStationWorkflow::Reset() noexcept {
  const std::scoped_lock lock(mutex_);
  selected_brightness_.reset();
  calibrations_ = {};
  camera_profiles_.reset();
  swing_receipt_.reset();
  capture_pending_ = false;
  analyzed_session_id_.reset();
  analyzed_optical_passed_ = false;
  analysis_diagnostic_.clear();
}

}  // namespace swing_capture::service
