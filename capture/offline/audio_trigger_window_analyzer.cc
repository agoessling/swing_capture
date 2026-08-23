#include "capture/offline/audio_trigger_window_analyzer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::offline {
namespace {

// NOLINTBEGIN(misc-include-cleaner)

struct Candidate {
  std::int64_t time_us;
  std::int64_t peak_ppm;
  std::int64_t noise_floor_ppm;
  std::int64_t threshold_ppm;
};

std::int64_t Ppm(float normalized) {
  return std::llround(static_cast<double>(normalized) * 1'000'000.0);
}

std::size_t FrameAtOrAfter(std::int64_t time_us, std::uint32_t sample_rate_hz) {
  const auto numerator = static_cast<std::uint64_t>(time_us) * sample_rate_hz;
  return static_cast<std::size_t>((numerator + 999'999) / 1'000'000);
}

std::size_t FrameAtOrBefore(std::int64_t time_us, std::uint32_t sample_rate_hz) {
  return static_cast<std::size_t>(static_cast<std::uint64_t>(time_us) * sample_rate_hz / 1'000'000);
}

std::vector<Candidate> DetectWindow(const DecodedMonoPcmS16Wav &audio,
                                    ImpactDetectorConfig detector_config,
                                    const AudioTriggerWindow &window) {
  const std::size_t start_frame = FrameAtOrAfter(window.start_us, audio.sample_rate_hz);
  const std::size_t end_frame =
      std::min(audio.samples.size(), FrameAtOrBefore(window.end_us, audio.sample_rate_hz));
  if (start_frame >= end_frame) {
    return {};
  }
  ImpactDetector detector(detector_config);
  constexpr std::size_t kBlockFrames = 4096;
  std::array<ImpactEvent, 32> events;
  std::vector<Candidate> candidates;
  const auto recording_origin = std::chrono::steady_clock::time_point(std::chrono::seconds(1));
  for (std::size_t offset = start_frame; offset < end_frame; offset += kBlockFrames) {
    const std::size_t count = std::min(kBlockFrames, end_frame - offset);
    const auto block_start =
        recording_origin +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(offset) / audio.sample_rate_hz));
    const auto result = detector.ProcessBlock(std::span(audio.samples).subspan(offset, count),
                                              block_start, audio.sample_rate_hz, events);
    if (result.events_dropped() != 0) {
      throw std::runtime_error("impact event buffer overflowed");
    }
    for (std::size_t event_index = 0; event_index < result.events_written; ++event_index) {
      const ImpactEvent &event = events[event_index];
      candidates.push_back({.time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                           event.strike_time - recording_origin)
                                           .count(),
                            .peak_ppm = Ppm(event.peak_amplitude),
                            .noise_floor_ppm = Ppm(event.noise_floor_at_detection),
                            .threshold_ppm = Ppm(event.threshold_at_detection)});
    }
  }
  return candidates;
}

std::string Outcome(const std::vector<Candidate> &candidates, const AudioTriggerWindow &window,
                    std::int64_t tolerance_us) {
  if (candidates.empty()) {
    return "no_candidate";
  }
  const std::int64_t error_us = candidates.front().time_us - window.target_us;
  if (std::abs(error_us) <= tolerance_us) {
    return "target_first";
  }
  return error_us < 0 ? "false_early_terminal" : "late_candidate";
}

void ValidateWindows(const DecodedMonoPcmS16Wav &audio,
                     const std::vector<AudioTriggerWindow> &windows,
                     std::int64_t target_tolerance_us) {
  if (audio.sample_rate_hz == 0 || audio.samples.empty()) {
    throw std::invalid_argument("audio cannot be empty");
  }
  if (target_tolerance_us < 0) {
    throw std::invalid_argument("target tolerance cannot be negative");
  }
  const std::int64_t duration_us =
      static_cast<std::int64_t>(audio.samples.size()) * 1'000'000 / audio.sample_rate_hz;
  std::int64_t previous_target_us = -1;
  for (const AudioTriggerWindow &window : windows) {
    if (window.id.empty() || window.start_us < 0 || window.target_us <= window.start_us ||
        window.end_us <= window.target_us || window.end_us > duration_us ||
        window.target_us <= previous_target_us) {
      throw std::invalid_argument("audio trigger window is invalid");
    }
    previous_target_us = window.target_us;
  }
}

}  // namespace

nlohmann::json AnalyzeAudioTriggerWindows(const DecodedMonoPcmS16Wav &audio,
                                          ImpactDetectorConfig detector_config,
                                          const std::vector<AudioTriggerWindow> &windows,
                                          std::int64_t target_tolerance_us) {
  ValidateWindows(audio, windows, target_tolerance_us);
  nlohmann::json window_json = nlohmann::json::array();
  std::size_t target_first_count = 0;
  std::size_t false_early_count = 0;
  std::size_t no_candidate_count = 0;
  for (const AudioTriggerWindow &window : windows) {
    const std::vector<Candidate> candidates = DetectWindow(audio, detector_config, window);
    const std::string outcome = Outcome(candidates, window, target_tolerance_us);
    target_first_count += outcome == "target_first" ? 1 : 0;
    false_early_count += outcome == "false_early_terminal" ? 1 : 0;
    no_candidate_count += outcome == "no_candidate" ? 1 : 0;
    nlohmann::json candidates_json = nlohmann::json::array();
    for (const Candidate &candidate : candidates) {
      candidates_json.push_back(
          {{"time_us", std::to_string(candidate.time_us)},
           {"error_from_target_us", std::to_string(candidate.time_us - window.target_us)},
           {"peak_amplitude_ppm", candidate.peak_ppm},
           {"noise_floor_ppm", candidate.noise_floor_ppm},
           {"threshold_ppm", candidate.threshold_ppm}});
    }
    window_json.push_back(
        {{"id", window.id},
         {"start_us", std::to_string(window.start_us)},
         {"target_us", std::to_string(window.target_us)},
         {"end_us", std::to_string(window.end_us)},
         {"outcome", outcome},
         {"candidate_count", candidates.size()},
         {"first_candidate_us", candidates.empty()
                                    ? nlohmann::json(nullptr)
                                    : nlohmann::json(std::to_string(candidates.front().time_us))},
         {"candidates", std::move(candidates_json)}});
  }
  return {{"schema_version", 1},
          {"target_tolerance_us", std::to_string(target_tolerance_us)},
          {"window_count", windows.size()},
          {"target_first_count", target_first_count},
          {"false_early_terminal_count", false_early_count},
          {"no_candidate_count", no_candidate_count},
          {"windows", std::move(window_json)}};
}

// NOLINTEND(misc-include-cleaner)

}  // namespace swing_capture::offline
