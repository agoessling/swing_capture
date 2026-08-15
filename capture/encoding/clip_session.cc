#include "capture/encoding/clip_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/optical/white_impact_acceptance.h"

namespace swing_capture::encoding {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)
using ProfileClock = std::chrono::steady_clock;

struct CameraTimeline {
  std::vector<std::int64_t> time_from_impact_us;
  std::vector<std::uint64_t> media_time_us;
  std::size_t impact_frame_index = 0;
  double nominal_fps = 0.0;
};

double Milliseconds(ProfileClock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

double ElapsedMilliseconds(ProfileClock::time_point started) {
  return Milliseconds(ProfileClock::now() - started);
}

void ValidateNonnegativeDuration(ProfileClock::duration duration, std::string_view name) {
  const double milliseconds = Milliseconds(duration);
  if (!std::isfinite(milliseconds) || duration < ProfileClock::duration::zero()) {
    throw std::invalid_argument(std::string(name) + " must be finite and nonnegative");
  }
}

void ValidateFiniteDuration(ProfileClock::duration duration, std::string_view name) {
  if (!std::isfinite(Milliseconds(duration))) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void ValidateCapturePipelineProfile(const ClipCapturePipelineProfile &profile) {
  ValidateNonnegativeDuration(profile.trigger_estimate_to_confirmation,
                              "trigger-estimate-to-confirmation duration");
  ValidateFiniteDuration(profile.confirmation_to_acceptance, "confirmation-to-acceptance duration");
  ValidateNonnegativeDuration(profile.acceptance_to_freeze_start,
                              "acceptance-to-freeze-start duration");
  ValidateFiniteDuration(profile.freeze_schedule_lateness, "freeze schedule lateness");
  ValidateNonnegativeDuration(profile.freeze_and_rotate, "freeze-and-rotate duration");
  ValidateNonnegativeDuration(profile.audio_stop, "audio-stop duration");
  ValidateNonnegativeDuration(profile.prepublication_analysis, "prepublication-analysis duration");
  ValidateNonnegativeDuration(profile.publisher_planning, "publisher-planning duration");
  ValidateNonnegativeDuration(profile.impact_preview_render, "impact-preview-render duration");
  ValidateNonnegativeDuration(profile.impact_preview_ready_after_confirmation,
                              "impact-preview-ready duration");
}

bool IsSafePathComponent(std::string_view value) {
  if (value.empty() || value == "." || value == "..") {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
           (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) ||
           (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) ||
           character == '-' || character == '_' || character == '.';
  });
}

std::int64_t RoundedMicroseconds(std::chrono::nanoseconds value) {
  const auto rounded = std::chrono::round<std::chrono::microseconds>(value).count();
  if (!std::in_range<std::int64_t>(rounded)) {
    throw std::overflow_error("clip frame time does not fit int64 microseconds");
  }
  return static_cast<std::int64_t>(rounded);
}

std::uint64_t MediaMicroseconds(std::chrono::nanoseconds value) {
  if (value < std::chrono::nanoseconds::zero()) {
    throw std::invalid_argument("clip media time cannot be negative");
  }
  const auto rounded = std::chrono::round<std::chrono::microseconds>(value).count();
  if (!std::in_range<std::uint64_t>(rounded)) {
    throw std::overflow_error("clip media time does not fit uint64 microseconds");
  }
  return static_cast<std::uint64_t>(rounded);
}

std::size_t ValidateViewHeader(const CameraClipInput &view) {
  if (!IsSafePathComponent(view.role)) {
    throw std::invalid_argument("clip role must be a safe nonempty path component");
  }
  if (view.camera_serial.empty()) {
    throw std::invalid_argument("clip camera serial cannot be empty");
  }
  if (view.pixel_format != "BayerRG8") {
    throw std::invalid_argument("the software clip encoder requires BayerRG8 input");
  }
  if (view.frames.size() < 2U) {
    throw std::invalid_argument("a clip view requires at least two frames");
  }

  const FrameMetadata &first = view.frames.front().frame.metadata;
  if (first.width < 2U || first.height < 2U) {
    throw std::invalid_argument("clip source dimensions must both be at least two");
  }
  if (first.width > std::numeric_limits<std::size_t>::max() / first.height) {
    throw std::overflow_error("clip source dimensions overflow size_t");
  }
  return static_cast<std::size_t>(first.width) * first.height;
}

void ValidateNormalizedEvidence(std::optional<double> value, std::string_view name) {
  if (value.has_value() && (!std::isfinite(*value) || *value < 0.0 || *value > 1.0)) {
    throw std::invalid_argument(std::string(name) + " must be finite and in [0, 1]");
  }
}

void ValidateTrigger(const ClipTriggerMetadata &trigger) {
  if (trigger.source.empty()) {
    throw std::invalid_argument("trigger source cannot be empty");
  }
  if (trigger.host_monotonic_time_ns < 0) {
    throw std::invalid_argument("trigger monotonic strike time cannot be negative");
  }
  if (trigger.confirmation_host_monotonic_time_ns.has_value() &&
      *trigger.confirmation_host_monotonic_time_ns < trigger.host_monotonic_time_ns) {
    throw std::invalid_argument("trigger confirmation cannot precede the strike time");
  }
  if (trigger.sample_rate_hz.has_value() && *trigger.sample_rate_hz == 0U) {
    throw std::invalid_argument("trigger sample rate must be positive");
  }
  ValidateNormalizedEvidence(trigger.peak_amplitude, "trigger peak amplitude");
  ValidateNormalizedEvidence(trigger.noise_floor, "trigger noise floor");
  ValidateNormalizedEvidence(trigger.threshold, "trigger threshold");
}

std::int64_t RequireConfirmedTriggerTimestamp(const ClipTriggerMetadata &trigger) {
  if (!trigger.confirmation_host_monotonic_time_ns.has_value()) {
    throw std::invalid_argument("pipeline profiling requires a confirmed trigger timestamp");
  }
  return *trigger.confirmation_host_monotonic_time_ns;
}

void ValidateWhiteImpactEvidence(const SyntheticSwingHilViewEvidence &view) {
  const bool fractions_valid =
      std::isfinite(view.matching_fraction) && view.matching_fraction >= 0.0 &&
      view.matching_fraction <= 1.0 && std::isfinite(view.mean_signal_delta) &&
      view.mean_signal_delta >= 0.0 && std::isfinite(view.mean_expected_color_distance) &&
      view.mean_expected_color_distance >= 0.0 && std::isfinite(view.maximum_saturated_fraction) &&
      view.maximum_saturated_fraction >= 0.0 && view.maximum_saturated_fraction <= 1.0 &&
      std::isfinite(view.maximum_bloom_fraction) && view.maximum_bloom_fraction >= 0.0 &&
      view.maximum_bloom_fraction <= 1.0;
  const double expected_matching_fraction = view.stable_frame_count == 0U
                                                ? 0.0
                                                : static_cast<double>(view.matching_frame_count) /
                                                      static_cast<double>(view.stable_frame_count);
  const bool declared_pass_meets_policy =
      !view.optical_white_passed ||
      optical::kDefaultWhiteImpactAcceptancePolicy.Accepts(
          view.stable_frame_count, view.matching_fraction, view.mean_signal_delta,
          view.maximum_saturated_fraction, view.maximum_bloom_fraction);
  if (view.matching_frame_count > view.stable_frame_count || !fractions_valid ||
      std::abs(view.matching_fraction - expected_matching_fraction) > 1e-12 ||
      !declared_pass_meets_policy) {
    throw std::invalid_argument("synthetic swing white-impact evidence is inconsistent");
  }
}

void ValidateHilEvidence(const DualViewClipInput &input,
                         const std::array<CameraTimeline, 2> &timelines) {
  if (!input.hil_evidence.has_value()) {
    return;
  }
  const SyntheticSwingHilEvidence &evidence = *input.hil_evidence;
  if (evidence.selected_brightness == 0U || evidence.step_duration_us == 0U ||
      evidence.pre_impact_step_count == 0U || evidence.white_impact_duration_us == 0U ||
      evidence.post_impact_step_count == 0U || evidence.tone_duration_us == 0U ||
      evidence.tone_frequency_hz == 0U) {
    throw std::invalid_argument("synthetic swing HIL evidence contains zero-valued stimulus data");
  }
  for (std::size_t index = 0; index < evidence.views.size(); ++index) {
    const SyntheticSwingHilViewEvidence &view = evidence.views[index];
    if (view.role != input.views[index].role ||
        view.optical_white_impact_frame_index >= timelines[index].time_from_impact_us.size() ||
        view.mapped_time_correction_us < -1'000'000 || view.mapped_time_correction_us > 1'000'000 ||
        view.schedule_uncertainty_us == 0U || view.schedule_uncertainty_us > 1'000'000U ||
        !std::isfinite(view.exposure_us) || view.exposure_us <= 0.0 ||
        !std::isfinite(view.gain_db) || view.gain_db < 0.0) {
      throw std::invalid_argument("synthetic swing HIL camera evidence is inconsistent");
    }
    ValidateWhiteImpactEvidence(view);
  }
}

Json HilEvidenceJson(const SyntheticSwingHilEvidence &evidence) {
  Json optical_indices = Json::object();
  Json audio_offsets = Json::object();
  Json schedule_alignment = Json::object();
  Json optical_checks = Json::object();
  for (const SyntheticSwingHilViewEvidence &view : evidence.views) {
    optical_indices[view.role] = view.optical_white_impact_frame_index;
    audio_offsets[view.role] = view.audio_trigger_estimate_offset_us;
    schedule_alignment[view.role] = {
        {"mapped_time_correction_us", view.mapped_time_correction_us},
        {"uncertainty_us", view.schedule_uncertainty_us},
    };
    optical_checks[view.role] = {
        {"passed", view.optical_white_passed},
        {"stable_frame_count", view.stable_frame_count},
        {"matching_frame_count", view.matching_frame_count},
        {"matching_fraction", view.matching_fraction},
        {"mean_signal_delta", view.mean_signal_delta},
        {"mean_expected_color_distance", view.mean_expected_color_distance},
        {"maximum_saturated_fraction", view.maximum_saturated_fraction},
        {"maximum_bloom_fraction", view.maximum_bloom_fraction},
        {"exposure_us", view.exposure_us},
        {"gain_db", view.gain_db},
    };
  }
  return {
      {"kind", "synthetic_swing"},
      {"selected_brightness", evidence.selected_brightness},
      {"timeline",
       {
           {"step_duration_us", evidence.step_duration_us},
           {"pre_impact_step_count", evidence.pre_impact_step_count},
           {"white_impact_duration_us", evidence.white_impact_duration_us},
           {"post_impact_step_count", evidence.post_impact_step_count},
       }},
      {"tone",
       {{"duration_us", evidence.tone_duration_us}, {"frequency_hz", evidence.tone_frequency_hz}}},
      {"optical_white_impact_frame_index", std::move(optical_indices)},
      {"audio_trigger_estimate_offset_us", std::move(audio_offsets)},
      {"camera_schedule_alignment", std::move(schedule_alignment)},
      {"optical_white_impact", std::move(optical_checks)},
  };
}

void ValidateFrameGeometry(const ClipFrameInput &frame, const FrameMetadata &first,
                           std::size_t expected_payload) {
  const FrameMetadata &metadata = frame.frame.metadata;
  if (!metadata.complete) {
    throw std::invalid_argument("clip input contains an incomplete frame");
  }
  if (metadata.width != first.width || metadata.height != first.height ||
      frame.frame.payload.size() != expected_payload) {
    throw std::invalid_argument("clip input frame geometry or payload size changed");
  }
}

std::chrono::nanoseconds ValidateFrameSequence(const ClipFrameInput &previous,
                                               const ClipFrameInput &current) {
  const FrameMetadata &before = previous.frame.metadata;
  const FrameMetadata &after = current.frame.metadata;
  if (after.frame_id <= before.frame_id) {
    throw std::invalid_argument("clip frame IDs must be strictly increasing");
  }
  if (after.frame_id - before.frame_id != 1U) {
    throw std::invalid_argument("clip input contains a frame ID gap");
  }
  if (after.device_timestamp <= before.device_timestamp) {
    throw std::invalid_argument("clip device timestamps must be strictly increasing");
  }
  const auto interval = current.time_from_impact - previous.time_from_impact;
  if (interval <= std::chrono::nanoseconds::zero()) {
    throw std::invalid_argument("clip mapped frame times must be strictly increasing");
  }
  return interval;
}

double NominalFramesPerSecond(std::vector<std::int64_t> interval_ns) {
  std::ranges::sort(interval_ns);
  const std::size_t middle = interval_ns.size() / 2U;
  auto median_ns = static_cast<double>(interval_ns[middle]);
  if ((interval_ns.size() & 1U) == 0U) {
    median_ns = static_cast<double>(std::midpoint(interval_ns[middle - 1U], interval_ns[middle]));
  }
  const double nominal_fps = 1'000'000'000.0 / median_ns;
  if (!std::isfinite(nominal_fps) || nominal_fps <= 0.0) {
    throw std::invalid_argument("clip nominal frame rate is invalid");
  }
  return nominal_fps;
}

CameraTimeline ValidateAndBuildTimeline(const CameraClipInput &view) {
  const std::size_t expected_payload = ValidateViewHeader(view);
  const FrameMetadata &first = view.frames.front().frame.metadata;

  CameraTimeline timeline;
  timeline.time_from_impact_us.reserve(view.frames.size());
  timeline.media_time_us.reserve(view.frames.size());
  std::vector<std::int64_t> interval_ns;
  interval_ns.reserve(view.frames.size() - 1U);
  auto smallest_impact_distance = std::chrono::nanoseconds::max();

  for (std::size_t index = 0; index < view.frames.size(); ++index) {
    const ClipFrameInput &frame = view.frames[index];
    ValidateFrameGeometry(frame, first, expected_payload);
    if (index > 0U) {
      const ClipFrameInput &previous = view.frames[index - 1U];
      interval_ns.push_back(ValidateFrameSequence(previous, frame).count());
    }

    timeline.time_from_impact_us.push_back(RoundedMicroseconds(frame.time_from_impact));
    timeline.media_time_us.push_back(
        MediaMicroseconds(frame.time_from_impact - view.frames.front().time_from_impact));
    if (index > 0U && timeline.media_time_us[index] <= timeline.media_time_us[index - 1U]) {
      throw std::invalid_argument("clip frame times collide at microsecond media precision");
    }

    const auto impact_distance = std::chrono::abs(frame.time_from_impact);
    if (impact_distance < smallest_impact_distance) {
      smallest_impact_distance = impact_distance;
      timeline.impact_frame_index = index;
    }
  }
  if (view.frames.front().time_from_impact > std::chrono::nanoseconds::zero() ||
      view.frames.back().time_from_impact < std::chrono::nanoseconds::zero()) {
    throw std::invalid_argument("clip view must bracket the impact time");
  }

  timeline.nominal_fps = NominalFramesPerSecond(std::move(interval_ns));
  return timeline;
}

std::filesystem::path CreatePendingDirectory(const std::filesystem::path &output_parent,
                                             std::string_view session_id) {
  static std::atomic_uint64_t next_suffix = 0;
  for (std::size_t attempt = 0; attempt < 1'000U; ++attempt) {
    const std::uint64_t suffix = next_suffix.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path candidate =
        output_parent / ("." + std::string(session_id) + ".pending-" + std::to_string(suffix));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      return candidate;
    }
    if (error && error != std::errc::file_exists) {
      throw std::filesystem::filesystem_error("failed to create pending clip session", candidate,
                                              error);
    }
  }
  throw std::runtime_error("could not allocate a unique pending clip session directory");
}

void WriteTextFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not open clip session file for writing: " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output) {
    throw std::runtime_error("could not write clip session file: " + path.string());
  }
}

class PendingDirectoryGuard final {
 public:
  explicit PendingDirectoryGuard(std::filesystem::path path) : path_(std::move(path)) {}
  ~PendingDirectoryGuard() {
    if (!published_) {
      std::error_code ignored;
      static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }
  }

  PendingDirectoryGuard(const PendingDirectoryGuard &) = delete;
  PendingDirectoryGuard &operator=(const PendingDirectoryGuard &) = delete;
  PendingDirectoryGuard(PendingDirectoryGuard &&) = delete;
  PendingDirectoryGuard &operator=(PendingDirectoryGuard &&) = delete;

  void MarkPublished() noexcept { published_ = true; }

 private:
  std::filesystem::path path_;
  bool published_ = false;
};

Json FrameJson(const ClipFrameInput &frame, const CameraTimeline &timeline, std::size_t index) {
  return {
      {"frame_index", index},
      {"frame_id", std::to_string(frame.frame.metadata.frame_id)},
      {"device_timestamp", std::to_string(frame.frame.metadata.device_timestamp)},
      {"time_from_impact_us", timeline.time_from_impact_us[index]},
      {"media_time_us", timeline.media_time_us[index]},
  };
}

