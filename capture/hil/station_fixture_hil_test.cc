#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "capture/audio/audio_capture_session.h"
#include "capture/audio/commanded_tone_analyzer.h"
#include "capture/audio/pcm_wav.h"
#include "capture/core/camera_source.h"
#include "capture/core/device_clock_mapper.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/core/robust_device_clock_mapping.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/hil/feather_hil_controller.h"
#include "capture/hil/feather_hil_serial.h"
#include "capture/image/bayer_rg8.h"
#include "capture/image/image_quality.h"
#include "capture/optical/april_tag.h"
#include "capture/optical/frame_selection.h"
#include "capture/optical/led_pulse.h"
#include "capture/optical/led_schedule_association.h"
#include "station/hardware_lock.h"
#include "station/station_config.h"

namespace {

using json = nlohmann::json;
using swing_capture::AnalyzeCommandedTone;
using swing_capture::ArecordPcmConfig;
using swing_capture::AudioCaptureResult;
using swing_capture::AudioCaptureSession;
using swing_capture::AudioCaptureWaitResult;
using swing_capture::CommandedToneEvaluation;
using swing_capture::CommandedToneInput;
using swing_capture::CommandedToneStimulus;
using swing_capture::CommandedToneThresholds;
using swing_capture::DeviceClockMapper;
using swing_capture::DeviceHostTimestampSample;
using swing_capture::EncodeMonoPcmS16Wav;
using swing_capture::FitRobustDeviceClockMapping;
using swing_capture::FrameView;
using swing_capture::MakeArecordAudioCaptureSourceFactory;
using swing_capture::PooledRawFrameHandle;
using swing_capture::PooledRawFramePushResult;
using swing_capture::PooledRawFrameRing;
using swing_capture::PooledRawFrameRingConfig;
using swing_capture::PooledRawFrameSnapshot;
using swing_capture::RobustDeviceClockMappingDiagnostics;
using swing_capture::RobustDeviceClockMappingResult;
using swing_capture::daheng::DahengCamera;
using swing_capture::daheng::DahengConfiguration;
using swing_capture::daheng::DahengDiagnostics;
using swing_capture::daheng::GalaxySdk;
using swing_capture::hil::FeatherDeviceInfo;
using swing_capture::hil::FeatherHilController;
using swing_capture::hil::FeatherHilFailureEvidence;
using swing_capture::hil::FeatherHilSerial;
using swing_capture::hil::FeatherHilTransactionError;
using swing_capture::hil::FeatherHilTransactionStageName;
using swing_capture::hil::FeatherStimulusReceipt;
using swing_capture::image::DemosaicBayerRg8;
using swing_capture::image::EncodePng;
using swing_capture::image::ImageQualityMetrics;
using swing_capture::image::MeasureRaw8ImageQuality;
using swing_capture::image::Raw8ImageView;
using swing_capture::image::Rgb8ToLuminance;
using swing_capture::optical::AnalyzeLedPulse;
using swing_capture::optical::AprilTagDetection;
using swing_capture::optical::AprilTagDetector;
using swing_capture::optical::AprilTagDetectorOptions;
using swing_capture::optical::AprilTagFamily;
using swing_capture::optical::AprilTagPresenceResult;
using swing_capture::optical::BayerRg8FrameView;
using swing_capture::optical::EstimateStimulusHostSchedule;
using swing_capture::optical::EvaluateLedScheduleAssociation;
using swing_capture::optical::FeatherAckScheduleBracket;
using swing_capture::optical::LedAnalysisFrameSelection;
using swing_capture::optical::LedPulseOptions;
using swing_capture::optical::LedPulseResult;
using swing_capture::optical::LedScheduleAssociationOptions;
using swing_capture::optical::LedScheduleAssociationResult;
using swing_capture::optical::MappedTagTimelineView;
using swing_capture::optical::SelectAcceptedLedPeakFrame;
using swing_capture::optical::SelectLedAnalysisFrames;
using swing_capture::optical::SelectTagRepresentativeFrame;
using swing_capture::optical::TagRepresentativeFrameSelection;
using swing_capture::optical::VerifyAprilTagPresence;
using swing_capture::station::HardwareLock;
using swing_capture::station::StationConfig;

constexpr std::uint32_t kAudioSampleRateHz = 32000;
constexpr std::uint64_t kAudioBaselineSamples = kAudioSampleRateHz * 300U / 1000U;
constexpr std::uint64_t kAudioPostCommandSamples = kAudioSampleRateHz * 750U / 1000U;
// Covers the nominal 1.264 s sequence plus the controller's bounded serial
// delays and sequential camera shutdown without overwriting retained evidence.
constexpr std::size_t kCameraRingFrames = 512;
constexpr auto kBaselineDuration = std::chrono::milliseconds(300);
constexpr auto kLedLead = std::chrono::microseconds(100000);
constexpr auto kLedDuration = std::chrono::microseconds(44053);
constexpr auto kBetweenStimuli = std::chrono::milliseconds(100);
constexpr auto kToneLead = std::chrono::microseconds(100000);
constexpr auto kToneDuration = std::chrono::microseconds(20000);
constexpr std::uint32_t kToneFrequencyHz = 2000;
constexpr std::uint32_t kToneLevelPermille = 10;
constexpr auto kCameraPostStimulus = std::chrono::milliseconds(600);

void RequireAudioWait(const AudioCaptureWaitResult &result, std::string_view operation) {
  if (result.condition_satisfied && result.error.empty()) {
    return;
  }
  std::string message(operation);
  message += result.deadline_expired ? " timed out" : " failed";
  if (!result.error.empty()) {
    message += ": " + result.error;
  }
  throw std::runtime_error(message);
}

std::filesystem::path OutputDirectory() {
  // Bazel supplies this before any worker thread starts.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *directory = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  return directory == nullptr || *directory == '\0' ? std::filesystem::current_path()
                                                    : std::filesystem::path(directory);
}

std::filesystem::path RequiredStationPath() {
  const std::optional<std::filesystem::path> path =
      swing_capture::station::StationConfigPathFromEnvironment();
  if (!path.has_value()) {
    throw std::runtime_error(
        "SWING_CAPTURE_STATION_CONFIG is not set; configure it in .bazelrc.local");
  }
  return std::filesystem::absolute(*path);
}

std::filesystem::path HardwareLockPath(const std::filesystem::path &station_path) {
  // Deployment overrides are read before worker threads start.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *override_path = std::getenv("SWING_CAPTURE_HARDWARE_LOCK");
  if (override_path != nullptr && *override_path != '\0') {
    return std::filesystem::absolute(override_path);
  }
  return station_path.parent_path() / "artifacts" / "hil" / "hardware.lock";
}

void WriteBytes(const std::filesystem::path &path, std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open HIL artifact: " + path.string());
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),  // NOLINT
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write HIL artifact: " + path.string());
  }
}

void WriteText(const std::filesystem::path &path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open HIL artifact: " + path.string());
  }
  output << text;
  if (!output) {
    throw std::runtime_error("cannot write HIL artifact: " + path.string());
  }
}

json FeatherFailureJson(const FeatherHilFailureEvidence &evidence);

json BaseReport() {
  return {
      {"schema_version", 1},
      {"passed", false},
      {"scope", "dual_camera_led_apriltag_and_speaker_microphone"},
      {"state", "initializing"},
  };
}

void WriteReport(const std::filesystem::path &output_directory, const json &report) {
  WriteText(output_directory / "station-fixture-report.json", report.dump(2) + "\n");
}

std::string ExceptionMessage(const std::exception_ptr &failure) {
  if (failure == nullptr) {
    return {};
  }
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception &error) {
    return error.what();
  } catch (...) {
    return "unknown exception";
  }
}

void PreserveFatalReport(const std::filesystem::path &output_directory, std::string_view error,
                         const FeatherHilFailureEvidence *feather_failure = nullptr) {
  const std::filesystem::path report_path = output_directory / "station-fixture-report.json";
  json report = BaseReport();
  std::ifstream existing(report_path);
  if (existing) {
    try {
      json parsed;
      existing >> parsed;
      if (parsed.is_object()) {
        report = std::move(parsed);
      }
    } catch (const json::exception &) {
      report["prior_report_parse_error"] = true;
    }
  }
  report["passed"] = false;
  report["state"] = "failed";
  report["fatal_error"] = error;
  if (feather_failure != nullptr) {
    report["feather"]["transaction_failure"] = FeatherFailureJson(*feather_failure);
  }
  WriteReport(output_directory, report);
}

class CameraCapture final {
 public:
  CameraCapture(GalaxySdk &sdk, std::string role, std::string serial)
      : role_(std::move(role)), serial_(std::move(serial)), camera_(sdk, serial_) {
    camera_.Configure(DahengConfiguration{});
    diagnostics_ = camera_.diagnostics();
    clock_mapper_ = std::make_unique<DeviceClockMapper>(diagnostics_.timestamp_ticks_per_second);
    ring_ = std::make_unique<PooledRawFrameRing>(PooledRawFrameRingConfig{
        .active_frame_capacity = kCameraRingFrames,
        .reserve_frame_blocks = 0,
        .maximum_payload_bytes = static_cast<std::size_t>(diagnostics_.payload_bytes),
    });
  }

