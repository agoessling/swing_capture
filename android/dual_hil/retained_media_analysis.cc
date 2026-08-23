#include "android/dual_hil/retained_media_analysis.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>  // NOLINT(misc-include-cleaner) - POSIX kill uses SIGKILL.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner) - Json::dump requires the full type.
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "android/dual_hil/dual_session_validation.h"
#include "android/dual_hil/hil_command.h"
#include "android/dual_hil/rgb_swing_analysis.h"
#include "capture/image/image_quality.h"
#include "capture/optical/april_tag.h"

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr auto kStageDeadline = std::chrono::seconds(15);

std::string SystemErrorMessage(const int error) {
  return std::error_code(error, std::generic_category()).message();
}

std::int64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               started)
      .count();
}

void WriteArtifact(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open dual Android HIL artifact " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    throw std::runtime_error("cannot write dual Android HIL artifact " + path.string());
  }
}

struct DecodeRequest {
  std::filesystem::path ffmpeg;
  std::filesystem::path media;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::chrono::steady_clock::time_point deadline;
};

// This linear lifecycle keeps every descriptor and child cleanup path visible at the POSIX
// boundary. Splitting it would obscure the timeout/error ownership without simplifying callers.
// NOLINTBEGIN(misc-include-cleaner) - include-cleaner cannot attribute several POSIX declarations.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void DecodeRgbVideo(const DecodeRequest &request, RgbSwingSequenceAnalyzer *analyzer) {
  std::array<int, 2> output_pipe = {-1, -1};
  if (pipe2(output_pipe.data(), O_CLOEXEC) != 0) {
    throw std::runtime_error("cannot create decoder pipe: " + SystemErrorMessage(errno));
  }
  const auto child = fork();
  if (child < 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error("cannot fork ffmpeg: " + SystemErrorMessage(saved_errno));
  }
  if (child == 0) {
    close(output_pipe[0]);
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    close(output_pipe[1]);
    const std::string filter = "scale=" + std::to_string(request.output_width) + ":" +
                               std::to_string(request.output_height) + ":flags=area";
    std::vector<std::string> arguments = {
        request.ffmpeg.string(),
        "-v",
        "error",
        "-nostdin",
        "-xerror",
        "-err_detect",
        "explode",
        "-i",
        request.media.string(),
        "-an",
        "-sn",
        "-dn",
        "-vf",
        filter,
        "-fps_mode",
        "passthrough",
        "-enc_time_base",
        "demux",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
    };
    std::vector<char *> command;
    command.reserve(arguments.size() + 1U);
    for (std::string &argument : arguments) {
      command.push_back(argument.data());
    }
    command.push_back(nullptr);
    execv(request.ffmpeg.c_str(), command.data());
    _exit(127);
  }

  close(output_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (flags >= 0) {
    // POSIX fcntl exposes its command-dependent third argument through varargs.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    static_cast<void>(fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK));
  }
  if (request.output_width == 0U || request.output_height == 0U ||
      static_cast<std::size_t>(request.output_width) >
          std::numeric_limits<std::size_t>::max() /
              static_cast<std::size_t>(request.output_height) / 3U) {
    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    close(output_pipe[0]);
    throw std::invalid_argument("ffmpeg analysis geometry is invalid");
  }
  const std::size_t frame_bytes =
      static_cast<std::size_t>(request.output_width) * request.output_height * 3U;
  std::vector<std::uint8_t> frame(frame_bytes);
  std::size_t filled = 0U;
  bool end_of_output = false;
  while (!end_of_output) {
    const std::span frame_span(frame);
    const auto destination = frame_span.subspan(filled);
    const auto count = read(output_pipe[0], destination.data(), destination.size());
    if (count > 0) {
      filled += static_cast<std::size_t>(count);
      if (filled == frame.size()) {
        analyzer->Append(frame);
        filled = 0U;
      }
      continue;
    }
    if (count == 0) {
      end_of_output = true;
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error("cannot read ffmpeg output: " + SystemErrorMessage(errno));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= request.deadline) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error("ffmpeg RGB decode exceeded its 15-second stage deadline");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(request.deadline - now);
    pollfd descriptor = {.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    const int polled =
        poll(&descriptor, 1,
             static_cast<int>(std::min(remaining, std::chrono::milliseconds(50)).count()));
    if (polled < 0 && errno != EINTR) {
      static_cast<void>(kill(child, SIGKILL));
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
      close(output_pipe[0]);
      throw std::runtime_error("cannot poll ffmpeg output: " + SystemErrorMessage(errno));
    }
  }
  close(output_pipe[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  // The wait-status accessors are POSIX macros that include-cleaner cannot attribute.
  if (filled != 0U || !WIFEXITED(status) ||  // NOLINT(misc-include-cleaner)
      WEXITSTATUS(status) != 0) {            // NOLINT(misc-include-cleaner)
    throw std::runtime_error("ffmpeg did not produce a whole successful RGB24 frame stream");
  }
}
// NOLINTEND(misc-include-cleaner)

struct DecodedLuminanceImage {
  std::span<const std::byte> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

swing_capture::optical::AprilTagDetection DetectPrintedTag(const DecodedLuminanceImage &luminance) {
  const swing_capture::image::Raw8ImageView image = {
      .pixels = luminance.pixels,
      .width = luminance.width,
      .height = luminance.height,
      .row_stride_bytes = luminance.width,
  };
  swing_capture::optical::AprilTagDetector detector(
      {.family = swing_capture::optical::AprilTagFamily::kTag36h11});
  auto presence = swing_capture::optical::VerifyAprilTagPresence(
      image, detector, {.expected_id = 0, .maximum_hamming = 1, .minimum_decision_margin = 10.0});
  if (presence.present && presence.accepted_detection.has_value()) {
    return presence.accepted_detection.value();
  }
  throw std::runtime_error("retained H.264 lacks required tag36h11 ID 0: " + presence.diagnostic);
}

std::size_t PostDiagnosticFrame(const RgbSwingAnalysis &optical, std::size_t frame_count) {
  return std::min(optical.last_white_frame_index + 5U, frame_count - 1U);
}

const std::vector<std::byte> &FindDiagnosticLuminance(const RgbSwingAnalysis &optical,
                                                      std::size_t frame_index) {
  const auto found = std::ranges::find(optical.diagnostic_luminance_frames, frame_index,
                                       &RetainedLuminanceFrame::frame_index);
  if (found == optical.diagnostic_luminance_frames.end()) {
    throw std::runtime_error("decoded RGB analyzer did not retain a diagnostic luminance frame");
  }
  return found->pixels;
}

// All paths have distinct roles fixed by the ffmpeg invocation below.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void ExtractDiagnosticPngs(const std::filesystem::path &ffmpeg,
                           const std::filesystem::path &media_path,
                           const std::filesystem::path &node_output,
                           const RgbSwingAnalysis &optical, std::size_t frame_count) {
  const std::size_t post_frame = PostDiagnosticFrame(optical, frame_count);
  const std::string selection = "select=eq(n\\," +
                                std::to_string(optical.representative_frame_index) + ")+eq(n\\," +
                                std::to_string(optical.first_white_frame_index) + ")+eq(n\\," +
                                std::to_string(post_frame) + ")";
  const std::filesystem::path output_pattern = node_output / "diagnostic-%02d.png";
  static_cast<void>(
      RunRequiredHilCommand(ffmpeg,
                            {"-v", "error", "-nostdin", "-xerror", "-err_detect", "explode", "-y",
                             "-i", media_path.string(), "-vf", selection, "-fps_mode",
                             "passthrough", "-frames:v", "3", output_pattern.string()},
                            std::chrono::steady_clock::now() + kStageDeadline));
  for (int index = 1; index <= 3; ++index) {
    const std::filesystem::path diagnostic =
        node_output / ("diagnostic-0" + std::to_string(index) + ".png");
    if (!std::filesystem::is_regular_file(diagnostic) ||
        std::filesystem::file_size(diagnostic) == 0U) {
      throw std::runtime_error("ffmpeg did not publish all three diagnostic PNG frames");
    }
  }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

MediaAnalysisGeometry SelectMediaAnalysisGeometry(const NodeEvidence &evidence,
                                                  std::uint32_t maximum_width) {
  if (evidence.width <= 0 || evidence.height <= 0 || maximum_width == 0U) {
    throw std::invalid_argument("manifest geometry cannot produce a decoded analysis image");
  }
  const auto source_width = static_cast<std::uint32_t>(evidence.width);
  const auto source_height = static_cast<std::uint32_t>(evidence.height);
  const std::uint32_t analysis_width = std::min(maximum_width, source_width);
  const auto analysis_height = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(source_height) * analysis_width / source_width);
  if (analysis_height == 0U) {
    throw std::invalid_argument("manifest geometry cannot produce a decoded analysis image");
  }
  return {.width = analysis_width, .height = analysis_height};
}

Json RetainedMediaAnalysisEvidenceJson(const RetainedMediaAnalysis &analysis,
                                       const NodeEvidence &evidence) {
  const RgbSwingAnalysis &optical = analysis.optical;
  const RgbSwingAcceptanceEvidence &acceptance = optical.acceptance;
  Json april_tag_frames = Json::array();
  Json diagnostic_frames = Json::array();
  for (std::size_t index = 0; index < analysis.april_tags.size(); ++index) {
    const auto &tag = analysis.april_tags[index];
    april_tag_frames.push_back({
        {"frame_index", analysis.diagnostic_frame_indices[index]},
        {"family", tag.family},
        {"id", tag.id},
        {"hamming", tag.hamming},
        {"decision_margin", tag.decision_margin},
    });
    diagnostic_frames.push_back({
        {"frame_index", analysis.diagnostic_frame_indices[index]},
        {"path", evidence.role + "/diagnostic-0" + std::to_string(index + 1U) + ".png"},
    });
  }
  return {
      {"schema_version", 1},
      {"report_type", "android_retained_media_analysis"},
      {"passed", optical.detected},
      {"role", evidence.role},
      {"timing_claim",
       {{"scope", "fixture_optical_marker_to_audio_trigger"},
        {"qualification", "operational_correlation_only"},
        {"absolute_ball_impact_calibrated", false}}},
      {"manifest_timing_inputs",
       {{"trigger_timestamp_uncertainty_ns", evidence.trigger_timestamp_uncertainty_ns},
        {"local_nearest_frame_residual_us", evidence.local_nearest_frame_residual_us}}},
      {"decoded_video",
       {{"passed", optical.decoded_frame_count == evidence.frame_count},
        {"decoded_frame_count", optical.decoded_frame_count},
        {"manifest_frame_count", evidence.frame_count},
        {"maximum_media_time_residual_us", analysis.maximum_media_time_residual_us},
        {"analysis_width", analysis.analysis_width},
        {"analysis_height", analysis.analysis_height}}},
      {"optical",
       {{"passed", optical.detected},
        {"diagnostic", optical.diagnostic},
        {"peak_frame_index", optical.peak_frame_index},
        {"first_white_frame_index", optical.first_white_frame_index},
        {"last_white_frame_index", optical.last_white_frame_index},
        {"white_frame_count", optical.white_frame_count},
        {"maximum_white_delta", optical.maximum_white_delta},
        {"white_duration_us", optical.white_duration_us},
        {"optical_to_audio_offset_us", optical.optical_to_audio_offset_us},
        {"optical_onset_lower_bound_us", optical.optical_onset_lower_bound_us},
        {"optical_onset_upper_bound_us", optical.optical_onset_upper_bound_us},
        {"localized_response_tile_count", optical.localized_response_tile_count},
        {"post_sequence_baseline_shift", optical.post_sequence_baseline_shift},
        {"acceptance",
         {{"policy",
           {{"minimum_white_delta", acceptance.minimum_white_delta},
            {"minimum_white_frames", acceptance.minimum_white_frames},
            {"maximum_white_frames", acceptance.maximum_white_frames},
            {"minimum_white_duration_us", acceptance.minimum_white_duration_us},
            {"maximum_white_duration_us", acceptance.maximum_white_duration_us},
            {"maximum_absolute_optical_audio_offset_us",
             acceptance.maximum_absolute_optical_audio_offset_us},
            {"maximum_localized_response_tile_count",
             acceptance.maximum_localized_response_tile_count}}},
          {"checks",
           {{"minimum_white_delta_passed", acceptance.minimum_white_delta_passed},
            {"white_frame_count_passed", acceptance.white_frame_count_passed},
            {"white_duration_passed", acceptance.white_duration_passed},
            {"optical_audio_offset_passed", acceptance.optical_audio_offset_passed},
            {"localized_response_passed", acceptance.localized_response_passed}}}}}}},
      {"april_tag",
       {{"passed", true},
        {"required_family", "tag36h11"},
        {"required_id", 0},
        {"persistent_pre_marker_post", true},
        {"frames", std::move(april_tag_frames)}}},
      {"diagnostic_frames", std::move(diagnostic_frames)},
      {"stages",
       {{"ffprobe_ms", analysis.ffprobe_stage_milliseconds},
        {"decode_ms", analysis.decode_stage_milliseconds},
        {"diagnostic_ms", analysis.diagnostic_stage_milliseconds}}},
  };
}

RetainedMediaAnalysis AnalyzeRetainedMedia(const RetainedMediaAnalysisRequest &request) {
  RetainedMediaAnalysis result;
  const NodeEvidence &evidence = request.evidence.get();
  const MediaAnalysisGeometry geometry = SelectMediaAnalysisGeometry(evidence);
  result.analysis_width = geometry.width;
  result.analysis_height = geometry.height;

  auto stage_started = std::chrono::steady_clock::now();
  const std::string ffprobe = RunRequiredHilCommand(
      request.ffprobe,
      {"-v", "error", "-select_streams", "v:0", "-show_entries",
       "stream=width,height,codec_name,nb_frames:frame=best_effort_timestamp_time", "-of", "json",
       request.media_path.string()},
      std::chrono::steady_clock::now() + kStageDeadline);
  WriteArtifact(request.node_output / "ffprobe.json", ffprobe);
  result.maximum_media_time_residual_us = ValidateFfprobeTimeline(evidence, ffprobe);
  result.ffprobe_stage_milliseconds = ElapsedMilliseconds(stage_started);

  std::vector<RgbFrameTiming> timings;
  timings.reserve(evidence.frame_count);
  for (std::size_t index = 0; index < evidence.frame_count; ++index) {
    timings.push_back({
        .media_time_us = evidence.media_times_us[index],
        .time_from_impact_us = evidence.times_from_impact_us[index],
    });
  }
  RgbSwingSequenceAnalyzer analyzer(RgbSwingSequenceConfiguration{
      .width = result.analysis_width,
      .height = result.analysis_height,
      .timings = timings,
  });
  stage_started = std::chrono::steady_clock::now();
  DecodeRgbVideo({.ffmpeg = request.ffmpeg,
                  .media = request.media_path,
                  .output_width = result.analysis_width,
                  .output_height = result.analysis_height,
                  .deadline = std::chrono::steady_clock::now() + kStageDeadline},
                 &analyzer);
  result.optical = analyzer.Finish();
  result.decode_stage_milliseconds = ElapsedMilliseconds(stage_started);

  stage_started = std::chrono::steady_clock::now();
  ExtractDiagnosticPngs(request.ffmpeg, request.media_path, request.node_output, result.optical,
                        evidence.frame_count);
  result.diagnostic_frame_indices = {
      result.optical.representative_frame_index,
      result.optical.first_white_frame_index,
      PostDiagnosticFrame(result.optical, evidence.frame_count),
  };
  std::array<AprilTagFrameEvidence, 3> tag_evidence;
  for (std::size_t index = 0; index < result.diagnostic_frame_indices.size(); ++index) {
    const std::size_t frame_index = result.diagnostic_frame_indices[index];
    result.april_tags[index] = DetectPrintedTag({
        .pixels = FindDiagnosticLuminance(result.optical, frame_index),
        .width = result.analysis_width,
        .height = result.analysis_height,
    });
    const auto &tag = result.april_tags[index];
    tag_evidence[index] = {
        .frame_index = frame_index,
        .family = tag.family,
        .id = tag.id,
        .hamming = tag.hamming,
        .decision_margin = tag.decision_margin,
    };
  }
  ValidateRequiredAprilTagPersistence(tag_evidence);
  result.diagnostic_stage_milliseconds = ElapsedMilliseconds(stage_started);
  WriteArtifact(request.node_output / "retained-media-analysis.json",
                RetainedMediaAnalysisEvidenceJson(result, evidence).dump(2) + "\n");
  if (!result.optical.detected) {
    throw std::runtime_error(
        "retained H.264 Feather LED/audio correlation failed: " + result.optical.diagnostic +
        "; evidence=" + evidence.role + "/retained-media-analysis.json");
  }
  return result;
}

}  // namespace swing_capture::android::dual_hil