Json CapturePipelineProfileJson(const ClipCapturePipelineProfile &profile) {
  return {
      {"trigger_estimate_to_confirmation_ms",
       Milliseconds(profile.trigger_estimate_to_confirmation)},
      {"confirmation_to_acceptance_ms", Milliseconds(profile.confirmation_to_acceptance)},
      {"acceptance_to_freeze_start_ms", Milliseconds(profile.acceptance_to_freeze_start)},
      {"freeze_schedule_lateness_ms", Milliseconds(profile.freeze_schedule_lateness)},
      {"freeze_and_rotate_ms", Milliseconds(profile.freeze_and_rotate)},
      {"audio_stop_ms", Milliseconds(profile.audio_stop)},
  };
}

Json SessionPipelineProfileJson(const ClipSessionPipelineProfile &profile) {
  return {
      {"prepublication_analysis_ms", profile.prepublication_analysis_ms},
      {"publisher_planning_ms", profile.publisher_planning_ms},
      {"impact_preview_render_ms", profile.impact_preview_render_ms},
      {"impact_preview_ready_after_confirmation_ms",
       profile.impact_preview_ready_after_confirmation_ms},
      {"validation_and_timeline_ms", profile.validation_and_timeline_ms},
      {"output_setup_ms", profile.output_setup_ms},
      {"media_encoding_wall_ms", profile.media_encoding_wall_ms},
      {"frame_metadata_ms", profile.frame_metadata_ms},
      {"profile_snapshot_after_confirmation_ms", profile.profile_snapshot_after_confirmation_ms},
      {"profile_snapshot_host_monotonic_ns",
       std::to_string(profile.profile_snapshot_host_monotonic_ns)},
  };
}

