#include "android/dual_hil/retained_media_analysis.h"

#include <cassert>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace swing_capture::android::dual_hil {
namespace {

void DownscalesQualifiedCaptureWithoutChangingAspectRatio() {
  NodeEvidence evidence;
  evidence.width = 1280;
  evidence.height = 720;

  const MediaAnalysisGeometry geometry = SelectMediaAnalysisGeometry(evidence);

  assert(geometry.width == 640U);
  assert(geometry.height == 360U);
}

void LeavesSmallerGeometryAtNativeResolution() {
  NodeEvidence evidence;
  evidence.width = 320;
  evidence.height = 240;

  const MediaAnalysisGeometry geometry = SelectMediaAnalysisGeometry(evidence);

  assert(geometry.width == 320U);
  assert(geometry.height == 240U);
}

void RejectsGeometryThatCannotProduceAnImage() {
  NodeEvidence evidence;
  evidence.width = 1280;
  evidence.height = 1;

  bool rejected = false;
  try {
    static_cast<void>(SelectMediaAnalysisGeometry(evidence, 1U));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

void RetainsMachineReadableReasonForStrictPointTimingFailure() {
  NodeEvidence evidence;
  evidence.role = "down_the_line";
  evidence.frame_count = 49U;
  evidence.trigger_timestamp_uncertainty_ns = 277241L;
  evidence.local_nearest_frame_residual_us = 101L;

  RetainedMediaAnalysis analysis;
  analysis.maximum_media_time_residual_us = 1;
  analysis.analysis_width = 640U;
  analysis.analysis_height = 360U;
  analysis.optical.decoded_frame_count = 49U;
  analysis.optical.diagnostic = "white LED frames=6 optical_to_audio_offset_us=-20832";
  analysis.optical.first_white_frame_index = 24U;
  analysis.optical.last_white_frame_index = 29U;
  analysis.optical.white_frame_count = 6U;
  analysis.optical.maximum_white_delta = 178.155;
  analysis.optical.white_duration_us = 25112.6;
  analysis.optical.optical_to_audio_offset_us = -20832L;
  analysis.optical.optical_onset_lower_bound_us = -24999L;
  analysis.optical.optical_onset_upper_bound_us = -16665L;
  analysis.optical.tile_x = 6U;
  analysis.optical.tile_y = 2U;
  analysis.optical.localized_response_tile_count = 9U;
  analysis.optical.acceptance = {
      .minimum_white_delta = 7.0,
      .minimum_white_frames = 2U,
      .maximum_white_frames = 9U,
      .minimum_white_duration_us = 8000.0,
      .maximum_white_duration_us = 35000.0,
      .maximum_absolute_optical_audio_offset_us = 20000L,
      .minimum_white_delta_passed = true,
      .white_frame_count_passed = true,
      .white_duration_passed = true,
      .optical_audio_offset_passed = false,
  };
  for (std::size_t index = 0; index < analysis.april_tags.size(); ++index) {
    auto &tag = analysis.april_tags[index];
    tag.family = "tag36h11";
    tag.id = 0;
    tag.hamming = 0;
    tag.decision_margin = 50.0;
    analysis.diagnostic_frame_indices[index] = 20U + index * 4U;
  }

  const nlohmann::json report = RetainedMediaAnalysisEvidenceJson(analysis, evidence);
  assert(report.at("schema_version") == 2);
  assert(report.at("passed") == false);
  assert(report.at("timing_claim").at("absolute_ball_impact_calibrated") == false);
  assert(report.at("manifest_timing_inputs").at("trigger_timestamp_uncertainty_ns") == 277241L);
  assert(report.at("decoded_video").at("passed") == true);
  const auto &acceptance = report.at("optical").at("acceptance");
  assert(acceptance.at("policy").at("maximum_absolute_optical_audio_offset_us") == 20000L);
  assert(acceptance.at("checks").at("optical_audio_offset_passed") == false);
  assert(acceptance.at("checks").at("white_frame_count_passed") == true);
  assert(acceptance.at("checks").at("white_duration_passed") == true);
  assert(!acceptance.at("policy").contains("maximum_localized_response_tile_count"));
  assert(!acceptance.at("checks").contains("localized_response_passed"));
  assert(report.at("optical").at("peak_tile").at("x") == 6U);
  assert(report.at("optical").at("peak_tile").at("y") == 2U);
  assert(report.at("optical").at("localized_response_tile_count") == 9U);
  assert(report.at("diagnostic_frames").at(1).at("path") == "down_the_line/diagnostic-02.png");
}

}  // namespace
}  // namespace swing_capture::android::dual_hil

int main() {
  swing_capture::android::dual_hil::DownscalesQualifiedCaptureWithoutChangingAspectRatio();
  swing_capture::android::dual_hil::LeavesSmallerGeometryAtNativeResolution();
  swing_capture::android::dual_hil::RejectsGeometryThatCannotProduceAnImage();
  swing_capture::android::dual_hil::RetainsMachineReadableReasonForStrictPointTimingFailure();
}
