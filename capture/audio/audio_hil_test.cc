#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "capture/audio/arecord_pcm_source.h"
#include "capture/audio/audio_hil_metrics.h"
#include "capture/trigger/impact_detector.h"

namespace {

using Clock = std::chrono::steady_clock;
using swing_capture::ArecordPcmConfig;
using swing_capture::ArecordPcmSource;
using swing_capture::AudioHilEvaluation;
using swing_capture::AudioHilMeasurements;
using swing_capture::AudioHilThresholds;
using swing_capture::EvaluateAudioHil;
using swing_capture::ImpactDetector;
using swing_capture::ImpactEvent;
using swing_capture::PcmReadStatus;

constexpr std::uint32_t kSampleRateHz = 32000;
constexpr std::uint16_t kChannelCount = 2;
constexpr std::uint16_t kSelectedChannel = 0;
constexpr std::uint64_t kTargetFrames = kSampleRateHz * 3;
constexpr AudioHilThresholds kThresholds = {};

std::string JsonEscape(std::string_view input) {
  std::string output;
  for (const char character : input) {
    switch (character) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output += character;
        break;
    }
  }
  return output;
}

std::filesystem::path OutputPath() {
  const char *directory = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory == nullptr || *directory == '\0') {
    return "audio_hil_summary.json";
  }
  return std::filesystem::path(directory) / "audio_hil_summary.json";
}

void WriteReport(const std::filesystem::path &path, const AudioHilMeasurements &measurements,
                 const AudioHilThresholds &thresholds, const AudioHilEvaluation &evaluation,
                 std::uint64_t blocks, std::uint64_t impact_events,
                 std::uint64_t dropped_impact_events, std::string_view device) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("cannot open audio HIL report: " + path.string());
  }
  output << std::setprecision(10);
  output << "{\n"
         << "  \"schema_version\": 2,\n"
         << "  \"passed\": " << (evaluation.passed ? "true" : "false") << ",\n"
         << "  \"device\": \"" << JsonEscape(device) << "\",\n"
         << "  \"sample_rate_hz\": " << measurements.sample_rate_hz << ",\n"
         << "  \"channel_count\": " << kChannelCount << ",\n"
         << "  \"selected_channel\": " << kSelectedChannel << ",\n"
         << "  \"target_mono_samples\": " << measurements.target_frames << ",\n"
         << "  \"received_mono_samples\": " << measurements.received_frames << ",\n"
         << "  \"blocks\": " << blocks << ",\n"
         << "  \"nonzero_samples\": " << measurements.nonzero_samples << ",\n"
         << "  \"clipped_samples\": " << measurements.clipped_samples << ",\n"
         << "  \"peak_normalized_amplitude\": " << measurements.peak_amplitude << ",\n"
         << "  \"rms_normalized_amplitude\": " << measurements.rms_amplitude << ",\n"
         << "  \"impact_events\": " << impact_events << ",\n"
         << "  \"dropped_impact_events\": " << dropped_impact_events << ",\n"
         << "  \"elapsed_seconds\": " << measurements.elapsed_seconds << ",\n"
         << "  \"captured_audio_seconds\": " << evaluation.captured_audio_seconds << ",\n"
         << "  \"cadence_ratio\": " << evaluation.cadence_ratio << ",\n"
         << "  \"clipped_fraction\": " << evaluation.clipped_fraction << ",\n"
         << "  \"thresholds\": {\n"
         << "    \"minimum_cadence_ratio\": " << thresholds.minimum_cadence_ratio << ",\n"
         << "    \"maximum_cadence_ratio\": " << thresholds.maximum_cadence_ratio << ",\n"
         << "    \"minimum_rms_normalized_amplitude\": " << thresholds.minimum_rms_amplitude
         << ",\n"
         << "    \"minimum_peak_normalized_amplitude\": " << thresholds.minimum_peak_amplitude
         << ",\n"
         << "    \"maximum_clipped_fraction\": " << thresholds.maximum_clipped_fraction << "\n"
         << "  },\n"
         << "  \"checks\": [\n";
  for (std::size_t index = 0; index < evaluation.checks.size(); ++index) {
    const auto &check = evaluation.checks[index];
    output << "    {\"name\": \"" << JsonEscape(check.name)
           << "\", \"passed\": " << (check.passed ? "true" : "false") << ", \"message\": \""
           << JsonEscape(check.message) << "\"}"
           << (index + 1 == evaluation.checks.size() ? "\n" : ",\n");
  }
  output << "  ],\n"
         << "  \"timestamp_model\": "
            "\"host read completion minus first block duration, then "
            "sample-count continuity; ALSA/device latency is unmeasured\",\n"
         << "  \"error\": \"" << JsonEscape(measurements.source_error) << "\"\n"
         << "}\n";
  output.close();
  if (!output) {
    throw std::runtime_error("cannot write audio HIL report: " + path.string());
  }
}

}  // namespace

