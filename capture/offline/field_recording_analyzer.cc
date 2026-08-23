#include "capture/offline/field_recording_analyzer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/trigger/impact_detector.h"

namespace swing_capture::offline {
namespace {

// nlohmann's aggregate alias is provided by the directly included public header, but this clang
// tooling version does not associate the alias with that header.
// NOLINTBEGIN(misc-include-cleaner)

constexpr std::int64_t kHistogramBinUs = 10'000;
constexpr std::size_t kMaximumAlignmentHypotheses = 24;

struct Candidate {
  std::int64_t frame_position;
  std::int64_t time_us;
  std::int64_t peak_ppm;
  std::int64_t noise_floor_ppm;
  std::int64_t threshold_ppm;
};

struct Match {
  std::size_t down_the_line_index;
  std::size_t face_on_index;
  std::int64_t residual_us;
};

struct Alignment {
  std::int64_t offset_us = 0;
  std::vector<Match> matches;
  std::int64_t total_absolute_residual_us = 0;
  std::int64_t total_peak_ppm = 0;
};

struct AlignmentTrial {
  std::int64_t offset_us;
  std::int64_t tolerance_us;
};

std::int64_t ParseDigits(std::string_view text, std::size_t offset, std::size_t length) {
  std::int64_t value = 0;
  for (std::size_t index = offset; index < offset + length; ++index) {
    if (text[index] < '0' || text[index] > '9') {
      throw std::invalid_argument("created_at_utc contains a nonnumeric component");
    }
    value = value * 10 + (text[index] - '0');
  }
  return value;
}

std::int64_t ParseFractionUs(std::string_view text) {
  if (text.size() == 20) {
    return 0;
  }
  if (text[19] != '.') {
    throw std::invalid_argument("created_at_utc has an invalid fractional second");
  }
  const std::size_t digits = text.size() - 21;
  if (digits == 0 || digits > 9) {
    throw std::invalid_argument("created_at_utc fractional precision is invalid");
  }
  std::int64_t fraction_us = 0;
  for (std::size_t index = 0; index < digits; ++index) {
    const char digit = text[20 + index];
    if (digit < '0' || digit > '9') {
      throw std::invalid_argument("created_at_utc fraction is nonnumeric");
    }
    if (index < 6) {
      fraction_us = fraction_us * 10 + (digit - '0');
    }
  }
  for (std::size_t index = std::min<std::size_t>(digits, 6); index < 6; ++index) {
    fraction_us *= 10;
  }
  return fraction_us;
}

std::int64_t PartsToUtcUs(std::string_view text) {
  if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text.back() != 'Z') {
    throw std::invalid_argument("created_at_utc is not an ISO-8601 UTC instant");
  }
  const auto year = std::chrono::year(static_cast<int>(ParseDigits(text, 0, 4)));
  const auto month = std::chrono::month(static_cast<unsigned>(ParseDigits(text, 5, 2)));
  const auto day = std::chrono::day(static_cast<unsigned>(ParseDigits(text, 8, 2)));
  const std::chrono::year_month_day date(year, month, day);
  if (!date.ok()) {
    throw std::invalid_argument("created_at_utc contains an invalid date");
  }
  const std::int64_t hour = ParseDigits(text, 11, 2);
  const std::int64_t minute = ParseDigits(text, 14, 2);
  const std::int64_t second = ParseDigits(text, 17, 2);
  if (hour > 23 || minute > 59 || second > 59) {
    throw std::invalid_argument("created_at_utc contains an invalid time");
  }
  const std::int64_t fraction_us = ParseFractionUs(text);
  const auto base = std::chrono::sys_days(date).time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(base).count() +
         hour * 3'600'000'000LL + minute * 60'000'000LL + second * 1'000'000LL + fraction_us;
}

std::int64_t RequiredDecimalString(const nlohmann::json &object, std::string_view name) {
  const std::string value = object.at(name).get<std::string>();
  if (value.empty() || !std::ranges::all_of(value, [](char character) {
        return character >= '0' && character <= '9';
      })) {
    throw std::invalid_argument(std::string(name) + " must be a decimal string");
  }
  std::size_t consumed = 0;
  const std::int64_t parsed = std::stoll(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument(std::string(name) + " is outside the supported range");
  }
  return parsed;
}

void ValidateRecording(const nlohmann::json &manifest, const DecodedMonoPcmS16Wav &audio,
                       std::string_view expected_role) {
  if (manifest.at("schema_version") != 1 || manifest.at("session_kind") != "field_recording" ||
      manifest.at("role") != expected_role) {
    throw std::invalid_argument("input is not the expected field recording role");
  }
  const auto &audio_json = manifest.at("audio");
  if (audio_json.at("encoding") != "pcm_s16le" || audio_json.at("channel_count") != 1 ||
      audio_json.at("sample_rate_hz").get<std::uint32_t>() != audio.sample_rate_hz ||
      RequiredDecimalString(audio_json, "frames") !=
          static_cast<std::int64_t>(audio.samples.size())) {
    throw std::invalid_argument("manifest audio metadata does not match the WAV");
  }
}

std::int64_t Ppm(float normalized) {
  return std::llround(static_cast<double>(normalized) * 1'000'000.0);
}

std::string PairId(std::size_t index) {
  const std::size_t ordinal = index + 1;
  if (ordinal < 10) {
    return "P00" + std::to_string(ordinal);
  }
  return ordinal < 100 ? "P0" + std::to_string(ordinal) : "P" + std::to_string(ordinal);
}

std::vector<Candidate> DetectCandidates(const DecodedMonoPcmS16Wav &audio,
                                        ImpactDetectorConfig detector_config) {
  ImpactDetector detector(detector_config);
  constexpr std::size_t kBlockFrames = 4096;
  std::array<ImpactEvent, 32> events;
  std::vector<Candidate> candidates;
  const auto origin = std::chrono::steady_clock::time_point(std::chrono::seconds(1));
  for (std::size_t offset = 0; offset < audio.samples.size(); offset += kBlockFrames) {
    const std::size_t count = std::min(kBlockFrames, audio.samples.size() - offset);
    const auto block_start =
        origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(static_cast<double>(offset) /
                                                   static_cast<double>(audio.sample_rate_hz)));
    const auto result = detector.ProcessBlock(std::span(audio.samples).subspan(offset, count),
                                              block_start, audio.sample_rate_hz, events);
    if (result.events_dropped() != 0) {
      throw std::runtime_error("impact event buffer overflowed");
    }
    for (std::size_t index = 0; index < result.events_written; ++index) {
      const ImpactEvent &event = events[index];
      const auto from_origin = event.strike_time - origin;
      const auto time_us =
          std::chrono::duration_cast<std::chrono::microseconds>(from_origin).count();
      const auto frame = static_cast<std::int64_t>(
          std::llround(std::chrono::duration<double>(from_origin).count() * audio.sample_rate_hz));
      candidates.push_back({.frame_position = frame,
                            .time_us = time_us,
                            .peak_ppm = Ppm(event.peak_amplitude),
                            .noise_floor_ppm = Ppm(event.noise_floor_at_detection),
                            .threshold_ppm = Ppm(event.threshold_at_detection)});
    }
  }
  return candidates;
}

