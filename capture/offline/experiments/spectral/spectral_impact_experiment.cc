#include "capture/offline/experiments/spectral/spectral_impact_experiment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "capture/audio/pcm_wav.h"
#include "capture/offline/experiments/benchmark/terminal_window_benchmark.h"

namespace swing_capture::offline::spectral {
namespace {

// NOLINTBEGIN(misc-include-cleaner)

constexpr std::size_t kSpectrumFrameSamples = 512;
constexpr std::size_t kSpectrumBands = 32;
constexpr std::int64_t kTargetToleranceUs = 100'000;
constexpr double kMinimumPower = 1.0e-18;

constexpr std::array<std::string_view, 18> kFeatureNames = {
    "log_peak",           "log_short_rms",     "log_long_rms",     "crest_factor",
    "attack_ratio",       "decay_ratio",       "impulse_fraction", "half_peak_ms",
    "zero_crossing_rate", "spectral_centroid", "spectral_rolloff", "spectral_flatness",
    "spectral_flux",      "band_100_1000",     "band_1000_3000",   "band_3000_6000",
    "band_6000_12000",    "band_12000_20000",
};

constexpr std::array<std::size_t, 8> kTemporalFeatureIndices = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr std::array<std::size_t, 10> kSpectroTemporalFeatureIndices = {0, 3, 4,  5,  6,
                                                                        8, 9, 10, 11, 12};
constexpr std::array<std::size_t, 18> kAllFeatureIndices = {0, 1,  2,  3,  4,  5,  6,  7,  8,
                                                            9, 10, 11, 12, 13, 14, 15, 16, 17};

struct Example {
  std::string shot_id;
  std::string view;
  std::string device;
  std::int64_t candidate_us = 0;
  std::int64_t arm_us = 0;
  std::int64_t ready_us = 0;
  std::int64_t target_us = 0;
  std::int64_t end_us = 0;
  bool target = false;
  ImpactFeatures features;
};

enum class MethodKind {
  kPeakAmplitude,
  kPositiveSpectralTemplate,
  kContrastiveSpectralTemplate,
  kTemporalLogistic,
  kSpectroTemporalLogistic,
  kAllFeatureLogistic,
  kDiagonalGaussian,
};

struct MethodSpec {
  std::string_view name;
  MethodKind kind;
  std::int64_t added_decision_latency_us;
  std::string_view complexity;
};

constexpr std::array<MethodSpec, 7> kMethods = {{
    {.name = "peak_amplitude",
     .kind = MethodKind::kPeakAmplitude,
     .added_decision_latency_us = 0,
     .complexity = "O(1) after production candidate generation"},
    {.name = "positive_spectral_template",
     .kind = MethodKind::kPositiveSpectralTemplate,
     .added_decision_latency_us = 9'000,
     .complexity = "one 512-sample real spectrum plus 32 multiply-adds"},
    {.name = "contrastive_spectral_template",
     .kind = MethodKind::kContrastiveSpectralTemplate,
     .added_decision_latency_us = 9'000,
     .complexity = "one 512-sample real spectrum plus 64 multiply-adds"},
    {.name = "temporal_logistic",
     .kind = MethodKind::kTemporalLogistic,
     .added_decision_latency_us = 30'000,
     .complexity = "8 standardized features and 8 multiply-adds per candidate"},
    {.name = "spectro_temporal_logistic",
     .kind = MethodKind::kSpectroTemporalLogistic,
     .added_decision_latency_us = 30'000,
     .complexity = "one 512-sample real spectrum, 10 features, and 10 multiply-adds"},
    {.name = "all_feature_logistic",
     .kind = MethodKind::kAllFeatureLogistic,
     .added_decision_latency_us = 30'000,
     .complexity = "one 512-sample real spectrum, 18 features, and 18 multiply-adds"},
    {.name = "diagonal_gaussian",
     .kind = MethodKind::kDiagonalGaussian,
     .added_decision_latency_us = 30'000,
     .complexity = "one 512-sample real spectrum and 36 standardized likelihood terms"},
}};

struct Standardizer {
  std::vector<double> mean;
  std::vector<double> scale;
};

struct Model {
  MethodKind kind = MethodKind::kPeakAmplitude;
  std::vector<std::size_t> feature_indices;
  Standardizer standardizer;
  std::vector<double> weights;
  double intercept = 0.0;
  std::vector<double> positive_template;
  std::vector<double> negative_template;
  std::vector<double> positive_mean;
  std::vector<double> negative_mean;
  std::vector<double> pooled_variance;
};

struct ScoredExample {
  const Example *example = nullptr;
  double score = 0.0;
};

struct TerminalMetrics {
  std::size_t target_first = 0;
  std::size_t false_early = 0;
  std::size_t late = 0;
  std::size_t no_candidate = 0;
  std::size_t accepted_targets = 0;
  std::size_t accepted_false_candidates = 0;
  std::int64_t total_absolute_terminal_error_us = 0;
  std::int64_t total_absolute_target_first_error_us = 0;
  std::vector<nlohmann::json> windows;
};

std::size_t FrameAt(std::int64_t time_us, std::uint32_t sample_rate_hz) {
  if (time_us <= 0) {
    return 0;
  }
  const auto frame = static_cast<std::uint64_t>(time_us) * sample_rate_hz / 1'000'000U;
  return static_cast<std::size_t>(frame);
}

double NormalizedSample(const DecodedMonoPcmS16Wav &audio, std::int64_t frame) {
  if (frame < 0 || static_cast<std::size_t>(frame) >= audio.samples.size()) {
    return 0.0;
  }
  return static_cast<double>(audio.samples[static_cast<std::size_t>(frame)]) / 32768.0;
}

std::int64_t RelativeFrames(double seconds, std::uint32_t sample_rate_hz) {
  return std::llround(seconds * static_cast<double>(sample_rate_hz));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<double> HighPassContext(const DecodedMonoPcmS16Wav &audio, std::int64_t start_frame,
                                    std::size_t count) {
  std::vector<double> filtered(count);
  double previous_input = NormalizedSample(audio, start_frame - 1);
  double previous_output = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    const double input = NormalizedSample(audio, start_frame + static_cast<std::int64_t>(index));
    const double output = input - previous_input + 0.98 * previous_output;
    filtered[index] = output;
    previous_input = input;
    previous_output = output;
  }
  return filtered;
}

double Energy(std::span<const double> samples, std::size_t begin, std::size_t end) {
  begin = std::min(begin, samples.size());
  end = std::min(std::max(end, begin + (begin < samples.size() ? 1U : 0U)), samples.size());
  if (begin >= end) {
    return kMinimumPower;
  }
  double sum = 0.0;
  for (std::size_t index = begin; index < end; ++index) {
    sum += samples[index] * samples[index];
  }
  return std::max(kMinimumPower, sum / static_cast<double>(end - begin));
}

std::vector<double> NormalizeVector(std::vector<double> values) {
  if (values.empty()) {
    return values;
  }
  const double mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double norm_squared = 0.0;
  for (double &value : values) {
    value -= mean;
    norm_squared += value * value;
  }
  const double norm = std::sqrt(norm_squared);
  if (norm > 1.0e-12) {
    for (double &value : values) {
      value /= norm;
    }
  }
  return values;
}

std::vector<double> PowerSpectrum(std::span<const double> samples) {
  std::vector<double> power(kSpectrumFrameSamples / 2U + 1U);
  for (std::size_t frequency = 0; frequency < power.size(); ++frequency) {
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t index = 0; index < kSpectrumFrameSamples; ++index) {
      const double window =
          0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                               static_cast<double>(kSpectrumFrameSamples - 1U));
      const double angle = -2.0 * std::numbers::pi * static_cast<double>(frequency * index) /
                           static_cast<double>(kSpectrumFrameSamples);
      const double value = samples[index] * window;
      real += value * std::cos(angle);
      imaginary += value * std::sin(angle);
    }
    power[frequency] = real * real + imaginary * imaginary;
  }
  return power;
}