Json ViewPipelineProfileJson(const ClipViewPipelineProfile &profile) {
  return {
      {"role", profile.role},
      {"frame_count", profile.frame_count},
      {"timeline_ms", profile.timeline_ms},
      {"bayer_fit_demosaic_ms", profile.bayer_fit_demosaic_ms},
      {"rgb_to_yuv420_ms", profile.rgb_to_yuv420_ms},
      {"codec_encode_ms", profile.codec_encode_ms},
      {"webm_mux_ms", profile.webm_mux_ms},
      {"finalize_ms", profile.finalize_ms},
      {"output_verification_ms", profile.output_verification_ms},
      {"total_ms", profile.total_ms},
  };
}

std::int64_t MonotonicNanoseconds(ProfileClock::time_point time) {
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
  if (!std::in_range<std::int64_t>(nanoseconds)) {
    throw std::overflow_error("profile snapshot does not fit int64 nanoseconds");
  }
  return static_cast<std::int64_t>(nanoseconds);
}

struct ValidatedClipSession {
  std::array<CameraTimeline, 2> timelines;
  std::array<double, 2> timeline_ms{};
  ClipMediaEncoderCapabilities capabilities;
  double validation_and_timeline_ms = 0.0;
};

ValidatedClipSession ValidateClipSession(const DualViewClipInput &input,
                                         const ClipMediaEncoder &encoder) {
  const auto started = ProfileClock::now();
  if (!IsSafePathComponent(input.session_id)) {
    throw std::invalid_argument("session_id must be a safe nonempty path component");
  }
  if (input.created_at_utc.empty()) {
    throw std::invalid_argument("created_at_utc cannot be empty");
  }
  ValidateTrigger(input.trigger);
  if (input.views[0].role == input.views[1].role) {
    throw std::invalid_argument("dual-view clip roles must be unique");
  }
  if (input.capture_pipeline_profile.has_value()) {
    const std::int64_t confirmation_ns = RequireConfirmedTriggerTimestamp(input.trigger);
    const ClipCapturePipelineProfile &profile = *input.capture_pipeline_profile;
    ValidateCapturePipelineProfile(profile);
    const auto expected_trigger_to_confirmation =
        std::chrono::nanoseconds(confirmation_ns - input.trigger.host_monotonic_time_ns);
    const auto measured_trigger_to_confirmation =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            profile.trigger_estimate_to_confirmation);
    if (std::chrono::abs(measured_trigger_to_confirmation - expected_trigger_to_confirmation) >
        std::chrono::microseconds(1)) {
      throw std::invalid_argument(
          "trigger-estimate-to-confirmation profile does not match trigger timestamps");
    }
  }

  ValidatedClipSession validated;
  for (std::size_t view_index = 0; view_index < input.views.size(); ++view_index) {
    const auto timeline_started = ProfileClock::now();
    validated.timelines[view_index] = ValidateAndBuildTimeline(input.views[view_index]);
    validated.timeline_ms[view_index] = ElapsedMilliseconds(timeline_started);
  }
  ValidateHilEvidence(input, validated.timelines);
  validated.capabilities = encoder.capabilities();
  if (!IsSafePathComponent(validated.capabilities.file_extension) ||
      validated.capabilities.encoder_id.empty() || validated.capabilities.mime_type.empty() ||
      validated.capabilities.codec.empty()) {
    throw std::invalid_argument("clip media encoder reported invalid capabilities");
  }
  validated.validation_and_timeline_ms = ElapsedMilliseconds(started);
  return validated;
}