  ~CameraCapture() { StopNoThrow(); }

  CameraCapture(const CameraCapture &) = delete;
  CameraCapture &operator=(const CameraCapture &) = delete;
  CameraCapture(CameraCapture &&) = delete;
  CameraCapture &operator=(CameraCapture &&) = delete;

  void Start() {
    camera_.Start();
    try {
      thread_ = std::jthread([this](const std::stop_token &stop_token) { Capture(stop_token); });
      started_ = true;
    } catch (...) {
      camera_.Stop();
      throw;
    }
  }

  void Stop() {
    if (!started_) {
      return;
    }
    RequestCaptureStop();
    JoinCapture();
    started_ = false;
    camera_.Stop();
    const std::scoped_lock lock(error_mutex_);
    if (!error_.empty()) {
      throw std::runtime_error(role_ + " camera capture failed: " + error_);
    }
  }

  void RequestCaptureStop() {
    if (started_) {
      thread_.request_stop();
    }
  }

  void JoinCapture() {
    if (started_ && thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] PooledRawFrameSnapshot Freeze() const { return ring_->Freeze(); }
  [[nodiscard]] const std::string &role() const { return role_; }
  [[nodiscard]] const std::string &serial() const { return serial_; }
  [[nodiscard]] const DahengDiagnostics &diagnostics() const { return diagnostics_; }
  [[nodiscard]] const DeviceClockMapper &clock_mapper() const { return *clock_mapper_; }
  [[nodiscard]] std::uint64_t captured_frames() const { return captured_frames_.load(); }
  [[nodiscard]] std::uint64_t timeouts() const { return timeouts_.load(); }
  [[nodiscard]] std::uint64_t frame_id_gaps() const { return frame_id_gaps_.load(); }
  [[nodiscard]] std::size_t ring_allocated_bytes() const { return ring_->allocated_bytes(); }
  [[nodiscard]] std::uint64_t payload_bytes() const { return payload_bytes_; }
  [[nodiscard]] double measured_host_frames_per_second() const {
    if (captured_frames() < 2) {
      return 0.0;
    }
    const double seconds =
        std::chrono::duration<double>(last_host_time_ - first_host_time_).count();
    return seconds > 0.0 ? static_cast<double>(captured_frames() - 1) / seconds : 0.0;
  }
  [[nodiscard]] double measured_device_frames_per_second() const {
    if (captured_frames() < 2 || last_device_timestamp_ <= first_device_timestamp_) {
      return 0.0;
    }
    const double seconds = static_cast<double>(last_device_timestamp_ - first_device_timestamp_) /
                           static_cast<double>(diagnostics_.timestamp_ticks_per_second);
    return seconds > 0.0 ? static_cast<double>(captured_frames() - 1) / seconds : 0.0;
  }
  [[nodiscard]] double maximum_host_interval_seconds() const {
    return std::chrono::duration<double>(maximum_host_interval_).count();
  }
  [[nodiscard]] double maximum_device_interval_seconds() const {
    return static_cast<double>(maximum_device_interval_ticks_) /
           static_cast<double>(diagnostics_.timestamp_ticks_per_second);
  }
  [[nodiscard]] std::uint64_t maximum_host_interval_previous_frame_id() const {
    return maximum_host_interval_previous_frame_id_;
  }
  [[nodiscard]] std::uint64_t maximum_host_interval_frame_id() const {
    return maximum_host_interval_frame_id_;
  }
  [[nodiscard]] std::chrono::steady_clock::time_point first_timeout_at() const {
    return first_timeout_at_;
  }
  [[nodiscard]] std::uint64_t first_timeout_after_frame_id() const {
    return first_timeout_after_frame_id_;
  }
  [[nodiscard]] std::string capture_error() const {
    const std::scoped_lock lock(error_mutex_);
    return error_;
  }

 private:
  void SetError(std::string message) {
    const std::scoped_lock lock(error_mutex_);
    if (error_.empty()) {
      error_ = std::move(message);
    }
  }

  void Capture(const std::stop_token &stop_token) {
    std::uint64_t previous_frame_id = 0;
    std::uint64_t previous_device_timestamp = 0;
    std::chrono::steady_clock::time_point previous_host_time;
    try {
      while (!stop_token.stop_requested()) {
        const bool received =
            camera_.CaptureOne(std::chrono::milliseconds(50), [&](const FrameView &frame) {
              const std::size_t expected_payload =
                  static_cast<std::size_t>(diagnostics_.width) * diagnostics_.height;
              if (!frame.metadata.complete || frame.metadata.width != diagnostics_.width ||
                  frame.metadata.height != diagnostics_.height ||
                  frame.payload.size() != expected_payload) {
                SetError("camera returned incomplete or malformed BayerRG8 frame");
                return;
              }
              if (previous_frame_id != 0 && frame.metadata.frame_id != previous_frame_id + 1) {
                if (frame.metadata.frame_id <= previous_frame_id) {
                  SetError("camera frame IDs are not strictly increasing");
                  return;
                }
                frame_id_gaps_.fetch_add(frame.metadata.frame_id - previous_frame_id - 1);
              }
              if (previous_frame_id == 0) {
                first_device_timestamp_ = frame.metadata.device_timestamp;
                first_host_time_ = frame.metadata.host_received_at;
              } else {
                if (frame.metadata.device_timestamp <= previous_device_timestamp) {
                  SetError("camera device timestamps are not strictly increasing");
                  return;
                }
                if (frame.metadata.host_received_at <= previous_host_time) {
                  SetError("camera host receipt timestamps are not strictly increasing");
                  return;
                }
                maximum_device_interval_ticks_ =
                    std::max(maximum_device_interval_ticks_,
                             frame.metadata.device_timestamp - previous_device_timestamp);
                const auto host_interval = frame.metadata.host_received_at - previous_host_time;
                if (host_interval > maximum_host_interval_) {
                  maximum_host_interval_ = host_interval;
                  maximum_host_interval_previous_frame_id_ = previous_frame_id;
                  maximum_host_interval_frame_id_ = frame.metadata.frame_id;
                }
              }
              previous_frame_id = frame.metadata.frame_id;
              previous_device_timestamp = frame.metadata.device_timestamp;
              previous_host_time = frame.metadata.host_received_at;
              last_device_timestamp_ = frame.metadata.device_timestamp;
              last_host_time_ = frame.metadata.host_received_at;
              clock_mapper_->AddSample(frame.metadata.device_timestamp,
                                       frame.metadata.host_received_at);
              const PooledRawFramePushResult result = ring_->TryPush(frame);
              if (result != PooledRawFramePushResult::kStored) {
                SetError("raw frame ring rejected a frame with result " +
                         std::to_string(static_cast<int>(result)));
                return;
              }
              payload_bytes_ += frame.payload.size();
              captured_frames_.fetch_add(1);
            });
        if (!received && !stop_token.stop_requested()) {
          if (timeouts_.fetch_add(1) == 0) {
            first_timeout_at_ = std::chrono::steady_clock::now();
            first_timeout_after_frame_id_ = previous_frame_id;
          }
        }
        {
          const std::scoped_lock lock(error_mutex_);
          if (!error_.empty()) {
            return;
          }
        }
      }
    } catch (const std::exception &error) {
      SetError(error.what());
    }
  }

  void StopNoThrow() noexcept {
    if (!started_) {
      return;
    }
    thread_.request_stop();
    if (thread_.joinable()) {
      thread_.join();
    }
    try {
      camera_.Stop();
    } catch (...) {
    }
    started_ = false;
  }

  std::string role_;
  std::string serial_;
  DahengCamera camera_;
  DahengDiagnostics diagnostics_;
  std::unique_ptr<DeviceClockMapper> clock_mapper_;
  std::unique_ptr<PooledRawFrameRing> ring_;
  std::jthread thread_;
  std::atomic<std::uint64_t> captured_frames_ = 0;
  std::atomic<std::uint64_t> timeouts_ = 0;
  std::atomic<std::uint64_t> frame_id_gaps_ = 0;
  std::uint64_t payload_bytes_ = 0;
  std::uint64_t first_device_timestamp_ = 0;
  std::uint64_t last_device_timestamp_ = 0;
  std::uint64_t maximum_device_interval_ticks_ = 0;
  std::chrono::steady_clock::time_point first_host_time_;
  std::chrono::steady_clock::time_point last_host_time_;
  std::chrono::steady_clock::duration maximum_host_interval_{};
  std::uint64_t maximum_host_interval_previous_frame_id_ = 0;
  std::uint64_t maximum_host_interval_frame_id_ = 0;
  std::chrono::steady_clock::time_point first_timeout_at_;
  std::uint64_t first_timeout_after_frame_id_ = 0;
  mutable std::mutex error_mutex_;
  std::string error_;
  bool started_ = false;
};

std::vector<BayerRg8FrameView> OpticalViews(const PooledRawFrameSnapshot &snapshot) {
  std::vector<BayerRg8FrameView> views;
  views.reserve(snapshot.size());
  for (const auto &frame : snapshot.frames()) {
    views.push_back({
        .image = {.pixels = frame.payload(),
                  .width = frame.metadata().width,
                  .height = frame.metadata().height,
                  .row_stride_bytes = frame.metadata().width},
        .frame_id = frame.metadata().frame_id,
        .device_timestamp = frame.metadata().device_timestamp,
    });
  }
  return views;
}

std::vector<DeviceHostTimestampSample> DeviceHostTimestampSamples(
    const PooledRawFrameSnapshot &snapshot) {
  std::vector<DeviceHostTimestampSample> samples;
  samples.reserve(snapshot.size());
  for (const PooledRawFrameHandle &frame : snapshot.frames()) {
    samples.push_back({
        .device_timestamp = frame.metadata().device_timestamp,
        .host_received_at = frame.metadata().host_received_at,
    });
  }
  return samples;
}

struct TagAttempt {
  AprilTagFamily family = AprilTagFamily::kTag36h11;
  AprilTagPresenceResult presence;
};

struct TagResult {
  std::vector<TagAttempt> attempts;
  std::optional<AprilTagDetection> accepted_detection;
};

TagResult DetectPrintedTag(const Raw8ImageView &frame) {
  TagResult result;
  AprilTagDetector tag36({.family = AprilTagFamily::kTag36h11});
  AprilTagPresenceResult presence = VerifyAprilTagPresence(frame, tag36);
  result.attempts.push_back({.family = AprilTagFamily::kTag36h11, .presence = presence});
  if (presence.present) {
    result.accepted_detection = std::move(presence.accepted_detection);
    return result;
  }
  AprilTagDetector tag16({.family = AprilTagFamily::kTag16h5});
  presence = VerifyAprilTagPresence(frame, tag16);
  result.attempts.push_back({.family = AprilTagFamily::kTag16h5, .presence = presence});
  if (presence.present) {
    result.accepted_detection = std::move(presence.accepted_detection);
    return result;
  }
  AprilTagDetector standard41({.family = AprilTagFamily::kTagStandard41h12});
  presence = VerifyAprilTagPresence(frame, standard41);
  result.attempts.push_back({.family = AprilTagFamily::kTagStandard41h12, .presence = presence});
  if (presence.present) {
    result.accepted_detection = std::move(presence.accepted_detection);
  }
  return result;
}

std::int64_t SteadyNanoseconds(std::chrono::steady_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

json ClockMapperJson(const DeviceClockMapper &mapper) {
  return {
      {"ready", mapper.ready()},
      {"sample_count", mapper.sample_count()},
      {"rate_ratio", mapper.clock_rate_ratio()},
      {"drift_parts_per_million", mapper.clock_drift_parts_per_million()},
      {"residual_standard_deviation_seconds", mapper.residual_standard_deviation_seconds()},
  };
}

json RobustClockMappingJson(const RobustDeviceClockMappingDiagnostics &diagnostics,
                            std::span<const std::size_t> outlier_indices) {
  json outliers = json::array();
  for (const std::size_t index : outlier_indices) {
    outliers.push_back(index);
  }
  return {
      {"timestamp_model", diagnostics.timestamp_model},
      {"sample_count", diagnostics.sample_count},
      {"slope_pair_count", diagnostics.slope_pair_count},
      {"inlier_count", diagnostics.inlier_count},
      {"outlier_count", diagnostics.outlier_count},
      {"outlier_sample_indices", std::move(outliers)},
      {"clock_rate_ratio", diagnostics.clock_rate_ratio},
      {"clock_drift_parts_per_million", diagnostics.clock_drift_parts_per_million},
      {"robust_slope_sigma", diagnostics.robust_slope_sigma},
      {"median_absolute_residual_seconds", diagnostics.median_absolute_residual.count()},
      {"robust_residual_sigma_seconds", diagnostics.robust_residual_sigma.count()},
      {"maximum_inlier_absolute_residual_seconds",
       diagnostics.maximum_inlier_absolute_residual.count()},
      {"maximum_positive_queue_delay_seconds", diagnostics.maximum_positive_queue_delay.count()},
      {"receipt_time_uncertainty_seconds", diagnostics.receipt_time_uncertainty.count()},
      {"fixed_transport_latency_upper_bound_seconds",
       diagnostics.fixed_transport_latency_upper_bound.has_value()
           ? json(diagnostics.fixed_transport_latency_upper_bound->count())
           : json(nullptr)},
  };
}

json ReceiptJson(const FeatherStimulusReceipt &receipt) {
  return {
      {"request_id", receipt.request_id},
      {"subject", receipt.subject},
      {"requested_lead_us", receipt.requested_lead_microseconds},
      {"requested_duration_us", receipt.requested_duration_microseconds},
      {"frequency_hz", receipt.frequency_hz},
      {"level_permille", receipt.level_permille},
      {"sample_rate_hz", receipt.sample_rate_hz},
      {"sample_count", receipt.sample_count},
      {"quantized_duration_us", receipt.quantized_duration_microseconds},
      {"accepted_device_us", receipt.accepted_device_microseconds},
      {"scheduled_device_us", receipt.scheduled_device_microseconds},
      {"start_device_us", receipt.start_device_microseconds},
      {"start_lateness_us", receipt.start_lateness_microseconds},
      {"end_device_us", receipt.end_device_microseconds},
      {"elapsed_device_us", receipt.elapsed_device_microseconds},
      {"host",
       {{"write_started_ns", SteadyNanoseconds(receipt.host_command_write_started)},
        {"sent_ns", SteadyNanoseconds(receipt.host_command_sent)},
        {"acknowledgement_received_ns", SteadyNanoseconds(receipt.host_acknowledgement_received)},
        {"start_record_received_ns", SteadyNanoseconds(receipt.host_start_received)},
        {"done_record_received_ns", SteadyNanoseconds(receipt.host_done_received)}}},
      {"wire",
       {{"acknowledgement", receipt.acknowledgement.wire_line},
        {"start", receipt.started.wire_line},
        {"done", receipt.done.wire_line}}},
  };
}

json DeviceInfoJson(const FeatherDeviceInfo &info) {
  return {
      {"firmware", info.firmware},
      {"protocol_version", info.protocol_version},
      {"capabilities", info.capabilities},
      {"maximum_lead_us", info.maximum_lead_microseconds},
      {"led_duration_min_us", info.led_minimum_duration_microseconds},
      {"led_duration_max_us", info.led_maximum_duration_microseconds},
      {"tone_lead_min_us", info.tone_minimum_lead_microseconds},
      {"tone_duration_min_us", info.tone_minimum_duration_microseconds},
      {"tone_duration_max_us", info.tone_maximum_duration_microseconds},
      {"tone_frequency_min_hz", info.tone_minimum_frequency_hz},
      {"tone_frequency_max_hz", info.tone_maximum_frequency_hz},
      {"tone_level_min_permille", info.tone_minimum_level_permille},
      {"tone_level_max_permille", info.tone_maximum_level_permille},
      {"device_us", info.device_microseconds},
      {"host",
       {{"write_started_ns", SteadyNanoseconds(info.host_query_write_started)},
        {"sent_ns", SteadyNanoseconds(info.host_query_sent)},
        {"response_received_ns", SteadyNanoseconds(info.host_response_received)}}},
      {"wire", info.response.wire_line},
      {"fields", info.fields},
  };
}

json FeatherFailureJson(const FeatherHilFailureEvidence &evidence) {
  json result = {
      {"request_id", evidence.request_id},
      {"subject", evidence.subject},
      {"stage", FeatherHilTransactionStageName(evidence.stage)},
      {"command_wire", evidence.command_wire},
  };
  if (evidence.device_info.has_value()) {
    result["partial_device_info"] = DeviceInfoJson(*evidence.device_info);
  }
  if (evidence.stimulus_receipt.has_value()) {
    result["partial_stimulus_receipt"] = ReceiptJson(*evidence.stimulus_receipt);
  }
  if (evidence.offending_response.has_value()) {
    result["offending_response"] = {
        {"request_id", evidence.offending_response->request_id},
        {"subject", evidence.offending_response->subject},
        {"state", evidence.offending_response->state},
        {"fields", evidence.offending_response->fields},
        {"wire", evidence.offending_response->wire_line},
    };
  }
  return result;
}

json TagDetectionJson(const AprilTagDetection &tag) {
  json corners = json::array();
  for (const auto &corner : tag.corners) {
    corners.push_back({{"x", corner.x}, {"y", corner.y}});
  }
  return {
      {"family", tag.family},
      {"id", tag.id},
      {"hamming", tag.hamming},
      {"decision_margin", tag.decision_margin},
      {"center", {{"x", tag.center.x}, {"y", tag.center.y}}},
      {"corners", std::move(corners)},
  };
}

json TagJson(const TagResult &result) {
  json attempts = json::array();
  for (const TagAttempt &attempt : result.attempts) {
    json detections = json::array();
    for (const AprilTagDetection &detection : attempt.presence.detections) {
      detections.push_back(TagDetectionJson(detection));
    }
    attempts.push_back({
        {"family", swing_capture::optical::AprilTagFamilyName(attempt.family)},
        {"present", attempt.presence.present},
        {"diagnostic", attempt.presence.diagnostic},
        {"detections", std::move(detections)},
    });
  }
  json output = {
      {"present", result.accepted_detection.has_value()},
      {"bootstrap_family_fallback_enabled", true},
      {"bootstrap_family_fallback_used", result.attempts.size() > 1},
      {"criteria", {{"maximum_hamming", 1}, {"minimum_decision_margin", 10.0}}},
      {"attempts", std::move(attempts)},
  };
  if (result.accepted_detection.has_value()) {
    output["accepted"] = TagDetectionJson(*result.accepted_detection);
  }
  return output;
}

json LedJson(const LedPulseResult &result, const LedPulseOptions &options) {
  json frames = json::array();
  for (const auto &frame : result.frames) {
    frames.push_back({
        {"frame_id", frame.frame_id},
        {"device_timestamp", frame.device_timestamp},
        {"changed_red_samples", frame.changed_red_samples},
        {"red_samples", frame.red_samples},
        {"local_changed_red_fraction", frame.local_changed_red_fraction},
        {"background_changed_red_fraction", frame.background_changed_red_fraction},
        {"changed_red_fraction", frame.changed_red_fraction},
        {"local_mean_positive_red_delta", frame.local_mean_positive_red_delta},
        {"background_mean_positive_red_delta", frame.background_mean_positive_red_delta},
        {"mean_positive_red_delta", frame.mean_positive_red_delta},
        {"global_illumination_scale", frame.global_illumination_scale},
        {"global_illumination_offset", frame.global_illumination_offset},
        {"baseline", frame.baseline_frame},
        {"active", frame.active},
    });
  }
  return {
      {"detected", result.detected},
      {"diagnostic", result.diagnostic},
      {"options",
       {{"baseline_frame_count", options.baseline_frame_count},
        {"region_width", options.region_width},
        {"region_height", options.region_height},
        {"region_stride", options.region_stride},
        {"background_margin", options.background_margin},
        {"minimum_red_sample_delta", options.minimum_red_sample_delta},
        {"minimum_changed_red_samples", options.minimum_changed_red_samples},
        {"minimum_changed_red_fraction", options.minimum_changed_red_fraction},
        {"minimum_mean_positive_red_delta", options.minimum_mean_positive_red_delta},
        {"minimum_active_frames", options.minimum_active_frames},
        {"maximum_pulse_span_frames", options.maximum_pulse_span_frames},
        {"maximum_internal_inactive_frames", options.maximum_internal_inactive_frames}}},
      {"selected_region",
       {{"x", result.selected_region.x},
        {"y", result.selected_region.y},
        {"width", result.selected_region.width},
        {"height", result.selected_region.height}}},
      {"pulse_start_frame_index", result.pulse_start_frame_index},
      {"pulse_end_frame_index", result.pulse_end_frame_index},
      {"active_frame_count", result.pulse_active_frame_count},
      {"span_frame_count", result.pulse_span_frame_count},
      {"start_device_timestamp", result.start_device_timestamp},
      {"end_device_timestamp_exclusive", result.end_device_timestamp_exclusive},
      {"nominal_frame_interval_ticks", result.nominal_frame_interval_ticks},
      {"duration_ticks", result.duration_ticks},
      {"duration_seconds", result.duration_seconds},
      {"missing_frame_ids", result.missing_frame_ids},
      {"frames", std::move(frames)},
  };
}

json LedScheduleAssociationJson(const LedScheduleAssociationResult &association) {
  json checks = json::array();
  for (const auto &check : association.checks) {
    checks.push_back({{"name", check.name}, {"passed", check.passed}, {"message", check.message}});
  }
  const auto nanoseconds = [](std::chrono::steady_clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  };
  return {
      {"passed", association.passed},
      {"acceptance_gate", false},
      {"schedule_consistent_under_operational_assumption", association.passed},
      {"absolute_association_proven", false},
      {"frame_delivery_latency_basis",
       "one nominal frame of fixed camera/USB delivery latency is an operational assumption, "
       "not a calibrated bound"},
      {"acceptance_basis",
       "LED detection and duration in the selected command-relative 48-frame window, together "
       "with separately gated Feather command/device timing; absolute camera-to-Feather edge "
       "association is not claimed"},
      {"schedule_bracket_width_ns", nanoseconds(association.schedule_bracket_width)},
      {"nominal_frame_interval_ns", nanoseconds(association.nominal_frame_interval)},
      {"mapping_uncertainty_ns", nanoseconds(association.mapping_uncertainty)},
      {"frame_delivery_latency_bound_ns", nanoseconds(association.frame_delivery_latency_bound)},
      {"edge_tolerance_ns", nanoseconds(association.edge_tolerance)},
      {"observed_start_host_ns", SteadyNanoseconds(association.observed_start)},
      {"observed_end_exclusive_host_ns", SteadyNanoseconds(association.observed_end_exclusive)},
      {"allowed_start_earliest_host_ns", SteadyNanoseconds(association.allowed_start_earliest)},
      {"allowed_start_latest_host_ns", SteadyNanoseconds(association.allowed_start_latest)},
      {"allowed_end_earliest_host_ns", SteadyNanoseconds(association.allowed_end_earliest)},
      {"allowed_end_latest_host_ns", SteadyNanoseconds(association.allowed_end_latest)},
      {"checks", std::move(checks)},
  };
}

json AudioJson(const CommandedToneEvaluation &evaluation, const AudioCaptureResult &capture,
               std::string_view device, std::uint64_t commanded_sample_offset,
               std::chrono::steady_clock::time_point sample_snapshot_time,
               const FeatherStimulusReceipt &tone_receipt, const CommandedToneStimulus &stimulus,
               const CommandedToneThresholds &thresholds, std::uint16_t channel_count,
               std::uint16_t selected_channel, std::size_t wav_bytes) {
  const auto &measurements = evaluation.measurements;
  const auto &diagnostics = capture.diagnostics;
  json checks = json::array();
  for (const auto &check : evaluation.checks) {
    checks.push_back({{"name", check.name}, {"passed", check.passed}, {"message", check.message}});
  }
  return {
      {"passed", evaluation.passed},
      {"device", device},
      {"sample_rate_hz", measurements.sample_rate_hz},
      {"channel_count", channel_count},
      {"selected_channel", selected_channel},
      {"frames_per_block", 1024},
      {"captured_samples", measurements.captured_samples},
      {"captured_seconds",
       static_cast<double>(measurements.captured_samples) / measurements.sample_rate_hz},
      {"capture_session",
       {{"source_started", diagnostics.source_started},
        {"stop_requested", diagnostics.stop_requested},
        {"reached_end_of_stream", diagnostics.reached_end_of_stream},
        {"continuity_valid", diagnostics.continuity_valid},
        {"clean_shutdown", diagnostics.clean_shutdown},
        {"completed_blocks", diagnostics.completed_blocks},
        {"completed_samples", diagnostics.completed_samples},
        {"worker_elapsed_ns", diagnostics.worker_elapsed.count()},
        {"error", diagnostics.error}}},
      {"commanded_sample_offset", commanded_sample_offset},
      {"command_sample_snapshot_host_ns", SteadyNanoseconds(sample_snapshot_time)},
      {"command_write_started_host_ns", SteadyNanoseconds(tone_receipt.host_command_write_started)},
      {"command_sent_host_ns", SteadyNanoseconds(tone_receipt.host_command_sent)},
      {"sample_snapshot_to_write_started_us",
       std::chrono::duration_cast<std::chrono::microseconds>(
           tone_receipt.host_command_write_started - sample_snapshot_time)
           .count()},
      {"stimulus",
       {{"lead_us", stimulus.lead.count()},
        {"duration_us", stimulus.duration.count()},
        {"frequency_hz", stimulus.frequency_hz}}},
      {"analysis_configuration",
       {{"background_duration_ms", thresholds.background_duration.count()},
        {"background_guard_ms", thresholds.background_guard.count()},
        {"onset_early_tolerance_ms", thresholds.onset_early_tolerance.count()},
        {"maximum_additional_latency_ms", thresholds.maximum_additional_latency.count()},
        {"spectral_frame_duration_ms", thresholds.spectral_frame_duration.count()},
        {"spectral_hop_duration_ms", thresholds.spectral_hop_duration.count()},
        {"minimum_event_rms_normalized_amplitude",
         thresholds.minimum_event_rms_normalized_amplitude},
        {"minimum_signal_to_noise_decibels", thresholds.minimum_signal_to_noise_decibels},
        {"minimum_fundamental_power_fraction", thresholds.minimum_fundamental_power_fraction},
        {"maximum_frequency_error_hz", thresholds.maximum_frequency_error_hz},
        {"minimum_active_duration_ms", thresholds.minimum_active_duration.count()},
        {"maximum_active_duration_ms", thresholds.maximum_active_duration.count()},
        {"maximum_event_clipped_fraction", thresholds.maximum_event_clipped_fraction}}},
      {"background_start_sample_offset", measurements.background_start_sample_offset},
      {"background_end_sample_offset", measurements.background_end_sample_offset},
      {"onset_search_start_sample_offset", measurements.onset_search_start_sample_offset},
      {"onset_search_end_sample_offset", measurements.onset_search_end_sample_offset},
      {"detected_onset_sample_offset", measurements.detected_onset_sample_offset},
      {"detected_end_sample_offset", measurements.detected_end_sample_offset},
      {"detected_onset_delay_samples", measurements.detected_onset_delay_samples},
      {"detected_onset_delay_seconds", measurements.detected_onset_delay_seconds},
      {"measured_active_duration_samples", measurements.measured_active_duration_samples},
      {"measured_active_duration_seconds", measurements.measured_active_duration_seconds},
      {"analysis_start_sample_offset", measurements.analysis_start_sample_offset},
      {"analysis_end_sample_offset", measurements.analysis_end_sample_offset},
      {"peak_normalized_amplitude", measurements.peak_normalized_amplitude},
      {"event_rms_normalized_amplitude", measurements.event_rms_normalized_amplitude},
      {"background_rms_normalized_amplitude", measurements.background_rms_normalized_amplitude},
      {"signal_rms_normalized_amplitude", measurements.signal_rms_normalized_amplitude},
      {"signal_to_noise_decibels", measurements.signal_to_noise_decibels},
      {"fundamental_power_fraction", measurements.fundamental_power_fraction},
      {"estimated_frequency_hz", measurements.estimated_frequency_hz},
      {"frequency_error_hz", measurements.frequency_error_hz},
      {"active_spectral_frames", measurements.active_spectral_frames},
      {"event_clipped_samples", measurements.event_clipped_samples},
      {"event_clipped_fraction", measurements.event_clipped_fraction},
      {"timestamp_model",
       "command offset uses completed PCM blocks before serial send; delay includes USB, firmware, "
       "ALSA, pipe, amplifier, speaker, and acoustic latency"},
      {"acceptance_scope",
       "energy, 2 kHz frequency concentration, and 20 ms duration; not calibrated latency"},
      {"wav", "speaker-microphone.wav"},
      {"wav_sample_count", capture.samples.size()},
      {"wav_bytes", wav_bytes},
      {"checks", std::move(checks)},
  };
}

json ImageQualityJson(const ImageQualityMetrics &metrics) {
  return {
      {"sample_count", metrics.sample_count},
      {"minimum", metrics.min_value},
      {"maximum", metrics.max_value},
      {"mean", metrics.mean},
      {"p01", metrics.p01},
      {"p50", metrics.p50},
      {"p99", metrics.p99},
      {"near_black_fraction", metrics.near_black_fraction},
      {"near_white_fraction", metrics.near_white_fraction},
      {"gradient_energy", metrics.gradient_energy},
  };
}

json CaptureFailureCameraTransportJson(const CameraCapture &camera) {
  const DahengDiagnostics &diagnostics = camera.diagnostics();
  return {
      {"role", camera.role()},
      {"serial", camera.serial()},
      {"capture_error", camera.capture_error()},
      {"configuration",
       {{"width", diagnostics.width},
        {"height", diagnostics.height},
        {"pixel_format", diagnostics.pixel_format},
        {"resulting_frames_per_second", diagnostics.resulting_frames_per_second},
        {"exposure_us", diagnostics.exposure_microseconds},
        {"gain_db", diagnostics.gain_decibels},
        {"acquisition_buffer_count", diagnostics.acquisition_buffer_count}}},
      {"captured_frames", camera.captured_frames()},
      {"ring_capacity_frames", kCameraRingFrames},
      {"ring_allocated_bytes", camera.ring_allocated_bytes()},
      {"timeouts", camera.timeouts()},
      {"frame_id_gaps", camera.frame_id_gaps()},
      {"payload_bytes", camera.payload_bytes()},
      {"expected_payload_bytes_per_frame", diagnostics.payload_bytes},
      {"host_fps", camera.measured_host_frames_per_second()},
      {"device_fps", camera.measured_device_frames_per_second()},
      {"maximum_host_interval_seconds", camera.maximum_host_interval_seconds()},
      {"maximum_host_interval_previous_frame_id", camera.maximum_host_interval_previous_frame_id()},
      {"maximum_host_interval_frame_id", camera.maximum_host_interval_frame_id()},
      {"maximum_device_interval_seconds", camera.maximum_device_interval_seconds()},
      {"first_timeout_host_ns", SteadyNanoseconds(camera.first_timeout_at())},
      {"first_timeout_after_frame_id", camera.first_timeout_after_frame_id()},
      {"clock_mapper", ClockMapperJson(camera.clock_mapper())},
  };
}

json CaptureFailureCameraJson(const CameraCapture &camera, const PooledRawFrameSnapshot &snapshot,
                              const std::filesystem::path &output_directory) {
  json result = CaptureFailureCameraTransportJson(camera);
  result["retained_frames"] = snapshot.size();
  result["retention_passed"] = snapshot.size() == camera.captured_frames();
  if (snapshot.empty()) {
    result["diagnostic"] = "no complete frame was retained";
    return result;
  }

  const PooledRawFrameHandle &frame = snapshot.at(snapshot.size() - 1);
  const std::string image_path = "failure-" + camera.role() + ".png";
  result["diagnostic_frame_id"] = frame.metadata().frame_id;
  result["diagnostic_device_timestamp"] = frame.metadata().device_timestamp;
  try {
    WriteText(output_directory / image_path,
              EncodePng(DemosaicBayerRg8(frame.payload(), frame.metadata().width,
                                         frame.metadata().height)));
    const Raw8ImageView image = {
        .pixels = frame.payload(),
        .width = frame.metadata().width,
        .height = frame.metadata().height,
        .row_stride_bytes = frame.metadata().width,
    };
    result["diagnostic_image"] = image_path;
    result["diagnostic_image_quality"] = ImageQualityJson(MeasureRaw8ImageQuality(image));
  } catch (const std::exception &error) {
    result["diagnostic_image_error"] = error.what();
  }
  return result;
}

json AudioCaptureDiagnosticsJson(const AudioCaptureResult &capture) {
  const auto &diagnostics = capture.diagnostics;
  return {
      {"source_started", diagnostics.source_started},
      {"stop_requested", diagnostics.stop_requested},
      {"reached_end_of_stream", diagnostics.reached_end_of_stream},
      {"continuity_valid", diagnostics.continuity_valid},
      {"clean_shutdown", diagnostics.clean_shutdown},
      {"completed_blocks", diagnostics.completed_blocks},
      {"completed_samples", diagnostics.completed_samples},
      {"worker_elapsed_ns", diagnostics.worker_elapsed.count()},
      {"error", diagnostics.error},
  };
}

void CheckpointFailureAudio(const std::filesystem::path &output_directory,
                            const StationConfig &station, const AudioCaptureSession &audio,
                            const AudioCaptureWaitResult &audio_stop, json *report) {
  json evidence = {
      {"device", station.audio_alsa_device},
      {"channel_count", station.audio_channel_count},
      {"selected_channel", station.audio_selected_channel},
      {"started", audio.started()},
      {"joined", audio.joined()},
      {"stop_joined", audio_stop.joined},
      {"completed_samples_at_stop", audio_stop.completed_samples},
      {"stop_error", audio_stop.error},
  };
  try {
    const std::optional<AudioCaptureResult> capture = audio.CopyTerminalResult();
    if (!capture.has_value()) {
      evidence["evidence_unavailable"] =
          "audio worker result was still mutable at the failure checkpoint";
    } else {
      evidence["wav_sample_count"] = capture->samples.size();
      evidence["capture_session"] = AudioCaptureDiagnosticsJson(*capture);
      try {
        const std::vector<std::byte> wav =
            EncodeMonoPcmS16Wav(capture->samples, kAudioSampleRateHz);
        WriteBytes(output_directory / "speaker-microphone.wav", wav);
        evidence["wav"] = "speaker-microphone.wav";
        evidence["wav_bytes"] = wav.size();
      } catch (const std::exception &error) {
        evidence["wav_error"] = error.what();
      }
    }
  } catch (const std::exception &error) {
    evidence["evidence_error"] = error.what();
  }
  (*report)["audio"] = std::move(evidence);
}

json CheckpointFailureCamera(const CameraCapture &camera,
                             const std::filesystem::path &output_directory) {
  try {
    return CaptureFailureCameraJson(camera, camera.Freeze(), output_directory);
  } catch (const std::exception &error) {
    json evidence = CaptureFailureCameraTransportJson(camera);
    evidence["evidence_error"] = error.what();
    return evidence;
  }
}

void CheckpointRunFailure(const std::filesystem::path &output_directory, std::string fatal_error,
                          const std::optional<FeatherStimulusReceipt> &led_receipt,
                          const std::optional<FeatherStimulusReceipt> &tone_receipt,
                          const StationConfig &station, const AudioCaptureSession &audio,
                          const AudioCaptureWaitResult &audio_stop,
                          const CameraCapture &down_the_line, const CameraCapture &face_on,
                          json *report) {
  (*report)["passed"] = false;
  (*report)["state"] = "failed";
  (*report)["fatal_error"] = std::move(fatal_error);
  if (led_receipt.has_value()) {
    (*report)["feather"]["led"] = ReceiptJson(*led_receipt);
  }
  if (tone_receipt.has_value()) {
    (*report)["feather"]["tone"] = ReceiptJson(*tone_receipt);
  }
  CheckpointFailureAudio(output_directory, station, audio, audio_stop, report);
  (*report)["cameras"] = json::array({CheckpointFailureCamera(down_the_line, output_directory),
                                      CheckpointFailureCamera(face_on, output_directory)});
  WriteReport(output_directory, *report);
}

struct CameraEvidence {
  LedPulseResult led;
  LedScheduleAssociationResult led_schedule_association;
  RobustDeviceClockMappingDiagnostics robust_clock_mapping;
  std::vector<std::size_t> clock_mapping_outlier_indices;
  TagResult tag;
  LedAnalysisFrameSelection led_selection;
  TagRepresentativeFrameSelection tag_selection;
  LedPulseOptions led_options;
  ImageQualityMetrics tag_image_quality;
  std::size_t tag_retained_frame_index = 0;
  std::size_t led_retained_frame_index = 0;
  bool pulse_frame_count_passed = false;
  bool pulse_duration_passed = false;
  std::string tag_image;
  std::string led_image;
};

CameraEvidence AnalyzeCamera(const CameraCapture &camera, const PooledRawFrameSnapshot &snapshot,
                             const FeatherStimulusReceipt &led_receipt,
                             const std::filesystem::path &output_directory) {
  if (snapshot.size() < swing_capture::optical::kLedAnalysisFrameCount) {
    throw std::runtime_error(camera.role() + " retained too few frames for optical analysis");
  }
  const std::vector<BayerRg8FrameView> frames = OpticalViews(snapshot);
  const std::vector<DeviceHostTimestampSample> timestamp_samples =
      DeviceHostTimestampSamples(snapshot);
  RobustDeviceClockMappingResult robust_mapping = FitRobustDeviceClockMapping(
      timestamp_samples, camera.diagnostics().timestamp_ticks_per_second);
  const std::vector<std::chrono::steady_clock::time_point> &mapped_host_times =
      robust_mapping.mapped_host_receipt_times;
  const FeatherAckScheduleBracket schedule_bracket = {
      .host_command_sent = led_receipt.host_command_sent,
      .host_acknowledgement_received = led_receipt.host_acknowledgement_received,
      .accepted_device_microseconds = led_receipt.accepted_device_microseconds,
      .scheduled_device_microseconds = led_receipt.scheduled_device_microseconds,
  };
  const auto schedule = EstimateStimulusHostSchedule(schedule_bracket);
  const LedAnalysisFrameSelection led_selection =
      SelectLedAnalysisFrames(frames, mapped_host_times, schedule);
  const auto analysis_frames = std::span<const BayerRg8FrameView>(frames).subspan(
      led_selection.begin_index, led_selection.end_index_exclusive - led_selection.begin_index);
  const auto analysis_host_times =
      std::span<const std::chrono::steady_clock::time_point>(mapped_host_times)
          .subspan(led_selection.begin_index,
                   led_selection.end_index_exclusive - led_selection.begin_index);

  LedPulseOptions led_options;
  led_options.baseline_frame_count = 8;
  led_options.minimum_red_sample_delta = 3;
  led_options.minimum_active_frames = 9;
  led_options.maximum_pulse_span_frames = 11;
  LedPulseResult led = AnalyzeLedPulse(
      analysis_frames, camera.diagnostics().timestamp_ticks_per_second, led_options);
  const double frame_seconds = 1.0 / camera.diagnostics().resulting_frames_per_second;
  const auto frame_duration = std::chrono::duration<double>(frame_seconds);
  const LedScheduleAssociationOptions association_options = {
      .commanded_duration = std::chrono::microseconds(led_receipt.elapsed_device_microseconds),
      .exposure_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double, std::micro>(camera.diagnostics().exposure_microseconds)),
      .mapping_uncertainty = robust_mapping.diagnostics.receipt_time_uncertainty,
      .maximum_mapping_uncertainty = frame_duration,
      .frame_delivery_latency_bound =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_duration),
      .maximum_schedule_bracket_width =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_duration),
  };
  LedScheduleAssociationResult led_schedule_association =
      EvaluateLedScheduleAssociation(analysis_host_times, led, schedule, association_options);
  const TagRepresentativeFrameSelection tag_selection =
      SelectTagRepresentativeFrame(analysis_frames, led.frames,
                                   MappedTagTimelineView{.frame_host_times = analysis_host_times,
                                                         .scheduled_start = schedule.target_start});
  const std::size_t tag_index = led_selection.begin_index + tag_selection.frame_index;
  const auto tag_rgb =
      DemosaicBayerRg8(snapshot.at(tag_index).payload(), snapshot.at(tag_index).metadata().width,
                       snapshot.at(tag_index).metadata().height);
  const std::vector<std::byte> tag_luminance = Rgb8ToLuminance(tag_rgb);
  TagResult tag = DetectPrintedTag({
      .pixels = tag_luminance,
      .width = tag_rgb.width,
      .height = tag_rgb.height,
      .row_stride_bytes = tag_rgb.width,
  });
  const ImageQualityMetrics tag_image_quality = MeasureRaw8ImageQuality(frames[tag_index].image);

  const std::size_t led_analysis_index =
      led.detected ? SelectAcceptedLedPeakFrame(led) : tag_selection.frame_index;
  const std::size_t led_index = led_selection.begin_index + led_analysis_index;
  const std::string tag_image = "tag-" + camera.role() + ".png";
  const std::string led_image = "led-" + camera.role() + ".png";
  WriteText(output_directory / tag_image, EncodePng(tag_rgb));
  WriteText(output_directory / led_image,
            EncodePng(DemosaicBayerRg8(snapshot.at(led_index).payload(),
                                       snapshot.at(led_index).metadata().width,
                                       snapshot.at(led_index).metadata().height)));

  const bool pulse_frame_count_passed =
      led.pulse_active_frame_count >= 9 && led.pulse_active_frame_count <= 11;
  const bool pulse_duration_passed = led.detected &&
                                     led.duration_ticks >= 9U * led.nominal_frame_interval_ticks &&
                                     led.duration_ticks <= 11U * led.nominal_frame_interval_ticks;
  return {
      .led = std::move(led),
      .led_schedule_association = std::move(led_schedule_association),
      .robust_clock_mapping = std::move(robust_mapping.diagnostics),
      .clock_mapping_outlier_indices = std::move(robust_mapping.outlier_sample_indices),
      .tag = std::move(tag),
      .led_selection = led_selection,
      .tag_selection = tag_selection,
      .led_options = led_options,
      .tag_image_quality = tag_image_quality,
      .tag_retained_frame_index = tag_index,
      .led_retained_frame_index = led_index,
      .pulse_frame_count_passed = pulse_frame_count_passed,
      .pulse_duration_passed = pulse_duration_passed,
      .tag_image = tag_image,
      .led_image = led_image,
  };
}