std::vector<Match> MatchAtOffset(const std::vector<Candidate> &down_the_line,
                                 const std::vector<Candidate> &face_on, AlignmentTrial trial) {
  std::vector<Match> matches;
  std::size_t down_index = 0;
  std::size_t face_index = 0;
  while (down_index < down_the_line.size() && face_index < face_on.size()) {
    const std::int64_t residual =
        face_on[face_index].time_us - down_the_line[down_index].time_us - trial.offset_us;
    if (std::abs(residual) <= trial.tolerance_us) {
      matches.push_back({.down_the_line_index = down_index,
                         .face_on_index = face_index,
                         .residual_us = residual});
      ++down_index;
      ++face_index;
    } else if (residual < 0) {
      ++face_index;
    } else {
      ++down_index;
    }
  }
  return matches;
}

Alignment ScoreAlignment(const std::vector<Candidate> &down_the_line,
                         const std::vector<Candidate> &face_on, std::int64_t offset_us,
                         std::int64_t tolerance_us) {
  Alignment value;
  value.offset_us = offset_us;
  value.matches =
      MatchAtOffset(down_the_line, face_on, {.offset_us = offset_us, .tolerance_us = tolerance_us});
  for (const Match &match : value.matches) {
    value.total_absolute_residual_us += std::abs(match.residual_us);
    value.total_peak_ppm +=
        down_the_line[match.down_the_line_index].peak_ppm + face_on[match.face_on_index].peak_ppm;
  }
  if (!value.matches.empty()) {
    std::vector<std::int64_t> differences;
    differences.reserve(value.matches.size());
    for (const Match &match : value.matches) {
      differences.push_back(face_on[match.face_on_index].time_us -
                            down_the_line[match.down_the_line_index].time_us);
    }
    std::ranges::sort(differences);
    value.offset_us = differences[differences.size() / 2];
    value.matches = MatchAtOffset(down_the_line, face_on,
                                  {.offset_us = value.offset_us, .tolerance_us = tolerance_us});
    value.total_absolute_residual_us = 0;
    value.total_peak_ppm = 0;
    for (const Match &match : value.matches) {
      value.total_absolute_residual_us += std::abs(match.residual_us);
      value.total_peak_ppm +=
          down_the_line[match.down_the_line_index].peak_ppm + face_on[match.face_on_index].peak_ppm;
    }
  }
  return value;
}