int main() {
  const auto report_path = OutputPath();
  ArecordPcmConfig config = {
      .capture_executable = "/usr/bin/arecord",
      .device = "hw:CARD=C925e,DEV=0",
      .sample_rate_hz = kSampleRateHz,
      .channel_count = kChannelCount,
      .selected_channel = kSelectedChannel,
      .frames_per_block = 1024,
      .read_timeout = std::chrono::seconds(2),
  };
  ArecordPcmSource source(config);
  ImpactDetector detector;
  std::array<ImpactEvent, 32> events;

  std::uint64_t frames = 0;
  std::uint64_t blocks = 0;
  std::uint64_t nonzero_samples = 0;
  std::uint64_t clipped_samples = 0;
  std::uint64_t impact_events = 0;
  std::uint64_t dropped_impact_events = 0;
  std::uint64_t squared_sample_sum = 0;
  std::uint32_t peak_magnitude = 0;
  std::string error;
  const auto start = Clock::now();

  if (source.Start(&error)) {
    while (frames < kTargetFrames) {
      auto read_result = source.ReadBlock();
      if (read_result.status != PcmReadStatus::kData) {
        error = read_result.status == PcmReadStatus::kEndOfStream
                    ? "audio stream ended before the target sample count"
                    : read_result.message;
        break;
      }
      ++blocks;
      const auto impact_result = detector.ProcessBlock(
          read_result.block.samples, read_result.block.estimated_start_time, kSampleRateHz, events);
      impact_events += impact_result.events_detected;
      dropped_impact_events += impact_result.events_dropped();

      for (const std::int16_t sample : read_result.block.samples) {
        const std::int32_t widened = sample;
        const std::uint32_t magnitude =
            static_cast<std::uint32_t>(widened < 0 ? -widened : widened);
        peak_magnitude = std::max(peak_magnitude, magnitude);
        if (magnitude != 0) {
          ++nonzero_samples;
        }
        if (magnitude >= 32767U) {
          ++clipped_samples;
        }
        squared_sample_sum += static_cast<std::uint64_t>(magnitude) * magnitude;
      }
      frames += read_result.block.samples.size();
    }
    source.Stop();
  }

  const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const double peak_amplitude = static_cast<double>(peak_magnitude) / 32768.0;
  const double rms_amplitude =
      frames == 0
          ? 0.0
          : std::sqrt(static_cast<double>(squared_sample_sum) / static_cast<double>(frames)) /
                32768.0;
  const AudioHilMeasurements measurements = {
      .sample_rate_hz = kSampleRateHz,
      .target_frames = kTargetFrames,
      .received_frames = frames,
      .nonzero_samples = nonzero_samples,
      .clipped_samples = clipped_samples,
      .peak_amplitude = peak_amplitude,
      .rms_amplitude = rms_amplitude,
      .elapsed_seconds = elapsed_seconds,
      .source_error = error,
  };
  const AudioHilEvaluation evaluation = EvaluateAudioHil(measurements, kThresholds);
  WriteReport(report_path, measurements, kThresholds, evaluation, blocks, impact_events,
              dropped_impact_events, config.device);

  std::cout << "Audio HIL " << (evaluation.passed ? "PASS" : "FAIL") << ": samples=" << frames
            << " blocks=" << blocks << " rms=" << rms_amplitude << " peak=" << peak_amplitude
            << " clipped=" << clipped_samples << " impacts=" << impact_events
            << " report=" << report_path << '\n';
  for (const auto &check : evaluation.checks) {
    std::cout << "  [" << (check.passed ? "PASS" : "FAIL") << "] " << check.name << ": "
              << check.message << '\n';
  }
  if (!evaluation.passed) {
    if (!error.empty()) {
      std::cerr << "Audio HIL source error: " << error << '\n';
    }
    return 1;
  }
  return 0;
}