Json BuildManifestHeader(const DualViewClipInput &input) {
  Json manifest = {
      {"schema_version", 1},
      {"session_id", input.session_id},
      {"created_at_utc", input.created_at_utc},
      {"trigger",
       {
           {"source", input.trigger.source},
           {"host_monotonic_time_ns", std::to_string(input.trigger.host_monotonic_time_ns)},
           {"confirmation_host_monotonic_time_ns", nullptr},
           {"sample_rate_hz", nullptr},
           {"peak_amplitude", nullptr},
           {"noise_floor", nullptr},
           {"threshold", nullptr},
       }},
      {"mapped_nearest_frame_skew_us", nullptr},
      {"views", Json::array()},
  };
  if (input.trigger.confirmation_host_monotonic_time_ns.has_value()) {
    manifest["trigger"]["confirmation_host_monotonic_time_ns"] =
        std::to_string(*input.trigger.confirmation_host_monotonic_time_ns);
  }
  if (input.trigger.sample_rate_hz.has_value()) {
    manifest["trigger"]["sample_rate_hz"] = *input.trigger.sample_rate_hz;
  }
  if (input.trigger.peak_amplitude.has_value()) {
    manifest["trigger"]["peak_amplitude"] = *input.trigger.peak_amplitude;
  }
  if (input.trigger.noise_floor.has_value()) {
    manifest["trigger"]["noise_floor"] = *input.trigger.noise_floor;
  }
  if (input.trigger.threshold.has_value()) {
    manifest["trigger"]["threshold"] = *input.trigger.threshold;
  }
  if (input.mapped_nearest_frame_skew.has_value()) {
    manifest["mapped_nearest_frame_skew_us"] =
        RoundedMicroseconds(*input.mapped_nearest_frame_skew);
  }
  if (input.hil_evidence.has_value()) {
    manifest["hil_evidence"] = HilEvidenceJson(*input.hil_evidence);
  }
  return manifest;
}