double Dot(std::span<const double> left, std::span<const double> right) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("vector dimensions do not match");
  }
  return std::inner_product(left.begin(), left.end(), right.begin(), 0.0);
}

std::vector<double> MeanUnitVectors(const std::vector<const Example *> &examples, bool target) {
  std::vector<double> mean(kSpectrumBands, 0.0);
  std::size_t count = 0;
  for (const Example *example : examples) {
    if (example->target != target) {
      continue;
    }
    for (std::size_t index = 0; index < mean.size(); ++index) {
      mean[index] += example->features.normalized_log_spectrum[index];
    }
    ++count;
  }
  if (count == 0) {
    throw std::invalid_argument("both target and false training candidates are required");
  }
  for (double &value : mean) {
    value /= static_cast<double>(count);
  }
  const double norm = std::sqrt(Dot(mean, mean));
  if (norm > 1.0e-12) {
    for (double &value : mean) {
      value /= norm;
    }
  }
  return mean;
}

std::vector<std::size_t> FeatureIndices(MethodKind kind) {
  switch (kind) {
    case MethodKind::kTemporalLogistic:
      return {kTemporalFeatureIndices.begin(), kTemporalFeatureIndices.end()};
    case MethodKind::kSpectroTemporalLogistic:
      return {kSpectroTemporalFeatureIndices.begin(), kSpectroTemporalFeatureIndices.end()};
    case MethodKind::kAllFeatureLogistic:
    case MethodKind::kDiagonalGaussian:
      return {kAllFeatureIndices.begin(), kAllFeatureIndices.end()};
    default:
      return {};
  }
}

Standardizer FitStandardizer(const std::vector<const Example *> &examples,
                             std::span<const std::size_t> indices) {
  Standardizer result{.mean = std::vector<double>(indices.size(), 0.0),
                      .scale = std::vector<double>(indices.size(), 1.0)};
  for (const Example *example : examples) {
    for (std::size_t index = 0; index < indices.size(); ++index) {
      result.mean[index] += example->features.engineered[indices[index]];
    }
  }
  for (double &value : result.mean) {
    value /= static_cast<double>(examples.size());
  }
  for (const Example *example : examples) {
    for (std::size_t index = 0; index < indices.size(); ++index) {
      const double centered = example->features.engineered[indices[index]] - result.mean[index];
      result.scale[index] += centered * centered;
    }
  }
  for (double &value : result.scale) {
    value = std::max(1.0e-6, std::sqrt(value / static_cast<double>(examples.size())));
  }
  return result;
}

std::vector<double> Standardized(const Example &example, std::span<const std::size_t> indices,
                                 const Standardizer &standardizer) {
  std::vector<double> values(indices.size());
  for (std::size_t index = 0; index < indices.size(); ++index) {
    values[index] = (example.features.engineered[indices[index]] - standardizer.mean[index]) /
                    standardizer.scale[index];
  }
  return values;
}

double Sigmoid(double value) {
  if (value >= 0.0) {
    const double exponent = std::exp(-value);
    return 1.0 / (1.0 + exponent);
  }
  const double exponent = std::exp(value);
  return exponent / (1.0 + exponent);
}