bool BetterAlignment(const Alignment &candidate, const Alignment &best, std::int64_t prior_us) {
  return std::tuple(candidate.matches.size(), candidate.total_peak_ppm,
                    -candidate.total_absolute_residual_us,
                    -std::abs(candidate.offset_us - prior_us), -std::abs(candidate.offset_us)) >
         std::tuple(best.matches.size(), best.total_peak_ppm, -best.total_absolute_residual_us,
                    -std::abs(best.offset_us - prior_us), -std::abs(best.offset_us));
}

Alignment Align(const std::vector<Candidate> &down_the_line, const std::vector<Candidate> &face_on,
                std::int64_t prior_us, std::int64_t tolerance_us) {
  if (down_the_line.empty() || face_on.empty()) {
    return {
        .offset_us = prior_us, .matches = {}, .total_absolute_residual_us = 0, .total_peak_ppm = 0};
  }
  std::map<std::int64_t, std::size_t> histogram;
  for (const Candidate &down : down_the_line) {
    for (const Candidate &face : face_on) {
      const std::int64_t difference = face.time_us - down.time_us;
      const std::int64_t bin = difference >= 0
                                   ? (difference + kHistogramBinUs / 2) / kHistogramBinUs
                                   : (difference - kHistogramBinUs / 2) / kHistogramBinUs;
      ++histogram[bin];
    }
  }
  std::vector<std::pair<std::int64_t, std::size_t>> hypotheses(histogram.begin(), histogram.end());
  std::ranges::sort(hypotheses, [prior_us](const auto &left, const auto &right) {
    return std::tuple(left.second, -std::abs(left.first * kHistogramBinUs - prior_us),
                      -left.first) > std::tuple(right.second,
                                                -std::abs(right.first * kHistogramBinUs - prior_us),
                                                -right.first);
  });
  Alignment best = ScoreAlignment(down_the_line, face_on, prior_us, tolerance_us);
  const std::size_t count = std::min(hypotheses.size(), kMaximumAlignmentHypotheses);
  for (std::size_t index = 0; index < count; ++index) {
    Alignment candidate = ScoreAlignment(down_the_line, face_on,
                                         hypotheses[index].first * kHistogramBinUs, tolerance_us);
    if (BetterAlignment(candidate, best, prior_us)) {
      best = std::move(candidate);
    }
  }
  return best;
}

nlohmann::json CandidateJson(const Candidate &candidate, std::size_t index,
                             std::int64_t duration_us, const FieldRecordingAnalysisConfig &config,
                             const std::vector<int> &matched_shots) {
  return {{"candidate_index", index},
          {"frame_position", std::to_string(candidate.frame_position)},
          {"matched_pair_index", matched_shots[index] < 0 ? nlohmann::json(nullptr)
                                                          : nlohmann::json(matched_shots[index])},
          {"matched_pair_id",
           matched_shots[index] < 0
               ? nlohmann::json(nullptr)
               : nlohmann::json(PairId(static_cast<std::size_t>(matched_shots[index])))},
          {"noise_floor_ppm", candidate.noise_floor_ppm},
          {"peak_amplitude_ppm", candidate.peak_ppm},
          {"threshold_ppm", candidate.threshold_ppm},
          {"time_us", std::to_string(candidate.time_us)},
          {"window_end_us",
           std::to_string(std::min(duration_us, candidate.time_us + config.post_roll_us))},
          {"window_start_us",
           std::to_string(std::max<std::int64_t>(0, candidate.time_us - config.pre_roll_us))}};
}

