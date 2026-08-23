#include "capture/offline/experiments/envelope/envelope_experiment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <nlohmann/json.hpp>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/benchmark/terminal_window_benchmark.h"

namespace swing_capture::offline::envelope {
namespace {

// NOLINTBEGIN(misc-include-cleaner)

constexpr double kPcmScale = 32768.0;

std::size_t FramesFor(std::int64_t duration_us, std::uint32_t sample_rate_hz) {
  if (duration_us <= 0) {
    throw std::invalid_argument("detector durations must be positive");
  }
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(duration_us * static_cast<std::int64_t>(sample_rate_hz) /
                                  1'000'000));
}

std::int64_t TimeUs(std::size_t frame, std::uint32_t sample_rate_hz) {
  return static_cast<std::int64_t>(frame) * 1'000'000 / static_cast<std::int64_t>(sample_rate_hz);
}

double RangeRms(std::span<const double> sum_squares, std::size_t begin, std::size_t end) {
  if (begin >= end || end >= sum_squares.size()) {
    return 0.0;
  }
  return std::sqrt(std::max(0.0, sum_squares[end] - sum_squares[begin]) /
                   static_cast<double>(end - begin));
}

double RobustBackgroundRms(std::span<const double> sum_squares, std::size_t end,
                           std::size_t window_frames, std::size_t block_frames) {
  if (end < window_frames || block_frames == 0) {
    return 0.0;
  }
  std::vector<double> blocks;
  const std::size_t begin = end - window_frames;
  for (std::size_t block_begin = begin; block_begin + block_frames <= end;
       block_begin += block_frames) {
    blocks.push_back(RangeRms(sum_squares, block_begin, block_begin + block_frames));
  }
  if (blocks.empty()) {
    return 0.0;
  }
  const auto middle = blocks.begin() + static_cast<std::ptrdiff_t>(blocks.size() / 2);
  std::nth_element(blocks.begin(), middle, blocks.end());
  return *middle;
}

struct Hit {
  std::size_t frame;
  std::size_t peak_frame;
  double score;
  Candidate feature;
};

struct FeatureSeries {
  std::span<const double> signal;
  std::span<const double> squares;
  std::span<const double> difference_squares;
};

struct FeaturePosition {
  std::size_t frame;
  std::size_t fast_frames;
  double background_rms;
};

Candidate MakeCandidate(const FeatureSeries &series, const FeaturePosition &position,
                        std::uint32_t sample_rate_hz) {
  const auto signal = series.signal;
  const auto squares = series.squares;
  const auto difference_squares = series.difference_squares;
  const std::size_t frame = position.frame;
  const std::size_t fast_frames = position.fast_frames;
  const double background_rms = position.background_rms;
  const std::size_t end = std::min(signal.size(), frame + fast_frames);
  const std::size_t prior_begin = frame > fast_frames * 6 ? frame - fast_frames * 6 : 0;
  const double fast_rms = RangeRms(squares, frame, end);
  const double prior_rms = RangeRms(squares, prior_begin, frame);
  const double difference_rms = RangeRms(difference_squares, frame, end);
  std::size_t peak_frame = frame;
  double peak = 0.0;
  for (std::size_t sample = frame; sample < end; ++sample) {
    if (std::abs(signal[sample]) > peak) {
      peak = std::abs(signal[sample]);
      peak_frame = sample;
    }
  }
  return {
      .strike_us = TimeUs(peak_frame, sample_rate_hz),
      .decision_us = TimeUs(end, sample_rate_hz),
      .peak = peak,
      .fast_rms = fast_rms,
      .background_rms = background_rms,
      .background_ratio = fast_rms / std::max(background_rms, 1e-7),
      .rise_ratio = fast_rms / std::max(prior_rms, 1e-7),
      .high_frequency_ratio = difference_rms / std::max(2.0 * fast_rms, 1e-7),
      .crest_factor = peak / std::max(fast_rms, 1e-7),
      .cluster_duration_us = 0,
  };
}

void Validate(const DecodedMonoPcmS16Wav &audio, const DetectorConfig &config) {
  if (audio.sample_rate_hz == 0 || audio.samples.empty()) {
    throw std::invalid_argument("audio cannot be empty");
  }
  if (config.name.empty() || config.high_pass_hz <= 0.0 || config.hop_us <= 0 ||
      config.fast_window_us <= 0 || config.background_window_us <= 0 ||
      config.background_guard_us <= 0 || config.background_block_us <= 0 ||
      config.background_multiplier <= 0.0 || config.rise_ratio <= 0.0 ||
      config.minimum_peak < 0.0 || config.minimum_high_frequency_ratio < 0.0 ||
      config.minimum_crest_factor < 0.0 || config.cluster_gap_us < 0 || config.refractory_us < 0) {
    throw std::invalid_argument("detector configuration is invalid");
  }
  if (config.high_pass_hz >= static_cast<double>(audio.sample_rate_hz) / 2.0 ||
      config.background_block_us > config.background_window_us) {
    throw std::invalid_argument("detector frequency or background block is invalid");
  }
}

nlohmann::json ConfigJson(const DetectorConfig &config) {
  return {{"name", config.name},
          {"high_pass_hz", config.high_pass_hz},
          {"hop_us", config.hop_us},
          {"fast_window_us", config.fast_window_us},
          {"background_window_us", config.background_window_us},
          {"background_guard_us", config.background_guard_us},
          {"background_block_us", config.background_block_us},
          {"background_multiplier", config.background_multiplier},
          {"rise_ratio", config.rise_ratio},
          {"minimum_peak", config.minimum_peak},
          {"minimum_high_frequency_ratio", config.minimum_high_frequency_ratio},
          {"minimum_crest_factor", config.minimum_crest_factor},
          {"cluster_gap_us", config.cluster_gap_us},
          {"refractory_us", config.refractory_us}};
}

nlohmann::json CandidateJson(const Candidate &candidate, std::int64_t target_us) {
  return {{"strike_us", std::to_string(candidate.strike_us)},
          {"decision_us", std::to_string(candidate.decision_us)},
          {"error_from_target_us", std::to_string(candidate.strike_us - target_us)},
          {"decision_latency_us", std::to_string(candidate.decision_us - candidate.strike_us)},
          {"peak", candidate.peak},
          {"fast_rms", candidate.fast_rms},
          {"background_rms", candidate.background_rms},
          {"background_ratio", candidate.background_ratio},
          {"rise_ratio", candidate.rise_ratio},
          {"high_frequency_ratio", candidate.high_frequency_ratio},
          {"crest_factor", candidate.crest_factor},
          {"cluster_duration_us", std::to_string(candidate.cluster_duration_us)}};
}

std::pair<std::size_t, std::vector<bool>> CandidateRecall(const std::vector<Candidate> &candidates,
                                                          const std::vector<ArmWindow> &windows,
                                                          std::int64_t target_tolerance_us) {
  std::size_t recalled = 0;
  std::vector<bool> credited(candidates.size(), false);
  for (const ArmWindow &window : windows) {
    bool target_recalled = false;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (std::abs(candidates[index].strike_us - window.target_us) <= target_tolerance_us) {
        target_recalled = true;
        credited[index] = true;
      }
    }
    recalled += target_recalled ? 1 : 0;
  }
  return {recalled, std::move(credited)};
}