struct EncodedViews {
  std::array<EncodedClipMedia, 2> media;
  std::array<std::string, 2> filenames;
  double wall_ms = 0.0;
};

EncodedClipMedia EncodeView(const std::filesystem::path &pending_directory,
                            const CameraClipInput &view, std::string_view filename,
                            const ClipMediaEncoder &encoder) {
  return encoder.Encode(view, pending_directory / filename);
}

EncodedViews EncodeViews(const std::filesystem::path &pending_directory,
                         const DualViewClipInput &input, const ValidatedClipSession &validated,
                         const ClipMediaEncoder &encoder) {
  const auto started = ProfileClock::now();
  EncodedViews output;
  for (std::size_t view_index = 0; view_index < input.views.size(); ++view_index) {
    output.filenames[view_index] =
        std::string(input.views[view_index].role) + "." + validated.capabilities.file_extension;
  }
  std::array<std::future<EncodedClipMedia>, 2> futures;
  for (std::size_t view_index = 0; view_index < input.views.size(); ++view_index) {
    futures[view_index] =
        std::async(std::launch::async, EncodeView, std::cref(pending_directory),
                   std::cref(input.views[view_index]),
                   std::string_view(output.filenames[view_index]), std::cref(encoder));
  }
  for (std::size_t view_index = 0; view_index < input.views.size(); ++view_index) {
    const CameraClipInput &view = input.views[view_index];
    EncodedClipMedia encoded = futures[view_index].get();
    if (encoded.frame_count != view.frames.size() || encoded.width == 0U || encoded.height == 0U ||
        encoded.encoded_bytes == 0U) {
      throw std::runtime_error("clip media encoder returned inconsistent output metadata");
    }
    if (validated.capabilities.all_frames_keyframes &&
        encoded.keyframe_count != encoded.frame_count) {
      throw std::runtime_error("all-intra clip encoder emitted a non-keyframe");
    }
    encoded.pipeline_profile.role = std::string(view.role);
    encoded.pipeline_profile.frame_count = encoded.frame_count;
    encoded.pipeline_profile.timeline_ms = validated.timeline_ms[view_index];
    output.media[view_index] = std::move(encoded);
  }
  output.wall_ms = ElapsedMilliseconds(started);
  return output;
}