// The method dispatch keeps every experimental model behind one deterministic
// fitting contract; splitting it would obscure the shared class balancing.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Model TrainModel(MethodKind kind, const std::vector<const Example *> &examples) {
  if (examples.empty()) {
    throw std::invalid_argument("model training set cannot be empty");
  }
  const auto positive_count = static_cast<std::size_t>(
      std::ranges::count_if(examples, [](const Example *example) { return example->target; }));
  const std::size_t negative_count = examples.size() - positive_count;
  if (positive_count == 0 || negative_count == 0) {
    throw std::invalid_argument("model training requires both candidate classes");
  }

  Model model;
  model.kind = kind;
  if (kind == MethodKind::kPeakAmplitude) {
    return model;
  }
  if (kind == MethodKind::kPositiveSpectralTemplate ||
      kind == MethodKind::kContrastiveSpectralTemplate) {
    model.positive_template = MeanUnitVectors(examples, true);
    if (kind == MethodKind::kContrastiveSpectralTemplate) {
      model.negative_template = MeanUnitVectors(examples, false);
    }
    return model;
  }

  model.feature_indices = FeatureIndices(kind);
  model.standardizer = FitStandardizer(examples, model.feature_indices);
  const std::size_t dimensions = model.feature_indices.size();
  if (kind == MethodKind::kDiagonalGaussian) {
    model.positive_mean.assign(dimensions, 0.0);
    model.negative_mean.assign(dimensions, 0.0);
    for (const Example *example : examples) {
      const auto values = Standardized(*example, model.feature_indices, model.standardizer);
      auto &mean = example->target ? model.positive_mean : model.negative_mean;
      for (std::size_t index = 0; index < dimensions; ++index) {
        mean[index] += values[index];
      }
    }
    for (double &value : model.positive_mean) {
      value /= static_cast<double>(positive_count);
    }
    for (double &value : model.negative_mean) {
      value /= static_cast<double>(negative_count);
    }
    model.pooled_variance.assign(dimensions, 0.25);
    for (const Example *example : examples) {
      const auto values = Standardized(*example, model.feature_indices, model.standardizer);
      const auto &mean = example->target ? model.positive_mean : model.negative_mean;
      for (std::size_t index = 0; index < dimensions; ++index) {
        const double difference = values[index] - mean[index];
        model.pooled_variance[index] += difference * difference;
      }
    }
    for (double &value : model.pooled_variance) {
      value /= static_cast<double>(examples.size());
      value = std::max(0.25, value);
    }
    return model;
  }

  model.weights.assign(dimensions, 0.0);
  for (std::size_t iteration = 0; iteration < 1'500; ++iteration) {
    std::vector<double> gradient(dimensions, 0.0);
    double intercept_gradient = 0.0;
    for (const Example *example : examples) {
      const auto values = Standardized(*example, model.feature_indices, model.standardizer);
      const double prediction = Sigmoid(model.intercept + Dot(model.weights, values));
      const double label = example->target ? 1.0 : 0.0;
      const double class_weight = example->target ? 0.5 / static_cast<double>(positive_count)
                                                  : 0.5 / static_cast<double>(negative_count);
      const double residual = (prediction - label) * class_weight;
      intercept_gradient += residual;
      for (std::size_t index = 0; index < dimensions; ++index) {
        gradient[index] += residual * values[index];
      }
    }
    constexpr double kL2 = 0.08;
    for (std::size_t index = 0; index < dimensions; ++index) {
      gradient[index] += kL2 * model.weights[index];
    }
    const double step = 0.3 / std::sqrt(1.0 + static_cast<double>(iteration) / 100.0);
    model.intercept -= step * intercept_gradient;
    for (std::size_t index = 0; index < dimensions; ++index) {
      model.weights[index] -= step * gradient[index];
    }
  }
  return model;
}

double Score(const Model &model, const Example &example) {
  switch (model.kind) {
    case MethodKind::kPeakAmplitude:
      return example.features.engineered.front();
    case MethodKind::kPositiveSpectralTemplate:
      return Dot(model.positive_template, example.features.normalized_log_spectrum);
    case MethodKind::kContrastiveSpectralTemplate:
      return Dot(model.positive_template, example.features.normalized_log_spectrum) -
             Dot(model.negative_template, example.features.normalized_log_spectrum);
    case MethodKind::kTemporalLogistic:
    case MethodKind::kSpectroTemporalLogistic:
    case MethodKind::kAllFeatureLogistic: {
      const auto values = Standardized(example, model.feature_indices, model.standardizer);
      return model.intercept + Dot(model.weights, values);
    }
    case MethodKind::kDiagonalGaussian: {
      const auto values = Standardized(example, model.feature_indices, model.standardizer);
      double score = 0.0;
      for (std::size_t index = 0; index < values.size(); ++index) {
        const double positive = values[index] - model.positive_mean[index];
        const double negative = values[index] - model.negative_mean[index];
        score += (negative * negative - positive * positive) / (2.0 * model.pooled_variance[index]);
      }
      return score;
    }
  }
  throw std::logic_error("unknown spectral experiment method");
}

std::vector<const Example *> SelectTraining(const std::vector<Example> &examples,
                                            std::string_view excluded_outer,
                                            std::optional<std::string_view> excluded_inner) {
  std::vector<const Example *> selected;
  for (const Example &example : examples) {
    if (example.shot_id == excluded_outer ||
        (excluded_inner.has_value() && example.shot_id == *excluded_inner)) {
      continue;
    }
    selected.push_back(&example);
  }
  return selected;
}

std::vector<ScoredExample> ScoreShot(const std::vector<Example> &examples, std::string_view shot_id,
                                     const Model &model) {
  std::vector<ScoredExample> scored;
  for (const Example &example : examples) {
    if (example.shot_id == shot_id) {
      scored.push_back({.example = &example, .score = Score(model, example)});
    }
  }
  return scored;
}