nlohmann::json CandidateList(const std::vector<Candidate> &candidates,
                             const std::vector<ArmWindow> &windows,
                             const std::vector<bool> &credited) {
  nlohmann::json result = nlohmann::json::array();
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const Candidate &candidate = candidates[index];
    const auto nearest =
        std::ranges::min_element(windows, [&](const ArmWindow &left, const ArmWindow &right) {
          return std::abs(candidate.strike_us - left.target_us) <
                 std::abs(candidate.strike_us - right.target_us);
        });
    nlohmann::json item = CandidateJson(candidate, nearest->target_us);
    item["credited"] = credited[index];
    item["nearest_target_id"] = nearest->id;
    result.push_back(std::move(item));
  }
  return result;
}

std::int64_t Median(std::vector<std::int64_t> values) {
  if (values.empty()) {
    return 0;
  }
  std::ranges::sort(values);
  return values[values.size() / 2];
}

std::string OutcomeName(experiments::TerminalOutcome outcome) {
  switch (outcome) {
    case experiments::TerminalOutcome::kTargetFirst:
      return "target_first";
    case experiments::TerminalOutcome::kFalseEarly:
      return "false_early_terminal";
    case experiments::TerminalOutcome::kLate:
      return "late_candidate";
    case experiments::TerminalOutcome::kNoCandidate:
      return "no_candidate";
  }
  throw std::logic_error("unknown terminal outcome");
}

