#include "capture/application/clip_session_publisher.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/application/capture_controller.h"
#include "capture/clip/clip_window_planner.h"
#include "capture/core/camera_source.h"
#include "capture/core/device_clock_mapper.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/encoding/clip_session.h"
#include "capture/image/bayer_rg8.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::application {
namespace {

struct PlannedCameraInput {
  std::vector<FrameTiming> timings;
  DeviceClockMapper mapper;
  CameraClipPlan plan;
  std::vector<encoding::ClipFrameInput> selected_frames;

  explicit PlannedCameraInput(std::uint64_t ticks_per_second) : mapper(ticks_per_second) {}
};

PlannedCameraInput PlanCamera(const CapturedCameraWindow &camera, const ImpactEvent &impact,
                              std::chrono::steady_clock::duration pre_roll,
                              std::chrono::steady_clock::duration post_roll) {
  PlannedCameraInput planned(camera.device_ticks_per_second);
  planned.timings.reserve(camera.frames.size());
  for (const PooledRawFrameHandle &frame : camera.frames.frames()) {
    const FrameMetadata &metadata = frame.metadata();
    planned.timings.push_back(
        {.frame_id = metadata.frame_id, .device_timestamp = metadata.device_timestamp});
    planned.mapper.AddSample(metadata.device_timestamp, metadata.host_received_at);
  }
  planned.plan = PlanCameraClipWindow(planned.timings, planned.mapper, impact.strike_time, pre_roll,
                                      post_roll);
  if (!planned.plan.ok() || !planned.plan.frame_range.has_value()) {
    throw std::runtime_error("cannot publish camera " + camera.role + ": " +
                             std::string(ClipPlanFailureMessage(planned.plan.failure)));
  }
  const FrameIndexRange range = planned.plan.frame_range.value();
  planned.selected_frames.reserve(range.end_index_exclusive - range.begin_index);
  for (std::size_t index = range.begin_index; index < range.end_index_exclusive; ++index) {
    const PooledRawFrameHandle &frame = camera.frames.at(index);
    const auto mapped = planned.mapper.EstimateHostTime(frame.metadata().device_timestamp);
    if (!mapped.has_value()) {
      throw std::runtime_error("selected camera frame is not mappable to host time");
    }
    planned.selected_frames.push_back({
        .frame = frame.view(),
        .time_from_impact = std::chrono::duration_cast<std::chrono::nanoseconds>(
            mapped.value() - impact.strike_time),
    });
  }
  return planned;
}

std::int64_t HostNanoseconds(std::chrono::steady_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

encoding::ClipTriggerMetadata TriggerMetadata(const CapturedTrigger &trigger) {
  encoding::ClipTriggerMetadata metadata = {
      .source = CaptureTriggerSourceName(trigger.source),
      .host_monotonic_time_ns = HostNanoseconds(trigger.impact.strike_time),
      .confirmation_host_monotonic_time_ns = HostNanoseconds(trigger.impact.confirmation_time),
      .sample_rate_hz = std::nullopt,
      .peak_amplitude = std::nullopt,
      .noise_floor = std::nullopt,
      .threshold = std::nullopt,
  };
  if (trigger.source == CaptureTriggerSource::kAudio) {
    metadata.sample_rate_hz = trigger.impact.sample_rate_hz;
    metadata.peak_amplitude = trigger.impact.peak_amplitude;
    metadata.noise_floor = trigger.impact.noise_floor_at_detection;
    metadata.threshold = trigger.impact.threshold_at_detection;
  }
  return metadata;
}

encoding::ClipCapturePipelineProfile CapturePipelineProfile(
    const CapturedSession &captured, std::chrono::steady_clock::duration prepublication_analysis,
    std::chrono::steady_clock::duration publisher_planning) {
  const CapturePipelineTiming &timing = captured.pipeline_timing;
  if (timing.freeze_started_at < timing.trigger_accepted_at ||
      timing.freeze_completed_at < timing.freeze_started_at ||
      timing.audio_stop_started_at < timing.freeze_completed_at ||
      timing.audio_stop_completed_at < timing.audio_stop_started_at ||
      prepublication_analysis < std::chrono::steady_clock::duration::zero() ||
      publisher_planning < std::chrono::steady_clock::duration::zero()) {
    throw std::invalid_argument("capture pipeline timing boundaries are inconsistent");
  }
  return {
      .trigger_estimate_to_confirmation =
          captured.trigger.impact.confirmation_time - captured.trigger.impact.strike_time,
      .confirmation_to_acceptance =
          timing.trigger_accepted_at - captured.trigger.impact.confirmation_time,
      .acceptance_to_freeze_start = timing.freeze_started_at - timing.trigger_accepted_at,
      .freeze_schedule_lateness = timing.freeze_started_at - timing.freeze_target_at,
      .freeze_and_rotate = timing.freeze_completed_at - timing.freeze_started_at,
      .audio_stop = timing.audio_stop_completed_at - timing.audio_stop_started_at,
      .prepublication_analysis = prepublication_analysis,
      .publisher_planning = publisher_planning,
  };
}

ImpactPreviewImage RenderImpactPreview(const CapturedCameraWindow &camera,
                                       const PlannedCameraInput &planned) {
  if (!planned.plan.strike_frame.has_value()) {
    throw std::logic_error("impact preview requires a selected strike frame");
  }
  const PooledRawFrameHandle &frame = camera.frames.at(planned.plan.strike_frame->frame_index);
  const FrameView view = frame.view();
  const image::Rgb8Image rgb =
      image::DemosaicBayerRg8(view.payload, view.metadata.width, view.metadata.height);
  return {
      .role = camera.role,
      .frame_id = view.metadata.frame_id,
      .time_from_impact_us =
          std::chrono::round<std::chrono::microseconds>(planned.plan.strike_frame->time_error)
              .count(),
      .width = rgb.width,
      .height = rgb.height,
      .media_type = "image/jpeg",
      .encoded_bytes = image::EncodeJpeg(rgb, 90),
  };
}

struct ImpactPreviewMeasurements {
  std::chrono::steady_clock::duration render{};
  std::chrono::steady_clock::duration ready_after_confirmation{};
};

ImpactPreviewMeasurements PublishImpactPreviews(const ClipSessionPublisherConfig &config,
                                                const CapturedSession &captured,
                                                const std::array<PlannedCameraInput, 2> &planned) {
  if (!config.impact_preview_ready) {
    return {};
  }
  const auto started_at = std::chrono::steady_clock::now();
  auto first = std::async(std::launch::async, RenderImpactPreview, std::cref(captured.cameras[0]),
                          std::cref(planned[0]));
  ImpactPreviewImage second = RenderImpactPreview(captured.cameras[1], planned[1]);
  config.impact_preview_ready(captured.identity, std::array{first.get(), std::move(second)});
  const auto ready_at = std::chrono::steady_clock::now();
  if (ready_at < captured.trigger.impact.confirmation_time) {
    throw std::logic_error("impact preview became ready before trigger confirmation");
  }
  return {
      .render = ready_at - started_at,
      .ready_after_confirmation = ready_at - captured.trigger.impact.confirmation_time,
  };
}

const SyntheticSwingCameraEvidence &CameraHilEvidence(const SyntheticSwingSessionEvidence &evidence,
                                                      std::string_view role) {
  for (const SyntheticSwingCameraEvidence &camera : evidence.cameras) {
    if (camera.role == role) {
      return camera;
    }
  }
  throw std::invalid_argument("synthetic swing evidence is missing camera role " +
                              std::string(role));
}

encoding::SyntheticSwingHilViewEvidence EncodeCameraHilEvidence(
    const SyntheticSwingCameraEvidence &camera,
    const std::vector<encoding::ClipFrameInput> &selected_frames) {
  std::optional<std::size_t> optical_index;
  for (std::size_t index = 0; index < selected_frames.size(); ++index) {
    if (selected_frames[index].frame.metadata.frame_id == camera.optical_white_impact_frame_id) {
      optical_index = index;
      break;
    }
  }
  if (!optical_index.has_value()) {
    throw std::invalid_argument("synthetic swing optical white frame is outside the encoded clip");
  }
  const auto optical_time_from_audio = std::chrono::round<std::chrono::microseconds>(
                                           selected_frames[*optical_index].time_from_impact)
                                           .count();
  return {
      .role = camera.role,
      .optical_white_impact_frame_index = *optical_index,
      .mapped_time_correction_us = camera.mapped_time_correction_us,
      .schedule_uncertainty_us = camera.schedule_uncertainty_us,
      .audio_trigger_estimate_offset_us = -optical_time_from_audio,
      .optical_white_passed = camera.optical_white_passed,
      .stable_frame_count = camera.stable_frame_count,
      .matching_frame_count = camera.matching_frame_count,
      .matching_fraction = camera.matching_fraction,
      .mean_signal_delta = camera.mean_signal_delta,
      .mean_expected_color_distance = camera.mean_expected_color_distance,
      .maximum_saturated_fraction = camera.maximum_saturated_fraction,
      .maximum_bloom_fraction = camera.maximum_bloom_fraction,
      .exposure_us = camera.exposure_us,
      .gain_db = camera.gain_db,
  };
}

encoding::SyntheticSwingHilEvidence EncodeHilEvidence(
    const SyntheticSwingSessionEvidence &evidence,
    const std::array<PlannedCameraInput, 2> &planned) {
  const SyntheticSwingCameraEvidence &first = CameraHilEvidence(evidence, "down_the_line");
  const SyntheticSwingCameraEvidence &second = CameraHilEvidence(evidence, "face_on");
  return {
      .selected_brightness = evidence.selected_brightness,
      .step_duration_us = evidence.step_duration_us,
      .pre_impact_step_count = evidence.pre_impact_step_count,
      .white_impact_duration_us = evidence.white_impact_duration_us,
      .post_impact_step_count = evidence.post_impact_step_count,
      .tone_duration_us = evidence.tone_duration_us,
      .tone_frequency_hz = evidence.tone_frequency_hz,
      .views =
          std::array{
              EncodeCameraHilEvidence(first, planned[0].selected_frames),
              EncodeCameraHilEvidence(second, planned[1].selected_frames),
          },
  };
}

}  // namespace

ClipSessionPublisher::ClipSessionPublisher(ClipSessionPublisherConfig config,
                                           std::unique_ptr<encoding::ClipMediaEncoder> encoder)
    : config_(std::move(config)), encoder_(std::move(encoder)) {
  if (config_.output_root.empty()) {
    throw std::invalid_argument("clip session output root cannot be empty");
  }
  if (config_.pre_roll <= std::chrono::steady_clock::duration::zero() ||
      config_.post_roll < std::chrono::steady_clock::duration::zero()) {
    throw std::invalid_argument("clip session roll durations are invalid");
  }
  if (encoder_ == nullptr) {
    throw std::invalid_argument("clip session publisher requires an encoder");
  }
}

PublishedSession ClipSessionPublisher::Publish(
    CapturedSession captured, std::optional<SyntheticSwingSessionEvidence> hil_evidence) const {
  const auto publisher_started_at = std::chrono::steady_clock::now();
  std::array<PlannedCameraInput, 2> planned = {
      PlanCamera(captured.cameras[0], captured.trigger.impact, config_.pre_roll, config_.post_roll),
      PlanCamera(captured.cameras[1], captured.trigger.impact, config_.pre_roll, config_.post_roll),
  };
  const DualViewClipPlan dual_plan = PlanDualViewClipWindow(
      captured.trigger.impact, planned[0].timings, planned[0].mapper, planned[1].timings,
      planned[1].mapper, config_.pre_roll, config_.post_roll);
  if (!dual_plan.ok()) {
    throw std::runtime_error("dual-view clip plan changed while publishing");
  }
  const ImpactPreviewMeasurements impact_preview =
      PublishImpactPreviews(config_, captured, planned);

  std::optional<std::chrono::nanoseconds> mapped_skew;
  if (dual_plan.mapped_nearest_frame_skew.has_value()) {
    mapped_skew = std::chrono::duration_cast<std::chrono::nanoseconds>(
        dual_plan.mapped_nearest_frame_skew.value());
  }
  std::optional<encoding::SyntheticSwingHilEvidence> encoded_hil_evidence;
  if (hil_evidence.has_value()) {
    encoded_hil_evidence = EncodeHilEvidence(*hil_evidence, planned);
  }
  const auto publisher_planning = std::chrono::steady_clock::now() - publisher_started_at;
  const auto prepublication_analysis =
      publisher_started_at - captured.pipeline_timing.audio_stop_completed_at;
  encoding::ClipCapturePipelineProfile capture_profile =
      CapturePipelineProfile(captured, prepublication_analysis, publisher_planning);
  capture_profile.impact_preview_render = impact_preview.render;
  capture_profile.impact_preview_ready_after_confirmation = impact_preview.ready_after_confirmation;
  const encoding::DualViewClipInput input = {
      .session_id = captured.identity.session_id,
      .created_at_utc = captured.identity.created_at_utc,
      .trigger = TriggerMetadata(captured.trigger),
      .views =
          std::array{
              encoding::CameraClipInput{
                  .role = captured.cameras[0].role,
                  .camera_serial = captured.cameras[0].serial,
                  .pixel_format = "BayerRG8",
                  .frames = planned[0].selected_frames,
              },
              encoding::CameraClipInput{
                  .role = captured.cameras[1].role,
                  .camera_serial = captured.cameras[1].serial,
                  .pixel_format = "BayerRG8",
                  .frames = planned[1].selected_frames,
              },
          },
      .mapped_nearest_frame_skew = mapped_skew,
      .hil_evidence = encoded_hil_evidence,
      .capture_pipeline_profile = capture_profile,
  };
  const encoding::ClipSessionWriteResult written =
      encoding::WriteClipSession(config_.output_root, input, *encoder_);
  return {
      .session_id = captured.identity.session_id,
      .created_at_utc = captured.identity.created_at_utc,
      .manifest_path =
          std::filesystem::relative(written.manifest_path, config_.output_root).generic_string(),
  };
}

}  // namespace swing_capture::application