TerminalMetrics ScoreTerminals(std::span<const ScoredExample> scored, double threshold) {
  TerminalMetrics metrics;
  std::map<std::pair<std::string, std::string>, std::vector<const ScoredExample *>> groups;
  for (const ScoredExample &item : scored) {
    groups[{item.example->shot_id, item.example->view}].push_back(&item);
    if (item.score >= threshold) {
      metrics.accepted_targets += item.example->target ? 1U : 0U;
      metrics.accepted_false_candidates += item.example->target ? 0U : 1U;
    }
  }
  for (auto &[key, group] : groups) {
    std::ranges::sort(group, {},
                      [](const ScoredExample *item) { return item->example->candidate_us; });
    std::vector<experiments::AcceptedAudioEvent> accepted;
    for (const ScoredExample *item : group) {
      if (item->score >= threshold) {
        accepted.push_back({.time_us = item->example->candidate_us});
      }
    }
    const Example &reference = *group.front()->example;
    const experiments::TerminalBenchmarkResult benchmark =
        experiments::BenchmarkTerminalWindows(accepted,
                                              {{.id = key.first,
                                                .arm_us = reference.arm_us,
                                                .ready_us = reference.ready_us,
                                                .target_us = reference.target_us,
                                                .end_us = reference.end_us}},
                                              kTargetToleranceUs);
    const experiments::TerminalWindowResult &result = benchmark.windows.front();
    std::string outcome;
    switch (result.outcome) {
      case experiments::TerminalOutcome::kTargetFirst:
        outcome = "target_first";
        ++metrics.target_first;
        metrics.total_absolute_target_first_error_us +=
            std::abs(result.error_from_target_us.value_or(0));
        break;
      case experiments::TerminalOutcome::kFalseEarly:
        outcome = "false_early";
        ++metrics.false_early;
        break;
      case experiments::TerminalOutcome::kLate:
        outcome = "late";
        ++metrics.late;
        break;
      case experiments::TerminalOutcome::kNoCandidate:
        outcome = "no_candidate";
        ++metrics.no_candidate;
        break;
    }
    const nlohmann::json terminal_us =
        result.terminal_event_us.has_value()
            ? nlohmann::json(std::to_string(*result.terminal_event_us))
            : nlohmann::json(nullptr);
    const nlohmann::json terminal_error_us =
        result.error_from_target_us.has_value()
            ? nlohmann::json(std::to_string(*result.error_from_target_us))
            : nlohmann::json(nullptr);
    if (result.error_from_target_us.has_value()) {
      metrics.total_absolute_terminal_error_us += std::abs(*result.error_from_target_us);
    }
    metrics.windows.push_back({{"shot_id", key.first},
                               {"view", key.second},
                               {"outcome", outcome},
                               {"terminal_us", terminal_us},
                               {"terminal_error_us", terminal_error_us}});
  }
  return metrics;
}

auto TerminalRank(const TerminalMetrics &metrics, double threshold) {
  return std::tuple(metrics.target_first, -static_cast<std::int64_t>(metrics.false_early),
                    metrics.accepted_targets,
                    -static_cast<std::int64_t>(metrics.accepted_false_candidates),
                    -metrics.total_absolute_terminal_error_us, threshold);
}

struct ThresholdNeighborhood {
  double permissive = -std::numeric_limits<double>::infinity();
  double selected = -std::numeric_limits<double>::infinity();
  double selective = std::numeric_limits<double>::infinity();
};

ThresholdNeighborhood SelectThreshold(std::span<const ScoredExample> scored) {
  std::vector<double> thresholds;
  thresholds.reserve(scored.size() + 2U);
  thresholds.push_back(-std::numeric_limits<double>::infinity());
  for (const ScoredExample &item : scored) {
    thresholds.push_back(item.score);
  }
  thresholds.push_back(std::numeric_limits<double>::infinity());
  std::ranges::sort(thresholds);
  const auto unique_end = std::ranges::unique(thresholds).begin();
  thresholds.erase(unique_end, thresholds.end());
  std::size_t best_index = 0;
  TerminalMetrics best = ScoreTerminals(scored, thresholds.front());
  for (std::size_t index = 1; index < thresholds.size(); ++index) {
    const TerminalMetrics candidate = ScoreTerminals(scored, thresholds[index]);
    if (TerminalRank(candidate, thresholds[index]) > TerminalRank(best, thresholds[best_index])) {
      best = candidate;
      best_index = index;
    }
  }
  return {.permissive = thresholds[best_index == 0 ? 0 : best_index - 1],
          .selected = thresholds[best_index],
          .selective = thresholds[std::min(best_index + 1, thresholds.size() - 1)]};
}

nlohmann::json MetricsJson(const TerminalMetrics &metrics) {
  const std::size_t terminal_count = metrics.target_first + metrics.false_early + metrics.late;
  nlohmann::json by_view = nlohmann::json::object();
  for (const nlohmann::json &window : metrics.windows) {
    const std::string view = window.at("view").get<std::string>();
    if (!by_view.contains(view)) {
      by_view[view] = {{"target_first", 0}, {"false_early", 0}, {"late", 0}, {"no_candidate", 0}};
    }
    const std::string outcome = window.at("outcome").get<std::string>();
    by_view[view][outcome] = by_view[view][outcome].get<std::size_t>() + 1U;
  }
  return {
      {"target_first", metrics.target_first},
      {"false_early", metrics.false_early},
      {"late", metrics.late},
      {"no_candidate", metrics.no_candidate},
      {"accepted_target_candidates", metrics.accepted_targets},
      {"accepted_false_candidates", metrics.accepted_false_candidates},
      {"mean_absolute_terminal_error_us",
       terminal_count == 0 ? nlohmann::json(nullptr)
                           : nlohmann::json(metrics.total_absolute_terminal_error_us /
                                            static_cast<std::int64_t>(terminal_count))},
      {"mean_absolute_target_first_error_us",
       metrics.target_first == 0 ? nlohmann::json(nullptr)
                                 : nlohmann::json(metrics.total_absolute_target_first_error_us /
                                                  static_cast<std::int64_t>(metrics.target_first))},
      {"by_view", std::move(by_view)},
      {"windows", metrics.windows}};
}

std::int64_t ParseIntegerString(const nlohmann::json &value, std::string_view name) {
  if (!value.is_string()) {
    throw std::invalid_argument(std::string(name) + " must be a decimal string");
  }
  const std::string encoded = value.get<std::string>();
  std::size_t consumed = 0;
  const std::int64_t parsed = std::stoll(encoded, &consumed);
  if (consumed != encoded.size()) {
    throw std::invalid_argument(std::string(name) + " is invalid");
  }
  return parsed;
}