struct AnalysisTiming {
  std::int64_t readiness_delay_us;
  std::int64_t target_tolerance_us;
};

nlohmann::json ScoreTerminalWindows(const std::vector<Candidate> &candidates,
                                    const std::vector<ArmWindow> &windows,
                                    const AnalysisTiming &timing) {
  std::vector<experiments::AcceptedAudioEvent> accepted_events;
  accepted_events.reserve(candidates.size());
  std::ranges::transform(candidates, std::back_inserter(accepted_events),
                         [](const Candidate &candidate) {
                           return experiments::AcceptedAudioEvent{.time_us = candidate.strike_us};
                         });
  std::vector<experiments::TerminalWindow> benchmark_windows;
  benchmark_windows.reserve(windows.size());
  std::ranges::transform(windows, std::back_inserter(benchmark_windows),
                         [&](const ArmWindow &window) {
                           return experiments::TerminalWindow{
                               .id = window.id,
                               .arm_us = window.arm_us,
                               .ready_us = window.arm_us + timing.readiness_delay_us,
                               .target_us = window.target_us,
                               .end_us = window.end_us,
                           };
                         });
  const experiments::TerminalBenchmarkResult benchmark = experiments::BenchmarkTerminalWindows(
      accepted_events, benchmark_windows, timing.target_tolerance_us);

  nlohmann::json terminal_windows = nlohmann::json::array();
  std::vector<std::int64_t> target_absolute_errors;
  std::vector<std::int64_t> decision_latencies;
  for (std::size_t index = 0; index < windows.size(); ++index) {
    const ArmWindow &window = windows[index];
    const experiments::TerminalWindowResult &window_result = benchmark.windows[index];
    const std::string outcome = OutcomeName(window_result.outcome);
    const auto terminal =
        window_result.terminal_event_us.has_value()
            ? std::ranges::lower_bound(candidates, *window_result.terminal_event_us, {},
                                       &Candidate::strike_us)
            : candidates.end();
    const Candidate *terminal_pointer = terminal == candidates.end() ? nullptr : &*terminal;
    if (terminal_pointer != nullptr) {
      decision_latencies.push_back(terminal_pointer->decision_us - terminal_pointer->strike_us);
      if (outcome == "target_first") {
        target_absolute_errors.push_back(std::abs(terminal_pointer->strike_us - window.target_us));
      }
    }
    terminal_windows.push_back(
        {{"id", window.id},
         {"arm_us", std::to_string(window.arm_us)},
         {"ready_us", std::to_string(benchmark_windows[index].ready_us)},
         {"target_us", std::to_string(window.target_us)},
         {"end_us", std::to_string(window.end_us)},
         {"outcome", outcome},
         {"terminal", terminal_pointer == nullptr
                          ? nlohmann::json(nullptr)
                          : CandidateJson(*terminal_pointer, window.target_us)}});
  }
  return {{"target_first_count", benchmark.target_first_count},
          {"false_early_terminal_count", benchmark.false_early_count},
          {"late_candidate_count", benchmark.late_count},
          {"no_candidate_count", benchmark.no_candidate_count},
          {"target_first_median_absolute_error_us", Median(target_absolute_errors)},
          {"target_first_max_absolute_error_us",
           target_absolute_errors.empty() ? 0 : *std::ranges::max_element(target_absolute_errors)},
          {"median_decision_latency_us", Median(decision_latencies)},
          {"max_decision_latency_us",
           decision_latencies.empty() ? 0 : *std::ranges::max_element(decision_latencies)},
          {"windows", std::move(terminal_windows)}};
}