double AppendFrameMetadata(Json &manifest, const DualViewClipInput &input,
                           const ValidatedClipSession &validated, const EncodedViews &encoded_views,
                           const std::filesystem::path &final_directory,
                           ClipSessionWriteResult &result) {
  const auto started = ProfileClock::now();
  for (std::size_t view_index = 0; view_index < input.views.size(); ++view_index) {
    const CameraClipInput &view = input.views[view_index];
    const CameraTimeline &timeline = validated.timelines[view_index];
    const EncodedClipMedia &encoded = encoded_views.media[view_index];
    Json frames = Json::array();
    for (std::size_t frame_index = 0; frame_index < view.frames.size(); ++frame_index) {
      frames.push_back(FrameJson(view.frames[frame_index], timeline, frame_index));
    }
    const FrameMetadata &source = view.frames.front().frame.metadata;
    manifest["views"].push_back({
        {"role", view.role},
        {"camera_serial", view.camera_serial},
        {"source",
         {
             {"pixel_format", view.pixel_format},
             {"width", source.width},
             {"height", source.height},
         }},
        {"encoded", {{"width", encoded.width}, {"height", encoded.height}}},
        {"frame_count", encoded.frame_count},
        {"nominal_fps", timeline.nominal_fps},
        {"impact_frame_index", timeline.impact_frame_index},
        {"media",
         {
             {"path", encoded_views.filenames[view_index]},
             {"mime_type", validated.capabilities.mime_type},
             {"codec", validated.capabilities.codec},
             {"all_frames_keyframes", validated.capabilities.all_frames_keyframes},
             {"encoded_bytes", encoded.encoded_bytes},
         }},
        {"frames", std::move(frames)},
    });
    result.media_paths[view_index] = final_directory / encoded_views.filenames[view_index];
  }
  return ElapsedMilliseconds(started);
}

struct PublicationMeasurements {
  double output_setup_ms = 0.0;
  double frame_metadata_ms = 0.0;
};