std::vector<Example> ParseExamples(const RecordingInput &recording) {
  if (recording.view.empty() || recording.device.empty() || recording.audio.sample_rate_hz == 0 ||
      recording.audio.samples.empty()) {
    throw std::invalid_argument("recording input is incomplete");
  }
  if (recording.production_windows.at("schema_version") != 1 ||
      recording.production_windows.at("target_tolerance_us") != "100000") {
    throw std::invalid_argument("production window report does not match benchmark tolerance");
  }
  std::vector<Example> examples;
  for (const auto &window : recording.production_windows.at("windows")) {
    const std::string shot_id = window.at("id").get<std::string>();
    const std::int64_t arm_us = ParseIntegerString(window.at("start_us"), "start_us");
    const std::int64_t target_us = ParseIntegerString(window.at("target_us"), "target_us");
    const std::int64_t end_us = ParseIntegerString(window.at("end_us"), "end_us");
    for (const auto &candidate : window.at("candidates")) {
      const std::int64_t candidate_us =
          ParseIntegerString(candidate.at("time_us"), "candidate time_us");
      examples.push_back({.shot_id = shot_id,
                          .view = recording.view,
                          .device = recording.device,
                          .candidate_us = candidate_us,
                          .arm_us = arm_us,
                          .ready_us = arm_us + 2'450'000,
                          .target_us = target_us,
                          .end_us = end_us,
                          .target = std::abs(candidate_us - target_us) <= kTargetToleranceUs,
                          .features = ExtractImpactFeatures(recording.audio, candidate_us)});
    }
  }
  return examples;
}

nlohmann::json EvaluateMethod(const MethodSpec &method, const std::vector<Example> &examples,
                              const std::vector<std::string> &shot_ids) {
  std::array<std::vector<ScoredExample>, 3> outer_scores;
  nlohmann::json folds = nlohmann::json::array();
  for (const std::string &outer_shot : shot_ids) {
    std::vector<ScoredExample> inner_scores;
    for (const std::string &inner_shot : shot_ids) {
      if (inner_shot == outer_shot) {
        continue;
      }
      const auto inner_training = SelectTraining(examples, outer_shot, inner_shot);
      const Model inner_model = TrainModel(method.kind, inner_training);
      auto scored = ScoreShot(examples, inner_shot, inner_model);
      inner_scores.insert(inner_scores.end(), scored.begin(), scored.end());
    }
    const ThresholdNeighborhood thresholds = SelectThreshold(inner_scores);
    const auto outer_training = SelectTraining(examples, outer_shot, std::nullopt);
    const Model outer_model = TrainModel(method.kind, outer_training);
    const auto held_out = ScoreShot(examples, outer_shot, outer_model);
    const std::array<double, 3> values = {thresholds.permissive, thresholds.selected,
                                          thresholds.selective};
    for (std::size_t index = 0; index < outer_scores.size(); ++index) {
      for (const ScoredExample &item : held_out) {
        outer_scores[index].push_back(
            {.example = item.example, .score = item.score - values[index]});
      }
    }
    folds.push_back({{"held_out_shot", outer_shot},
                     {"training_shot_count", shot_ids.size() - 1U},
                     {"threshold_permissive", thresholds.permissive},
                     {"threshold_selected", thresholds.selected},
                     {"threshold_selective", thresholds.selective}});
  }
  const TerminalMetrics permissive = ScoreTerminals(outer_scores[0], 0.0);
  const TerminalMetrics selected = ScoreTerminals(outer_scores[1], 0.0);
  const TerminalMetrics selective = ScoreTerminals(outer_scores[2], 0.0);
  return {{"name", method.name},
          {"added_decision_latency_us", method.added_decision_latency_us},
          {"compute_complexity", method.complexity},
          {"evaluation", "nested_leave_one_swing_out"},
          {"selected", MetricsJson(selected)},
          {"threshold_neighborhood",
           {{"one_inner_score_more_permissive", MetricsJson(permissive)},
            {"selected", MetricsJson(selected)},
            {"one_inner_score_more_selective", MetricsJson(selective)}}},
          {"folds", std::move(folds)}};
}

std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t> ReportRank(
    const nlohmann::json &method) {
  const auto &metrics = method.at("selected");
  return {metrics.at("target_first").get<std::int64_t>(),
          -metrics.at("false_early").get<std::int64_t>(),
          -metrics.at("no_candidate").get<std::int64_t>(),
          -method.at("added_decision_latency_us").get<std::int64_t>()};
}

}  // namespace