nlohmann::json AnalyzeConfig(const DecodedMonoPcmS16Wav &audio,
                             const std::vector<ArmWindow> &windows, const DetectorConfig &config,
                             std::int64_t readiness_delay_us, std::int64_t target_tolerance_us,
                             std::int64_t duration_us) {
  const std::vector<Candidate> candidates = DetectCandidates(audio, config);
  auto [recalled, credited] = CandidateRecall(candidates, windows, target_tolerance_us);
  const auto non_credited = static_cast<std::size_t>(std::ranges::count(credited, false));
  return {{"config", ConfigJson(config)},
          {"candidate_generation",
           {{"target_recalled", recalled},
            {"target_count", windows.size()},
            {"candidate_count", candidates.size()},
            {"non_credited_candidate_count", non_credited},
            {"false_candidates_per_minute",
             static_cast<double>(non_credited) * 60'000'000.0 / static_cast<double>(duration_us)},
            {"candidates", CandidateList(candidates, windows, credited)}}},
          {"armed_terminal",
           ScoreTerminalWindows(candidates, windows,
                                AnalysisTiming{.readiness_delay_us = readiness_delay_us,
                                               .target_tolerance_us = target_tolerance_us})}};
}

}  // namespace

std::vector<Candidate> DetectCandidates(const DecodedMonoPcmS16Wav &audio,
                                        const DetectorConfig &config) {
  Validate(audio, config);
  const double time_constant = 1.0 / (2.0 * std::numbers::pi * config.high_pass_hz);
  const double sample_period = 1.0 / static_cast<double>(audio.sample_rate_hz);
  const double alpha = time_constant / (time_constant + sample_period);
  std::vector<double> signal(audio.samples.size());
  double previous_input = static_cast<double>(audio.samples.front()) / kPcmScale;
  double previous_output = 0.0;
  for (std::size_t frame = 1; frame < audio.samples.size(); ++frame) {
    const double input = static_cast<double>(audio.samples[frame]) / kPcmScale;
    previous_output = alpha * (previous_output + input - previous_input);
    signal[frame] = previous_output;
    previous_input = input;
  }

  std::vector<double> squares(signal.size() + 1, 0.0);
  std::vector<double> difference_squares(signal.size() + 1, 0.0);
  for (std::size_t frame = 0; frame < signal.size(); ++frame) {
    squares[frame + 1] = squares[frame] + signal[frame] * signal[frame];
    const double difference = frame == 0 ? 0.0 : signal[frame] - signal[frame - 1];
    difference_squares[frame + 1] = difference_squares[frame] + difference * difference;
  }

  const std::size_t hop_frames = FramesFor(config.hop_us, audio.sample_rate_hz);
  const std::size_t fast_frames = FramesFor(config.fast_window_us, audio.sample_rate_hz);
  const std::size_t background_frames =
      FramesFor(config.background_window_us, audio.sample_rate_hz);
  const std::size_t guard_frames = FramesFor(config.background_guard_us, audio.sample_rate_hz);
  const std::size_t block_frames = FramesFor(config.background_block_us, audio.sample_rate_hz);
  const std::size_t cluster_gap_frames =
      FramesFor(std::max<std::int64_t>(1, config.cluster_gap_us), audio.sample_rate_hz);
  const std::size_t refractory_frames =
      FramesFor(std::max<std::int64_t>(1, config.refractory_us), audio.sample_rate_hz);

  std::vector<Candidate> candidates;
  std::vector<Hit> cluster;
  std::size_t last_accepted_frame = 0;
  double cached_background = 0.0;
  std::size_t cached_background_frame = 0;
  const std::size_t background_refresh_frames = FramesFor(5'000, audio.sample_rate_hz);

  const auto finish_cluster = [&]() {
    if (cluster.empty()) {
      return;
    }
    const auto best = std::ranges::max_element(
        cluster, [](const Hit &left, const Hit &right) { return left.score < right.score; });
    Candidate candidate = best->feature;
    const std::size_t cluster_end = cluster.back().frame + fast_frames;
    candidate.decision_us = TimeUs(cluster_end + cluster_gap_frames, audio.sample_rate_hz);
    candidate.cluster_duration_us =
        TimeUs(cluster_end - cluster.front().frame, audio.sample_rate_hz);
    if (candidates.empty() || best->peak_frame >= last_accepted_frame + refractory_frames) {
      candidates.push_back(candidate);
      last_accepted_frame = best->peak_frame;
    }
    cluster.clear();
  };

  const std::size_t first_frame = background_frames + guard_frames;
  for (std::size_t frame = first_frame; frame + fast_frames < signal.size(); frame += hop_frames) {
    if (cached_background_frame == 0 ||
        frame >= cached_background_frame + background_refresh_frames) {
      cached_background =
          RobustBackgroundRms(squares, frame - guard_frames, background_frames, block_frames);
      cached_background_frame = frame;
    }
    const Candidate feature = MakeCandidate(
        FeatureSeries{
            .signal = signal, .squares = squares, .difference_squares = difference_squares},
        FeaturePosition{
            .frame = frame, .fast_frames = fast_frames, .background_rms = cached_background},
        audio.sample_rate_hz);
    const bool qualifies = feature.background_ratio >= config.background_multiplier &&
                           feature.rise_ratio >= config.rise_ratio &&
                           feature.peak >= config.minimum_peak &&
                           feature.high_frequency_ratio >= config.minimum_high_frequency_ratio &&
                           feature.crest_factor >= config.minimum_crest_factor;
    if (!qualifies) {
      if (!cluster.empty() && frame > cluster.back().frame + cluster_gap_frames) {
        finish_cluster();
      }
      continue;
    }
    if (!cluster.empty() && frame > cluster.back().frame + cluster_gap_frames) {
      finish_cluster();
    }
    const double score = feature.background_ratio * feature.rise_ratio *
                         (0.5 + feature.high_frequency_ratio) * feature.crest_factor;
    const auto peak_frame = static_cast<std::size_t>(
        feature.strike_us * static_cast<std::int64_t>(audio.sample_rate_hz) / 1'000'000);
    cluster.push_back(
        {.frame = frame, .peak_frame = peak_frame, .score = score, .feature = feature});
  }
  finish_cluster();
  return candidates;
}

nlohmann::json Analyze(const DecodedMonoPcmS16Wav &audio, const std::vector<ArmWindow> &windows,
                       const std::vector<DetectorConfig> &configs, std::int64_t readiness_delay_us,
                       std::int64_t target_tolerance_us) {
  if (readiness_delay_us < 0 || target_tolerance_us < 0 || windows.empty() || configs.empty()) {
    throw std::invalid_argument("analysis inputs are invalid");
  }
  const std::int64_t duration_us =
      static_cast<std::int64_t>(audio.samples.size()) * 1'000'000 / audio.sample_rate_hz;
  nlohmann::json reports = nlohmann::json::array();
  for (const DetectorConfig &config : configs) {
    reports.push_back(AnalyzeConfig(audio, windows, config, readiness_delay_us, target_tolerance_us,
                                    duration_us));
  }
  return {{"schema_version", 1},
          {"recording_duration_us", std::to_string(duration_us)},
          {"readiness_delay_us", std::to_string(readiness_delay_us)},
          {"target_tolerance_us", std::to_string(target_tolerance_us)},
          {"method",
           "one-pole high-pass; median RMS of 10 ms blocks in the preceding 240 ms; 1.5 ms "
           "fast RMS; rise, crest, and first-difference high-frequency features; bounded online "
           "clustering"},
          {"reports", std::move(reports)}};
}

std::vector<ArmWindow> ParseArmWindows(const nlohmann::json &window_report) {
  std::vector<ArmWindow> windows;
  for (const auto &window : window_report.at("windows")) {
    windows.push_back({.id = window.at("id").get<std::string>(),
                       .arm_us = std::stoll(window.at("start_us").get<std::string>()),
                       .target_us = std::stoll(window.at("target_us").get<std::string>()),
                       .end_us = std::stoll(window.at("end_us").get<std::string>())});
  }
  return windows;
}

std::vector<DetectorConfig> ExperimentConfigs() {
  std::vector<DetectorConfig> configs;
  for (const double multiplier : std::array{6.0, 8.0, 10.0, 12.0, 14.0, 16.0, 18.0, 20.0}) {
    DetectorConfig config;
    config.name = "robust_hp120_x" + std::to_string(static_cast<int>(multiplier));
    config.background_multiplier = multiplier;
    configs.push_back(config);
  }
  for (const double multiplier : std::array{6.0, 8.0, 10.0}) {
    DetectorConfig config;
    config.name = "robust_hp200_hf_x" + std::to_string(static_cast<int>(multiplier));
    config.high_pass_hz = 200.0;
    config.background_multiplier = multiplier;
    config.minimum_high_frequency_ratio = 0.18;
    config.minimum_crest_factor = 2.5;
    configs.push_back(config);
  }
  DetectorConfig longer_cluster;
  longer_cluster.name = "robust_hp120_x8_cluster60";
  longer_cluster.cluster_gap_us = 60'000;
  longer_cluster.refractory_us = 75'000;
  configs.push_back(longer_cluster);
  for (const double rise_ratio : std::array{1.2, 1.6, 2.0}) {
    DetectorConfig config;
    config.name =
        "robust_hp120_x12_rise" + std::to_string(static_cast<int>(std::lround(rise_ratio * 10.0)));
    config.background_multiplier = 12.0;
    config.rise_ratio = rise_ratio;
    configs.push_back(config);
  }
  for (const double high_pass_hz : std::array{80.0, 200.0, 300.0}) {
    DetectorConfig config;
    config.name = "robust_hp" + std::to_string(static_cast<int>(high_pass_hz)) + "_x12";
    config.high_pass_hz = high_pass_hz;
    config.background_multiplier = 12.0;
    configs.push_back(config);
  }
  for (const double high_frequency_ratio : std::array{0.30, 0.35, 0.40, 0.45, 0.50}) {
    DetectorConfig config;
    config.name = "robust_hp120_x12_hf" +
                  std::to_string(static_cast<int>(std::lround(high_frequency_ratio * 100.0)));
    config.background_multiplier = 12.0;
    config.minimum_high_frequency_ratio = high_frequency_ratio;
    configs.push_back(config);
  }
  for (const double multiplier : std::array{14.0, 16.0, 18.0, 20.0, 22.0, 24.0, 28.0, 32.0}) {
    DetectorConfig config;
    config.name = "robust_hp120_x" + std::to_string(static_cast<int>(multiplier)) + "_hf40";
    config.background_multiplier = multiplier;
    config.minimum_high_frequency_ratio = 0.40;
    configs.push_back(config);
  }
  for (const double high_frequency_ratio : std::array{0.35, 0.45}) {
    DetectorConfig config;
    config.name = "robust_hp120_x20_hf" +
                  std::to_string(static_cast<int>(std::lround(high_frequency_ratio * 100.0)));
    config.background_multiplier = 20.0;
    config.minimum_high_frequency_ratio = high_frequency_ratio;
    configs.push_back(config);
  }
  return configs;
}

// NOLINTEND(misc-include-cleaner)

}  // namespace swing_capture::offline::envelope