json CameraJson(const CameraCapture &camera, const CameraEvidence &evidence,
                std::size_t retained_frames) {
  const bool retention_passed = retained_frames == camera.captured_frames();
  const DahengDiagnostics &diagnostics = camera.diagnostics();
  const double host_fps = camera.measured_host_frames_per_second();
  const double device_fps = camera.measured_device_frames_per_second();
  const double minimum_fps = diagnostics.requested_frames_per_second * 0.98;
  const double maximum_fps = diagnostics.requested_frames_per_second * 1.02;
  const double maximum_host_interval = 10.0 / diagnostics.resulting_frames_per_second;
  const double maximum_device_interval = 4.0 / diagnostics.resulting_frames_per_second;
  const bool fps_passed = host_fps >= minimum_fps && host_fps <= maximum_fps &&
                          device_fps >= minimum_fps && device_fps <= maximum_fps;
  const bool interval_passed = camera.maximum_host_interval_seconds() <= maximum_host_interval &&
                               camera.maximum_device_interval_seconds() <= maximum_device_interval;
  const bool payload_passed =
      camera.payload_bytes() == camera.captured_frames() * diagnostics.payload_bytes;
  const bool transport_passed = camera.timeouts() == 0 && camera.frame_id_gaps() == 0 &&
                                retention_passed && camera.clock_mapper().ready() && fps_passed &&
                                interval_passed && payload_passed;
  const bool passed = transport_passed && evidence.led.detected &&
                      evidence.pulse_frame_count_passed && evidence.pulse_duration_passed &&
                      evidence.tag.accepted_detection.has_value();
  const json checks = json::array({
      {{"name", "timeouts_zero"},
       {"passed", camera.timeouts() == 0},
       {"message", "timeouts=" + std::to_string(camera.timeouts())}},
      {{"name", "frame_id_gaps_zero"},
       {"passed", camera.frame_id_gaps() == 0},
       {"message", "frame_id_gaps=" + std::to_string(camera.frame_id_gaps())}},
      {{"name", "retention_complete"},
       {"passed", retention_passed},
       {"message", "captured=" + std::to_string(camera.captured_frames()) +
                       " retained=" + std::to_string(retained_frames)}},
      {{"name", "payload_exact"},
       {"passed", payload_passed},
       {"message", "payload_bytes=" + std::to_string(camera.payload_bytes())}},
      {{"name", "clock_mapper_ready"},
       {"passed", camera.clock_mapper().ready()},
       {"message", "samples=" + std::to_string(camera.clock_mapper().sample_count())}},
      {{"name", "measured_frame_rates"},
       {"passed", fps_passed},
       {"message",
        "host_fps=" + std::to_string(host_fps) + " device_fps=" + std::to_string(device_fps)}},
      {{"name", "maximum_frame_intervals"},
       {"passed", interval_passed},
       {"message",
        "host_seconds=" + std::to_string(camera.maximum_host_interval_seconds()) +
            " device_seconds=" + std::to_string(camera.maximum_device_interval_seconds())}},
      {{"name", "led_detected"},
       {"passed", evidence.led.detected},
       {"message", evidence.led.diagnostic}},
      {{"name", "led_visible_frame_count"},
       {"passed", evidence.pulse_frame_count_passed},
       {"message", "active_frames=" + std::to_string(evidence.led.pulse_active_frame_count)}},
      {{"name", "led_duration"},
       {"passed", evidence.pulse_duration_passed},
       {"message", "duration_seconds=" + std::to_string(evidence.led.duration_seconds)}},
      {{"name", "april_tag_present"},
       {"passed", evidence.tag.accepted_detection.has_value()},
       {"message",
        evidence.tag.accepted_detection.has_value() ? "tag accepted" : "no acceptable tag"}},
  });
  return {
      {"passed", passed},
      {"role", camera.role()},
      {"serial", camera.serial()},
      {"configuration",
       {{"requested",
         {{"width", 1440},
          {"height", 1080},
          {"frames_per_second", diagnostics.requested_frames_per_second},
          {"exposure_us", diagnostics.requested_exposure_microseconds},
          {"gain_db", diagnostics.requested_gain_decibels},
          {"acquisition_buffer_count", 40}}},
        {"read_back",
         {{"width", diagnostics.width},
          {"height", diagnostics.height},
          {"offset_x", diagnostics.offset_x},
          {"offset_y", diagnostics.offset_y},
          {"pixel_format", diagnostics.pixel_format},
          {"resulting_frames_per_second", diagnostics.resulting_frames_per_second},
          {"exposure_us", diagnostics.exposure_microseconds},
          {"gain_db", diagnostics.gain_decibels},
          {"acquisition_buffer_count", diagnostics.acquisition_buffer_count}}}}},
      {"captured_frames", camera.captured_frames()},
      {"retained_frames", retained_frames},
      {"ring_capacity_frames", kCameraRingFrames},
      {"ring_reserve_frames", 0},
      {"ring_allocated_bytes", camera.ring_allocated_bytes()},
      {"retention_passed", retention_passed},
      {"timeouts", camera.timeouts()},
      {"frame_id_gaps", camera.frame_id_gaps()},
      {"payload_bytes", camera.payload_bytes()},
      {"expected_payload_bytes_per_frame", diagnostics.payload_bytes},
      {"timestamp_ticks_per_second", camera.diagnostics().timestamp_ticks_per_second},
      {"online_receipt_clock_mapper", ClockMapperJson(camera.clock_mapper())},
      {"robust_receipt_clock_mapping",
       RobustClockMappingJson(evidence.robust_clock_mapping,
                              evidence.clock_mapping_outlier_indices)},
      {"resulting_fps", camera.diagnostics().resulting_frames_per_second},
      {"host_fps", host_fps},
      {"device_fps", device_fps},
      {"maximum_host_interval_seconds", camera.maximum_host_interval_seconds()},
      {"maximum_host_interval_previous_frame_id", camera.maximum_host_interval_previous_frame_id()},
      {"maximum_host_interval_frame_id", camera.maximum_host_interval_frame_id()},
      {"first_timeout_host_ns", SteadyNanoseconds(camera.first_timeout_at())},
      {"first_timeout_after_frame_id", camera.first_timeout_after_frame_id()},
      {"maximum_device_interval_seconds", camera.maximum_device_interval_seconds()},
      {"exposure_us", camera.diagnostics().exposure_microseconds},
      {"gain_db", camera.diagnostics().gain_decibels},
      {"pulse_frame_count_passed", evidence.pulse_frame_count_passed},
      {"pulse_duration_passed", evidence.pulse_duration_passed},
      {"optical_selection",
       {{"analysis_begin_retained_index", evidence.led_selection.begin_index},
        {"analysis_pivot_retained_index", evidence.led_selection.pivot_index},
        {"analysis_end_retained_index_exclusive", evidence.led_selection.end_index_exclusive},
        {"estimated_start_earliest_host_ns",
         SteadyNanoseconds(evidence.led_selection.schedule.earliest_start)},
        {"estimated_start_latest_host_ns",
         SteadyNanoseconds(evidence.led_selection.schedule.latest_start)},
        {"estimated_start_target_host_ns",
         SteadyNanoseconds(evidence.led_selection.schedule.target_start)},
        {"tag_retained_frame_index", evidence.tag_retained_frame_index},
        {"tag_frame_id", evidence.led.frames.at(evidence.tag_selection.frame_index).frame_id},
        {"tag_used_preflash_mapping", evidence.tag_selection.used_preflash_mapping},
        {"tag_selection_diagnostic", evidence.tag_selection.diagnostic},
        {"led_peak_retained_frame_index", evidence.led_retained_frame_index}}},
      {"led", LedJson(evidence.led, evidence.led_options)},
      {"led_schedule_association", LedScheduleAssociationJson(evidence.led_schedule_association)},
      {"april_tag", TagJson(evidence.tag)},
      {"tag_frame_image_quality", ImageQualityJson(evidence.tag_image_quality)},
      {"tag_image", evidence.tag_image},
      {"led_image", evidence.led_image},
      {"checks", checks},
  };
}