// The feature families intentionally share a single peak and spectrum so the
// experiment cannot accidentally compare differently centered candidates.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ImpactFeatures ExtractImpactFeatures(const DecodedMonoPcmS16Wav &audio,
                                     std::int64_t candidate_time_us) {
  if (audio.sample_rate_hz < 40'000 || audio.samples.empty() || candidate_time_us < 20'000) {
    throw std::invalid_argument("impact feature input must be nonempty >=40 kHz PCM with context");
  }
  const auto candidate_frame =
      static_cast<std::int64_t>(FrameAt(candidate_time_us, audio.sample_rate_hz));
  const std::int64_t context_before = RelativeFrames(0.020, audio.sample_rate_hz);
  const auto context_count = static_cast<std::size_t>(RelativeFrames(0.060, audio.sample_rate_hz));
  const std::int64_t context_start = candidate_frame - context_before;
  const std::vector<double> context = HighPassContext(audio, context_start, context_count);
  const auto candidate_index = static_cast<std::size_t>(context_before);
  const auto search_radius = static_cast<std::size_t>(RelativeFrames(0.003, audio.sample_rate_hz));
  const std::size_t search_begin = candidate_index - search_radius;
  const std::size_t search_end = candidate_index + search_radius + 1U;
  const auto search = std::span(context).subspan(search_begin, search_end - search_begin);
  const auto peak_iterator =
      std::ranges::max_element(search, {}, [](double value) { return std::abs(value); });
  const std::size_t peak_index =
      search_begin + static_cast<std::size_t>(peak_iterator - search.begin());
  const double peak = std::max(1.0e-9, std::abs(context[peak_index]));

  const auto relative_index = [&](double seconds) {
    const std::int64_t index =
        static_cast<std::int64_t>(peak_index) + RelativeFrames(seconds, audio.sample_rate_hz);
    return static_cast<std::size_t>(std::max<std::int64_t>(0, index));
  };
  const double pre_energy = Energy(context, relative_index(-0.005), relative_index(-0.0005));
  const double short_energy = Energy(context, relative_index(0.0), relative_index(0.003));
  const double early_energy = Energy(context, relative_index(0.0), relative_index(0.005));
  const double late_energy = Energy(context, relative_index(0.010), relative_index(0.030));
  const double long_energy = Energy(context, relative_index(-0.005), relative_index(0.030));
  const double post_energy = Energy(context, relative_index(0.0), relative_index(0.030));

  const std::size_t half_begin = relative_index(-0.002);
  const std::size_t half_end = relative_index(0.015);
  std::size_t half_peak_samples = 0;
  for (std::size_t index = half_begin; index < half_end && index < context.size(); ++index) {
    half_peak_samples += std::abs(context[index]) >= peak * 0.5 ? 1U : 0U;
  }
  const std::size_t zcr_begin = relative_index(-0.001);
  const std::size_t zcr_end = std::min(relative_index(0.010), context.size());
  std::size_t crossings = 0;
  for (std::size_t index = zcr_begin + 1U; index < zcr_end; ++index) {
    crossings += std::signbit(context[index]) != std::signbit(context[index - 1U]) ? 1U : 0U;
  }
  const double zcr = static_cast<double>(crossings) /
                     static_cast<double>(std::max<std::size_t>(1, zcr_end - zcr_begin - 1U));

  const auto spectrum_pre_samples =
      static_cast<std::size_t>(RelativeFrames(0.002, audio.sample_rate_hz));
  const std::size_t spectrum_start = peak_index - spectrum_pre_samples;
  if (spectrum_start + kSpectrumFrameSamples > context.size() ||
      spectrum_start < kSpectrumFrameSamples) {
    throw std::invalid_argument("impact candidate does not have enough spectral context");
  }
  const std::vector<double> power =
      PowerSpectrum(std::span(context).subspan(spectrum_start, kSpectrumFrameSamples));
  const std::vector<double> previous_power = PowerSpectrum(
      std::span(context).subspan(spectrum_start - kSpectrumFrameSamples, kSpectrumFrameSamples));
  const double bin_hz =
      static_cast<double>(audio.sample_rate_hz) / static_cast<double>(kSpectrumFrameSamples);
  const auto first_bin = static_cast<std::size_t>(std::ceil(100.0 / bin_hz));
  const std::size_t last_bin =
      std::min(power.size() - 1U, static_cast<std::size_t>(std::floor(20'000.0 / bin_hz)));
  double total_power = 0.0;
  double weighted_frequency = 0.0;
  double log_power_sum = 0.0;
  for (std::size_t index = first_bin; index <= last_bin; ++index) {
    total_power += power[index];
    weighted_frequency += static_cast<double>(index) * bin_hz * power[index];
    log_power_sum += std::log(std::max(kMinimumPower, power[index]));
  }
  total_power = std::max(kMinimumPower, total_power);
  const double centroid = weighted_frequency / total_power / 20'000.0;
  double cumulative = 0.0;
  std::size_t rolloff_bin = first_bin;
  for (std::size_t index = first_bin; index <= last_bin; ++index) {
    cumulative += power[index];
    if (cumulative >= total_power * 0.85) {
      rolloff_bin = index;
      break;
    }
  }
  const double arithmetic_mean = total_power / static_cast<double>(last_bin - first_bin + 1U);
  const double geometric_mean =
      std::exp(log_power_sum / static_cast<double>(last_bin - first_bin + 1U));
  const double flatness = geometric_mean / std::max(kMinimumPower, arithmetic_mean);
  double current_norm = 0.0;
  double previous_norm = 0.0;
  for (std::size_t index = first_bin; index <= last_bin; ++index) {
    current_norm += power[index];
    previous_norm += previous_power[index];
  }
  double flux = 0.0;
  for (std::size_t index = first_bin; index <= last_bin; ++index) {
    const double difference = power[index] / std::max(kMinimumPower, current_norm) -
                              previous_power[index] / std::max(kMinimumPower, previous_norm);
    flux += difference * difference;
  }
  flux = std::sqrt(flux);

  const std::array<double, 6> band_edges = {100.0, 1'000.0, 3'000.0, 6'000.0, 12'000.0, 20'000.0};
  std::array<double, 5> band_fractions{};
  for (std::size_t band = 0; band < band_fractions.size(); ++band) {
    for (std::size_t index = first_bin; index <= last_bin; ++index) {
      const double frequency = static_cast<double>(index) * bin_hz;
      if (frequency >= band_edges[band] && frequency < band_edges[band + 1U]) {
        band_fractions[band] += power[index] / total_power;
      }
    }
  }

  std::vector<double> log_spectrum(kSpectrumBands, std::log(kMinimumPower));
  const double log_min = std::log(100.0);
  const double log_max = std::log(20'000.0);
  std::array<std::size_t, kSpectrumBands> band_counts{};
  for (std::size_t index = first_bin; index <= last_bin; ++index) {
    const double frequency = static_cast<double>(index) * bin_hz;
    const double position = (std::log(frequency) - log_min) / (log_max - log_min);
    const std::size_t band = std::min(
        kSpectrumBands - 1U,
        static_cast<std::size_t>(std::max(0.0, position) * static_cast<double>(kSpectrumBands)));
    if (band_counts[band] == 0) {
      log_spectrum[band] = 0.0;
    }
    log_spectrum[band] += std::log(std::max(kMinimumPower, power[index] / total_power));
    ++band_counts[band];
  }
  for (std::size_t band = 0; band < log_spectrum.size(); ++band) {
    if (band_counts[band] != 0) {
      log_spectrum[band] /= static_cast<double>(band_counts[band]);
    }
  }

  ImpactFeatures result;
  result.engineered = {
      std::log(peak),
      0.5 * std::log(short_energy),
      0.5 * std::log(long_energy),
      peak / std::sqrt(long_energy),
      std::log(short_energy / pre_energy),
      std::log(early_energy / late_energy),
      short_energy / post_energy,
      static_cast<double>(half_peak_samples) * 1'000.0 / static_cast<double>(audio.sample_rate_hz),
      zcr,
      centroid,
      static_cast<double>(rolloff_bin) * bin_hz / 20'000.0,
      flatness,
      flux,
      band_fractions[0],
      band_fractions[1],
      band_fractions[2],
      band_fractions[3],
      band_fractions[4],
  };
  result.normalized_log_spectrum = NormalizeVector(std::move(log_spectrum));
  return result;
}

nlohmann::json EvaluateSpectralImpactExperiment(const std::vector<RecordingInput> &recordings) {
  if (recordings.size() < 2) {
    throw std::invalid_argument("spectral experiment requires both phone recordings");
  }
  std::vector<Example> examples;
  nlohmann::json generation_by_view = nlohmann::json::object();
  std::vector<std::string> shot_ids;
  for (const RecordingInput &recording : recordings) {
    auto parsed = ParseExamples(recording);
    const auto target_count = static_cast<std::size_t>(
        std::ranges::count_if(parsed, [](const Example &example) { return example.target; }));
    const std::size_t false_count = parsed.size() - target_count;
    const double duration_minutes = static_cast<double>(recording.audio.samples.size()) /
                                    static_cast<double>(recording.audio.sample_rate_hz) / 60.0;
    std::vector<std::string> recalled;
    for (const Example &example : parsed) {
      if (example.target && std::ranges::find(recalled, example.shot_id) == recalled.end()) {
        recalled.push_back(example.shot_id);
      }
      if (std::ranges::find(shot_ids, example.shot_id) == shot_ids.end()) {
        shot_ids.push_back(example.shot_id);
      }
    }
    generation_by_view[recording.view] = {
        {"device", recording.device},
        {"target_recall", recalled.size()},
        {"target_total", recording.production_windows.at("window_count")},
        {"credited_target_candidates", target_count},
        {"non_credited_candidates", false_count},
        {"false_candidates_per_minute", static_cast<double>(false_count) / duration_minutes},
    };
    examples.insert(examples.end(), std::make_move_iterator(parsed.begin()),
                    std::make_move_iterator(parsed.end()));
  }
  std::ranges::sort(shot_ids);
  if (shot_ids.size() < 4) {
    throw std::invalid_argument("at least four swing IDs are required for nested evaluation");
  }

  // The face-on Pixel 6 is the frozen leader authority. Put every phone on the
  // leader's per-shot arm timeline by carrying the target-axis offset into the
  // local WAV domain. This retains all pre-ready candidates as diagnostics but
  // prevents them from becoming terminal events.
  std::map<std::string, std::pair<std::int64_t, std::int64_t>> leader_timing;
  for (const Example &example : examples) {
    if (example.view == "face_on") {
      leader_timing[example.shot_id] = {example.arm_us, example.target_us};
    }
  }
  for (Example &example : examples) {
    const auto timing = leader_timing.find(example.shot_id);
    if (timing == leader_timing.end()) {
      throw std::invalid_argument("face-on leader is missing a swing timing reference");
    }
    example.arm_us = timing->second.first + example.target_us - timing->second.second;
    example.ready_us = example.arm_us + 2'450'000;
  }

  std::vector<ScoredExample> unfiltered_candidates;
  unfiltered_candidates.reserve(examples.size());
  for (const Example &example : examples) {
    unfiltered_candidates.push_back({.example = &example, .score = 0.0});
  }
  const nlohmann::json unfiltered_terminal_baseline =
      MetricsJson(ScoreTerminals(unfiltered_candidates, -std::numeric_limits<double>::infinity()));

  std::vector<nlohmann::json> methods;
  methods.reserve(kMethods.size());
  for (const MethodSpec &method : kMethods) {
    methods.push_back(EvaluateMethod(method, examples, shot_ids));
  }
  std::ranges::sort(methods, [](const nlohmann::json &left, const nlohmann::json &right) {
    return ReportRank(left) > ReportRank(right);
  });
  for (std::size_t index = 0; index < methods.size(); ++index) {
    methods[index]["rank"] = index + 1U;
  }
  nlohmann::json methods_json = nlohmann::json::array();
  for (nlohmann::json &method : methods) {
    methods_json.push_back(std::move(method));
  }
  nlohmann::json feature_names = nlohmann::json::array();
  for (const std::string_view name : kFeatureNames) {
    feature_names.push_back(name);
  }
  return {
      {"schema_version", 1},
      {"experiment", "spectral_impact_classification"},
      {"development_evidence_only", true},
      {"candidate_generation", generation_by_view},
      {"candidate_feature_count", kFeatureNames.size()},
      {"candidate_feature_names", std::move(feature_names)},
      {"spectrum", {{"frame_samples", kSpectrumFrameSamples}, {"log_bands", kSpectrumBands}}},
      {"target_tolerance_us", kTargetToleranceUs},
      {"audio_trigger_ready_budget_us", 2'450'000},
      {"leader_authority", "face_on"},
      {"training_policy",
       "nested leave-one-swing-out; both phone examples for outer swing excluded from model, "
       "template, and inner threshold selection"},
      {"threshold_policy",
       "inner leave-one-swing-out threshold maximizes target-first windows, then minimizes "
       "false terminals and false accepted candidates"},
      {"unfiltered_production_candidate_terminal_baseline", unfiltered_terminal_baseline},
      {"methods", std::move(methods_json)},
      {"complete_capture_lifecycle",
       {{"evaluated", false},
        {"reason",
         "isolated classifier branch requires integration into canonical ATL-led "
         "pose/camera lifecycle replay before production selection"}}},
      {"limitations",
       {"one 12-swing development session",
        "device, view, mounting location, and acoustics are confounded",
        "production candidate windows contain few ATL negatives",
        "model selection remains provisional until phone-swap and negative-rich holdout data",
        "naive offline DFT cost is reported structurally; production should use a platform FFT"}},
  };
}

std::string RenderSpectralImpactExperimentMarkdown(const nlohmann::json &report) {
  std::string markdown;
  markdown += "# Spectral impact-classification experiment\n\n";
  markdown +=
      "This is a development-set ranking, not a production accuracy claim. Both phone "
      "examples for each evaluated swing were excluded from fitting, templates, and "
      "threshold selection.\n\n";
  markdown += "## Candidate generation\n\n";
  markdown += "| View | Device | Recall | Non-credited candidates | False/min |\n";
  markdown += "| --- | --- | ---: | ---: | ---: |\n";
  for (const auto &[view, generation] : report.at("candidate_generation").items()) {
    markdown += "| " + view + " | " + generation.at("device").get<std::string>() + " | " +
                std::to_string(generation.at("target_recall").get<std::size_t>()) + "/" +
                std::to_string(generation.at("target_total").get<std::size_t>()) + " | " +
                std::to_string(generation.at("non_credited_candidates").get<std::size_t>()) +
                " | " + std::to_string(generation.at("false_candidates_per_minute").get<double>()) +
                " |\n";
  }
  markdown += "\n## Nested leave-one-swing-out terminal results\n\n";
  const auto &baseline = report.at("unfiltered_production_candidate_terminal_baseline");
  markdown +=
      "Accepting the first production candidate after full trigger readiness gives " +
      std::to_string(baseline.at("target_first").get<std::size_t>()) + "/24 target-first, " +
      std::to_string(baseline.at("false_early").get<std::size_t>()) + " false-early, and " +
      std::to_string(baseline.at("no_candidate").get<std::size_t>()) + " no-candidate windows.\n\n";
  markdown +=
      "| Rank | Method | Target-first | False-early | None | Accepted false | Added latency |\n";
  markdown += "| ---: | --- | ---: | ---: | ---: | ---: | ---: |\n";
  for (const auto &method : report.at("methods")) {
    const auto &selected = method.at("selected");
    markdown += "| " + std::to_string(method.at("rank").get<std::size_t>()) + " | " +
                method.at("name").get<std::string>() + " | " +
                std::to_string(selected.at("target_first").get<std::size_t>()) + "/24 | " +
                std::to_string(selected.at("false_early").get<std::size_t>()) + " | " +
                std::to_string(selected.at("no_candidate").get<std::size_t>()) + " | " +
                std::to_string(selected.at("accepted_false_candidates").get<std::size_t>()) +
                " | " +
                std::to_string(method.at("added_decision_latency_us").get<std::int64_t>() / 1'000) +
                " ms |\n";
  }
  const auto &winner = report.at("methods").front();
  const auto &winner_by_view = winner.at("selected").at("by_view");
  markdown +=
      "\nThe top nested result (`" + winner.at("name").get<std::string>() + "`) was " +
      std::to_string(winner_by_view.at("down_the_line").at("target_first").get<std::size_t>()) +
      "/" +
      std::to_string(report.at("candidate_generation")
                         .at("down_the_line")
                         .at("target_total")
                         .get<std::size_t>()) +
      " target-first on DTL and " +
      std::to_string(winner_by_view.at("face_on").at("target_first").get<std::size_t>()) + "/" +
      std::to_string(
          report.at("candidate_generation").at("face_on").at("target_total").get<std::size_t>()) +
      " on ATL. Mean absolute timestamp error across its target-first terminals was " +
      std::to_string(
          winner.at("selected").at("mean_absolute_target_first_error_us").get<std::int64_t>() /
          1'000) +
      " ms.\n";
  markdown +=
      "\nEach selected operating point was chosen only from the eleven non-held-out "
      "swings. The JSON report includes one-score-more-permissive and "
      "one-score-more-selective neighborhoods, per-fold thresholds, every terminal "
      "outcome, and timestamp errors.\n\n";
  markdown += "## Operating-point stability\n\n";
  markdown += "| Method | More permissive | Selected | More selective |\n";
  markdown += "| --- | --- | --- | --- |\n";
  for (const auto &method : report.at("methods")) {
    const auto &neighborhood = method.at("threshold_neighborhood");
    const auto summary = [](const nlohmann::json &metrics) {
      return std::to_string(metrics.at("target_first").get<std::size_t>()) + " target / " +
             std::to_string(metrics.at("false_early").get<std::size_t>()) + " false / " +
             std::to_string(metrics.at("no_candidate").get<std::size_t>()) + " none";
    };
    markdown += "| " + method.at("name").get<std::string>() + " | " +
                summary(neighborhood.at("one_inner_score_more_permissive")) + " | " +
                summary(neighborhood.at("selected")) + " | " +
                summary(neighborhood.at("one_inner_score_more_selective")) + " |\n";
  }
  markdown +=
      "\nThe winning temporal classifier is not yet threshold-stable: moving one "
      "inner-validation score in either direction reduces target-first behavior. This "
      "is evidence to test it on new data, not to ship its fitted threshold.\n\n";
  markdown += "## Interpretation and limits\n\n";
  markdown +=
      "The branch evaluates candidate classification and armed terminal behavior. It "
      "does not claim a complete capture result until a winning policy is integrated "
      "into the canonical ATL-led pose/camera lifecycle replay. A phone-swap and a "
      "negative-rich holdout recording remain mandatory before fixing production "
      "thresholds.\n";
  return markdown;
}

// NOLINTEND(misc-include-cleaner)

}  // namespace swing_capture::offline::spectral