nlohmann::json DetectorJson(const ImpactDetectorConfig &config) {
  return {{"initial_noise_floor_ppm", Ppm(config.initial_noise_floor)},
          {"minimum_peak_amplitude_ppm", Ppm(config.minimum_peak_amplitude)},
          {"noise_floor_time_constant_us",
           std::llround(config.noise_floor_time_constant_seconds * 1'000'000.0)},
          {"noise_update_clip_multiplier_milli",
           std::llround(static_cast<double>(config.noise_update_clip_multiplier) * 1'000.0)},
          {"peak_confirmation_us", std::llround(config.peak_confirmation_seconds * 1'000'000.0)},
          {"refractory_us", std::llround(config.refractory_period_seconds * 1'000'000.0)},
          {"threshold_multiplier_milli",
           std::llround(static_cast<double>(config.threshold_multiplier) * 1'000.0)}};
}

}  // namespace

nlohmann::json AnalyzeFieldRecordingPair(const FieldRecordingPair &recordings,
                                         FieldRecordingAnalysisConfig config) {
  if (config.pre_roll_us < 0 || config.post_roll_us < 0 || config.pair_tolerance_us <= 0) {
    throw std::invalid_argument("analysis time bounds are invalid");
  }
  ValidateRecording(recordings.down_the_line_manifest, recordings.down_the_line_audio,
                    "down_the_line");
  ValidateRecording(recordings.face_on_manifest, recordings.face_on_audio, "face_on");
  const std::string shared_id =
      recordings.down_the_line_manifest.at("shared_recording_id").get<std::string>();
  if (shared_id.empty() || recordings.face_on_manifest.at("shared_recording_id") != shared_id) {
    throw std::invalid_argument("field recordings do not share a recording ID");
  }

  const std::vector<Candidate> down_candidates =
      DetectCandidates(recordings.down_the_line_audio, config.down_the_line_detector);
  const std::vector<Candidate> face_candidates =
      DetectCandidates(recordings.face_on_audio, config.face_on_detector);
  const std::int64_t prior_us =
      PartsToUtcUs(recordings.down_the_line_manifest.at("created_at_utc").get<std::string>()) -
      PartsToUtcUs(recordings.face_on_manifest.at("created_at_utc").get<std::string>());
  const Alignment alignment =
      Align(down_candidates, face_candidates, prior_us, config.pair_tolerance_us);

  std::vector<int> down_matched(down_candidates.size(), -1);
  std::vector<int> face_matched(face_candidates.size(), -1);
  nlohmann::json paired_candidates = nlohmann::json::array();
  const std::int64_t down_duration_us =
      static_cast<std::int64_t>(recordings.down_the_line_audio.samples.size()) * 1'000'000LL /
      recordings.down_the_line_audio.sample_rate_hz;
  const std::int64_t face_duration_us =
      static_cast<std::int64_t>(recordings.face_on_audio.samples.size()) * 1'000'000LL /
      recordings.face_on_audio.sample_rate_hz;
  for (std::size_t index = 0; index < alignment.matches.size(); ++index) {
    const Match &match = alignment.matches[index];
    down_matched[match.down_the_line_index] = static_cast<int>(index);
    face_matched[match.face_on_index] = static_cast<int>(index);
    const Candidate &down = down_candidates[match.down_the_line_index];
    const Candidate &face = face_candidates[match.face_on_index];
    paired_candidates.push_back(
        {{"down_the_line_candidate_index", match.down_the_line_index},
         {"down_the_line_impact_time_us", std::to_string(down.time_us)},
         {"down_the_line_window_end_us",
          std::to_string(std::min(down_duration_us, down.time_us + config.post_roll_us))},
         {"down_the_line_window_start_us",
          std::to_string(std::max<std::int64_t>(0, down.time_us - config.pre_roll_us))},
         {"face_on_candidate_index", match.face_on_index},
         {"face_on_impact_time_us", std::to_string(face.time_us)},
         {"face_on_window_end_us",
          std::to_string(std::min(face_duration_us, face.time_us + config.post_roll_us))},
         {"face_on_window_start_us",
          std::to_string(std::max<std::int64_t>(0, face.time_us - config.pre_roll_us))},
         {"pair_residual_us", std::to_string(match.residual_us)},
         {"pair_index", index},
         {"paired_candidate_id", PairId(index)}});
  }

  const auto unmatched_json = [](const std::vector<Candidate> &candidates,
                                 const std::vector<int> &matched) {
    nlohmann::json unmatched = nlohmann::json::array();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (matched[index] < 0) {
        unmatched.push_back({{"candidate_index", index},
                             {"peak_amplitude_ppm", candidates[index].peak_ppm},
                             {"time_us", std::to_string(candidates[index].time_us)}});
      }
    }
    return unmatched;
  };

  const auto stream_json = [&config](const nlohmann::json &manifest,
                                     const DecodedMonoPcmS16Wav &audio,
                                     const std::vector<Candidate> &candidates,
                                     const std::vector<int> &matched) {
    const std::int64_t duration_us =
        static_cast<std::int64_t>(audio.samples.size()) * 1'000'000LL / audio.sample_rate_hz;
    nlohmann::json candidate_json = nlohmann::json::array();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      candidate_json.push_back(
          CandidateJson(candidates[index], index, duration_us, config, matched));
    }
    return nlohmann::json{
        {"candidate_count", candidates.size()},        {"candidates", std::move(candidate_json)},
        {"duration_us", std::to_string(duration_us)},  {"node_id", manifest.at("node_id")},
        {"recording_id", manifest.at("recording_id")}, {"role", manifest.at("role")},
        {"sample_rate_hz", audio.sample_rate_hz}};
  };

  return {{"alignment",
           {{"created_at_prior_face_on_minus_down_the_line_us", std::to_string(prior_us)},
            {"face_on_time_minus_down_the_line_us", std::to_string(alignment.offset_us)},
            {"matched_candidate_count", alignment.matches.size()},
            {"method", alignment.matches.empty() ? "recording_start_utc_fallback"
                                                 : "audio_impulse_consensus"},
            {"pair_tolerance_us", std::to_string(config.pair_tolerance_us)},
            {"total_absolute_residual_us", std::to_string(alignment.total_absolute_residual_us)}}},
          {"detectors",
           {{"down_the_line", DetectorJson(config.down_the_line_detector)},
            {"face_on", DetectorJson(config.face_on_detector)}}},
          {"schema_version", 1},
          {"shared_recording_id", shared_id},
          {"paired_candidate_count", paired_candidates.size()},
          {"paired_candidates", std::move(paired_candidates)},
          {"streams",
           {{"down_the_line",
             stream_json(recordings.down_the_line_manifest, recordings.down_the_line_audio,
                         down_candidates, down_matched)},
            {"face_on", stream_json(recordings.face_on_manifest, recordings.face_on_audio,
                                    face_candidates, face_matched)}}},
          {"unmatched_candidates",
           {{"down_the_line", unmatched_json(down_candidates, down_matched)},
            {"face_on", unmatched_json(face_candidates, face_matched)}}},
          {"window",
           {{"post_roll_us", std::to_string(config.post_roll_us)},
            {"pre_roll_us", std::to_string(config.pre_roll_us)}}}};
}

std::string FieldRecordingPairedCandidatesCsv(const nlohmann::json &analysis) {
  std::string csv =
      "paired_candidate_id,down_the_line_impact_time_us,face_on_impact_time_us,pair_residual_us,"
      "down_the_line_window_start_us,down_the_line_window_end_us,face_on_window_start_us,"
      "face_on_window_end_us\n";
  for (const nlohmann::json &shot : analysis.at("paired_candidates")) {
    csv += shot.at("paired_candidate_id").get<std::string>() + ",";
    csv += shot.at("down_the_line_impact_time_us").get<std::string>() + ",";
    csv += shot.at("face_on_impact_time_us").get<std::string>() + ",";
    csv += shot.at("pair_residual_us").get<std::string>() + ",";
    csv += shot.at("down_the_line_window_start_us").get<std::string>() + ",";
    csv += shot.at("down_the_line_window_end_us").get<std::string>() + ",";
    csv += shot.at("face_on_window_start_us").get<std::string>() + ",";
    csv += shot.at("face_on_window_end_us").get<std::string>() + "\n";
  }
  return csv;
}

// NOLINTEND(misc-include-cleaner)

}  // namespace swing_capture::offline