ClipPipelineProfile SnapshotPipelineProfile(const ClipCapturePipelineProfile &capture,
                                            std::int64_t confirmation_ns,
                                            const ValidatedClipSession &validated,
                                            const EncodedViews &encoded_views,
                                            PublicationMeasurements measurements) {
  ClipPipelineProfile profile;
  profile.capture = capture;
  profile.session.prepublication_analysis_ms =
      Milliseconds(profile.capture.prepublication_analysis);
  profile.session.publisher_planning_ms = Milliseconds(profile.capture.publisher_planning);
  profile.session.impact_preview_render_ms = Milliseconds(profile.capture.impact_preview_render);
  profile.session.impact_preview_ready_after_confirmation_ms =
      Milliseconds(profile.capture.impact_preview_ready_after_confirmation);
  profile.session.validation_and_timeline_ms = validated.validation_and_timeline_ms;
  profile.session.output_setup_ms = measurements.output_setup_ms;
  profile.session.media_encoding_wall_ms = encoded_views.wall_ms;
  profile.session.frame_metadata_ms = measurements.frame_metadata_ms;
  for (std::size_t view_index = 0; view_index < encoded_views.media.size(); ++view_index) {
    profile.views[view_index] = encoded_views.media[view_index].pipeline_profile;
  }

  const std::int64_t snapshot_ns = MonotonicNanoseconds(ProfileClock::now());
  if (snapshot_ns < confirmation_ns) {
    throw std::invalid_argument("profile snapshot precedes the trigger confirmation");
  }
  profile.session.profile_snapshot_host_monotonic_ns = snapshot_ns;
  profile.session.profile_snapshot_after_confirmation_ms =
      Milliseconds(std::chrono::nanoseconds(snapshot_ns - confirmation_ns));
  return profile;
}

Json PipelineProfileJson(const ClipPipelineProfile &profile) {
  Json views = Json::array();
  for (const ClipViewPipelineProfile &view : profile.views) {
    views.push_back(ViewPipelineProfileJson(view));
  }
  return {
      {"schema_version", ClipPipelineProfile::kSchemaVersion},
      {"capture", CapturePipelineProfileJson(profile.capture)},
      {"session", SessionPipelineProfileJson(profile.session)},
      {"views", std::move(views)},
  };
}

}  // namespace

ClipSessionWriteResult WriteClipSession(const std::filesystem::path &output_parent,
                                        const DualViewClipInput &input,
                                        const ClipMediaEncoder &encoder) {
  const ValidatedClipSession validated = ValidateClipSession(input, encoder);
  const auto output_setup_started = ProfileClock::now();
  std::filesystem::create_directories(output_parent);
  const std::filesystem::path final_directory = output_parent / input.session_id;
  if (std::filesystem::exists(final_directory)) {
    throw std::invalid_argument("clip session already exists and will not be overwritten");
  }
  const std::filesystem::path pending_directory =
      CreatePendingDirectory(output_parent, input.session_id);
  PendingDirectoryGuard pending_guard(pending_directory);
  const double output_setup_ms = ElapsedMilliseconds(output_setup_started);
  Json manifest = BuildManifestHeader(input);
  ClipSessionWriteResult result{
      .session_directory = final_directory,
      .manifest_path = final_directory / "manifest.json",
      .media_paths = {},
      .pipeline_profile = std::nullopt,
  };
  const EncodedViews encoded_views = EncodeViews(pending_directory, input, validated, encoder);
  const double frame_metadata_ms =
      AppendFrameMetadata(manifest, input, validated, encoded_views, final_directory, result);

  std::optional<ClipPipelineProfile> pipeline_profile;
  if (input.capture_pipeline_profile.has_value()) {
    pipeline_profile = SnapshotPipelineProfile(
        input.capture_pipeline_profile.value(), RequireConfirmedTriggerTimestamp(input.trigger),
        validated, encoded_views,
        {.output_setup_ms = output_setup_ms, .frame_metadata_ms = frame_metadata_ms});
    manifest["pipeline_profile"] = PipelineProfileJson(*pipeline_profile);
  }

  WriteTextFile(pending_directory / "manifest.json", manifest.dump(2) + "\n");
  std::error_code rename_error;
  std::filesystem::rename(pending_directory, final_directory, rename_error);
  if (rename_error) {
    throw std::filesystem::filesystem_error("failed to publish completed clip session",
                                            pending_directory, final_directory, rename_error);
  }
  pending_guard.MarkPublished();
  result.pipeline_profile = std::move(pipeline_profile);
  return result;
}

}  // namespace swing_capture::encoding