bool SameTagIdentity(const CameraEvidence &first, const CameraEvidence &second) {
  if (!first.tag.accepted_detection.has_value() || !second.tag.accepted_detection.has_value()) {
    return false;
  }
  return first.tag.accepted_detection->family == second.tag.accepted_detection->family &&
         first.tag.accepted_detection->id == second.tag.accepted_detection->id;
}

int RunFixtureHil() {
  const std::filesystem::path output_directory = OutputDirectory();
  std::filesystem::create_directories(output_directory);
  json report = BaseReport();
  WriteReport(output_directory, report);

  const std::filesystem::path station_path = RequiredStationPath();
  const StationConfig station = swing_capture::station::LoadStationConfig(station_path);
  if (!station.camera_roles_verified) {
    throw std::runtime_error("station camera roles must be verified before fixture HIL");
  }
  report["state"] = "station_configured";
  report["station"] = {
      {"config_path", station_path.string()},
      {"camera_roles_verified", station.camera_roles_verified},
      {"down_the_line_camera_serial", station.down_the_line_camera_serial},
      {"face_on_camera_serial", station.face_on_camera_serial},
      {"audio_alsa_device", station.audio_alsa_device},
      {"audio_channel_count", station.audio_channel_count},
      {"audio_selected_channel", station.audio_selected_channel},
      {"feather_serial_path", station.feather_serial_path.string()},
  };
  const json station_evidence = report.at("station");
  WriteReport(output_directory, report);

  const HardwareLock hardware_lock(HardwareLockPath(station_path));

  FeatherHilSerial serial(station.feather_serial_path);
  FeatherHilController feather(serial);
  const FeatherDeviceInfo info = feather.QueryInfo();
  constexpr bool kFeatherCompatible = true;
  report["state"] = "feather_negotiated";
  report["feather"] = {
      {"serial_path", station.feather_serial_path.string()},
      {"info", DeviceInfoJson(info)},
      {"compatible", kFeatherCompatible},
      {"shared_power_note",
       "GPIO23 briefly powers the speaker amplifier, external NeoPixel rail, and servo rail"},
  };
  WriteReport(output_directory, report);

  GalaxySdk sdk;
  CameraCapture down_the_line(sdk, "down_the_line", station.down_the_line_camera_serial);
  CameraCapture face_on(sdk, "face_on", station.face_on_camera_serial);
  AudioCaptureSession audio(MakeArecordAudioCaptureSourceFactory({
      .capture_executable = "/usr/bin/arecord",
      .device = station.audio_alsa_device,
      .sample_rate_hz = kAudioSampleRateHz,
      .channel_count = station.audio_channel_count,
      .selected_channel = station.audio_selected_channel,
      .frames_per_block = 1024,
      .read_timeout = std::chrono::milliseconds(250),
  }));
  std::optional<FeatherStimulusReceipt> led_receipt;
  std::optional<FeatherStimulusReceipt> tone_receipt;
  std::uint64_t tone_command_sample_offset = 0;
  std::chrono::steady_clock::time_point tone_command_sample_snapshot_time;
  std::exception_ptr failure;
  try {
    audio.Start();
    RequireAudioWait(
        audio.WaitUntilReady(std::chrono::steady_clock::now() + std::chrono::seconds(3)),
        "starting microphone capture");
    down_the_line.Start();
    face_on.Start();
    std::this_thread::sleep_for(kBaselineDuration);
    RequireAudioWait(
        audio.WaitForCompletedSamples(kAudioBaselineSamples,
                                      std::chrono::steady_clock::now() + std::chrono::seconds(2)),
        "collecting microphone baseline");
    led_receipt = feather.PulseLed(kLedLead, kLedDuration);
    std::this_thread::sleep_for(kBetweenStimuli);
    tone_command_sample_offset = audio.completed_sample_count();
    tone_command_sample_snapshot_time = std::chrono::steady_clock::now();
    tone_receipt = feather.PlayTone(kToneLead, kToneDuration, kToneFrequencyHz, kToneLevelPermille);
    std::this_thread::sleep_until(tone_receipt->host_done_received + kCameraPostStimulus);
  } catch (...) {
    failure = std::current_exception();
  }

  const auto remember_failure = [&failure](auto &&operation) {
    try {
      operation();
    } catch (...) {
      if (failure == nullptr) {
        failure = std::current_exception();
      }
    }
  };
  // Stop both producer threads before either GXStreamOff call. The Galaxy SDK
  // can briefly serialize stream shutdown across devices; leaving the other
  // producer inside GXDQBufEx manufactures a late timeout and host interval.
  down_the_line.RequestCaptureStop();
  face_on.RequestCaptureStop();
  remember_failure([&down_the_line] { down_the_line.JoinCapture(); });
  remember_failure([&face_on] { face_on.JoinCapture(); });
  remember_failure([&down_the_line] { down_the_line.Stop(); });
  remember_failure([&face_on] { face_on.Stop(); });

  if (failure == nullptr && tone_receipt.has_value()) {
    remember_failure([&] {
      RequireAudioWait(audio.WaitThroughWallTime(
                           tone_receipt->host_done_received + std::chrono::milliseconds(500),
                           std::chrono::steady_clock::now() + std::chrono::seconds(2)),
                       "capturing post-tone microphone wall-time window");
      RequireAudioWait(
          audio.WaitForCompletedSamples(tone_command_sample_offset + kAudioPostCommandSamples,
                                        std::chrono::steady_clock::now() + std::chrono::seconds(2)),
          "capturing post-tone microphone sample window");
    });
  }
  AudioCaptureWaitResult audio_stop;
  if (audio.started()) {
    audio_stop =
        audio.RequestStopAndJoin(std::chrono::steady_clock::now() + std::chrono::seconds(4));
  }
  if (audio.started() && (!audio_stop.joined || !audio_stop.error.empty())) {
    remember_failure([&] {
      throw std::runtime_error(audio_stop.error.empty()
                                   ? "timed out stopping microphone capture"
                                   : "microphone capture failed: " + audio_stop.error);
    });
  }
  if (audio_stop.joined) {
    remember_failure([&] {
      const std::vector<std::byte> partial_wav =
          EncodeMonoPcmS16Wav(audio.result().samples, kAudioSampleRateHz);
      WriteBytes(output_directory / "speaker-microphone.wav", partial_wav);
    });
  }
  const auto checkpoint_failure = [&](const std::exception_ptr &exception) {
    try {
      CheckpointRunFailure(output_directory, ExceptionMessage(exception), led_receipt, tone_receipt,
                           station, audio, audio_stop, down_the_line, face_on, &report);
    } catch (const std::exception &evidence_error) {
      std::cerr << "cannot checkpoint station fixture failure evidence: " << evidence_error.what()
                << '\n';
    }
  };
  if (failure != nullptr) {
    checkpoint_failure(failure);
    std::rethrow_exception(failure);
  }
  try {
    if (!led_receipt.has_value() || !tone_receipt.has_value()) {
      throw std::logic_error("fixture stimuli completed without receipts");
    }

    const AudioCaptureResult &audio_result = audio.result();
    const PooledRawFrameSnapshot down_snapshot = down_the_line.Freeze();
    const PooledRawFrameSnapshot face_snapshot = face_on.Freeze();
    const CameraEvidence down_evidence =
        AnalyzeCamera(down_the_line, down_snapshot, *led_receipt, output_directory);
    const CameraEvidence face_evidence =
        AnalyzeCamera(face_on, face_snapshot, *led_receipt, output_directory);
    const CommandedToneStimulus tone_stimulus = {
        .lead = kToneLead,
        .duration = kToneDuration,
        .frequency_hz = kToneFrequencyHz,
    };
    const CommandedToneThresholds tone_thresholds;
    const CommandedToneEvaluation tone_evaluation = AnalyzeCommandedTone(
        {
            .mono_samples = audio_result.samples,
            .sample_rate_hz = kAudioSampleRateHz,
            .commanded_sample_offset = tone_command_sample_offset,
        },
        tone_stimulus, tone_thresholds);
    const std::vector<std::byte> wav =
        EncodeMonoPcmS16Wav(audio_result.samples, kAudioSampleRateHz);

    const bool led_device_timing_passed =
        std::abs(static_cast<std::int64_t>(led_receipt->elapsed_device_microseconds) -
                 kLedDuration.count()) <= 100;
    const bool tone_device_timing_passed =
        std::abs(static_cast<std::int64_t>(tone_receipt->elapsed_device_microseconds) -
                 kToneDuration.count()) <= 250;
    json down_json = CameraJson(down_the_line, down_evidence, down_snapshot.size());
    json face_json = CameraJson(face_on, face_evidence, face_snapshot.size());
    const bool same_april_tag_identity = SameTagIdentity(down_evidence, face_evidence);
    const bool passed = kFeatherCompatible && led_device_timing_passed &&
                        tone_device_timing_passed && down_json.at("passed").get<bool>() &&
                        face_json.at("passed").get<bool>() && same_april_tag_identity &&
                        tone_evaluation.passed;
    report = {
        {"schema_version", 1},
        {"passed", passed},
        {"state", "complete"},
        {"scope", "dual_camera_led_apriltag_and_speaker_microphone"},
        {"station", station_evidence},
        {"feather",
         {{"serial_path", station.feather_serial_path.string()},
          {"info", DeviceInfoJson(info)},
          {"compatible", kFeatherCompatible},
          {"led", ReceiptJson(*led_receipt)},
          {"led_device_timing_passed", led_device_timing_passed},
          {"tone", ReceiptJson(*tone_receipt)},
          {"tone_device_timing_passed", tone_device_timing_passed},
          {"shared_power_note",
           "GPIO23 briefly powers the speaker amplifier, external NeoPixel rail, and servo "
           "rail"}}},
        {"cameras", json::array({std::move(down_json), std::move(face_json)})},
        {"same_april_tag_identity", same_april_tag_identity},
        {"audio",
         AudioJson(tone_evaluation, audio_result, station.audio_alsa_device,
                   tone_command_sample_offset, tone_command_sample_snapshot_time, *tone_receipt,
                   tone_stimulus, tone_thresholds, station.audio_channel_count,
                   station.audio_selected_channel, wav.size())},
        {"timing_limitations",
         "camera device clocks are evaluated independently; one nominal frame of fixed "
         "camera/USB delivery latency is only an operational assumption, so schedule "
         "association is diagnostic and does not prove absolute camera-to-Feather timing; LED "
         "acceptance is based on detection and duration in the selected command-relative "
         "48-frame window plus Feather command/device evidence; audio delay is not calibrated "
         "absolute latency; free-running exposure phases need not yield exactly ten active "
         "frames"},
    };
    WriteReport(output_directory, report);
    std::cout << "Station fixture HIL " << (passed ? "PASS" : "FAIL")
              << " report=" << output_directory / "station-fixture-report.json" << '\n';
    return passed ? 0 : 1;
  } catch (...) {
    checkpoint_failure(std::current_exception());
    throw;
  }
}

}  // namespace

int main() {
  try {
    return RunFixtureHil();
  } catch (const FeatherHilTransactionError &error) {
    try {
      const std::filesystem::path output_directory = OutputDirectory();
      std::filesystem::create_directories(output_directory);
      PreserveFatalReport(output_directory, error.what(), &error.evidence());
    } catch (const std::exception &report_error) {
      std::cerr << "cannot preserve station fixture failure report: " << report_error.what()
                << '\n';
    }
    std::cerr << "station_fixture_hil_test: " << error.what() << '\n';
    return 1;
  } catch (const std::exception &error) {
    try {
      const std::filesystem::path output_directory = OutputDirectory();
      std::filesystem::create_directories(output_directory);
      PreserveFatalReport(output_directory, error.what());
    } catch (const std::exception &report_error) {
      std::cerr << "cannot preserve station fixture failure report: " << report_error.what()
                << '\n';
    }
    std::cerr << "station_fixture_hil_test: " << error.what() << '\n';
    return 1;
  }
}
