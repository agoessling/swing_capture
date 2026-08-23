#include "android/dual_hil/timing_calibration_evidence.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace swing_capture::android::dual_hil {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

constexpr std::int64_t kMaximumMappingAgeNs = 10'000'000'000LL;
constexpr std::int64_t kMinimumLongRunningScreenOffSeconds = 1'800LL;
constexpr std::size_t kMinimumPairClockTrials = 20U;
constexpr std::size_t kMinimumPairClockAcceptTrials = 10U;
constexpr std::size_t kMinimumPairClockRejectTrials = 5U;
constexpr std::size_t kMinimumLatencyTrialsPerDevice = 5U;
constexpr std::int64_t kMinimumDistanceSpanMm = 500LL;
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000LL;
constexpr std::uintmax_t kMaximumArtifactBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

class Sha256 {
 public:
  void Update(const char *data, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
      // SHA-256 consumes a byte buffer supplied by the bounded file-reading loop below.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      block_[block_size_++] = static_cast<std::uint8_t>(data[index]);
      ++byte_count_;
      if (block_size_ == block_.size()) {
        Transform();
        block_size_ = 0U;
      }
    }
  }

  [[nodiscard]] std::string Finish() {
    const std::uint64_t bit_count = byte_count_ * 8U;
    block_[block_size_++] = 0x80U;
    if (block_size_ > 56U) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0U);
      Transform();
      block_size_ = 0U;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56, 0U);
    for (std::size_t index = 0; index < 8U; ++index) {
      block_[63U - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
    }
    Transform();
    std::ostringstream digest;
    digest << std::hex << std::setfill('0');
    for (const std::uint32_t word : state_) {
      digest << std::setw(8) << word;
    }
    return digest.str();
  }

 private:
  static constexpr std::array<std::uint32_t, 64> kRoundConstants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
      0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
      0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
      0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
      0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
      0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
      0xc67178f2U};

  void Transform() {
    std::array<std::uint32_t, 64> schedule = {};
    for (std::size_t index = 0; index < 16U; ++index) {
      const std::size_t offset = index * 4U;
      schedule[index] = static_cast<std::uint32_t>(block_[offset]) << 24U |
                        static_cast<std::uint32_t>(block_[offset + 1U]) << 16U |
                        static_cast<std::uint32_t>(block_[offset + 2U]) << 8U |
                        static_cast<std::uint32_t>(block_[offset + 3U]);
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
      const std::uint32_t first = std::rotr(schedule[index - 15U], 7) ^
                                  std::rotr(schedule[index - 15U], 18) ^
                                  (schedule[index - 15U] >> 3U);
      const std::uint32_t second = std::rotr(schedule[index - 2U], 17) ^
                                   std::rotr(schedule[index - 2U], 19) ^
                                   (schedule[index - 2U] >> 10U);
      schedule[index] = schedule[index - 16U] + first + schedule[index - 7U] + second;
    }
    std::array<std::uint32_t, 8> work = state_;
    for (std::size_t index = 0; index < schedule.size(); ++index) {
      const std::uint32_t choice = (work[4] & work[5]) ^ (~work[4] & work[6]);
      const std::uint32_t majority =
          (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
      const std::uint32_t sum0 =
          std::rotr(work[0], 2) ^ std::rotr(work[0], 13) ^ std::rotr(work[0], 22);
      const std::uint32_t sum1 =
          std::rotr(work[4], 6) ^ std::rotr(work[4], 11) ^ std::rotr(work[4], 25);
      const std::uint32_t first =
          work[7] + sum1 + choice + kRoundConstants[index] + schedule[index];
      const std::uint32_t second = sum0 + majority;
      for (std::size_t word = work.size() - 1U; word > 0U; --word) {
        work[word] = work[word - 1U];
      }
      work[4] += first;
      work[0] = first + second;
    }
    for (std::size_t index = 0; index < state_.size(); ++index) {
      state_[index] += work[index];
    }
  }

  std::array<std::uint32_t, 8> state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                         0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> block_ = {};
  std::size_t block_size_ = 0U;
  std::uint64_t byte_count_ = 0U;
};

[[noreturn]] void Invalid(std::string message) {
  throw std::invalid_argument("timing calibration evidence: " + std::move(message));
}

void RequireExactFields(const Json &value, std::initializer_list<std::string_view> expected,
                        std::string_view label) {
  if (!value.is_object() || value.size() != expected.size()) {
    Invalid(std::string(label) + " fields do not match schema 1");
  }
  for (const std::string_view field : expected) {
    if (!value.contains(field)) {
      Invalid(std::string(label) + " lacks " + std::string(field));
    }
  }
}

std::string RequiredString(const Json &value, std::string_view field) {
  const Json &encoded = value.at(field);
  if (!encoded.is_string()) {
    Invalid(std::string(field) + " must be a string");
  }
  const std::string result = encoded.get<std::string>();
  if (result.empty() || result.size() > 128U) {
    Invalid(std::string(field) + " must be a bounded nonempty string");
  }
  return result;
}

bool HexDigit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

std::string RequiredSha256(const Json &value, std::string_view field) {
  const std::string digest = RequiredString(value, field);
  if (digest.size() != 64U || !std::ranges::all_of(digest, HexDigit)) {
    Invalid(std::string(field) + " must be a lowercase SHA-256");
  }
  return digest;
}

struct VerifiedArtifact {
  std::string relative_path;
  std::string sha256;
};

std::string Sha256File(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    Invalid("cannot open artifact " + path.string());
  }
  Sha256 sha256;
  std::array<char, std::size_t{64} * 1024U> buffer = {};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    sha256.Update(buffer.data(), static_cast<std::size_t>(input.gcount()));
  }
  if (!input.eof()) {
    Invalid("cannot read artifact " + path.string());
  }
  return sha256.Finish();
}

VerifiedArtifact VerifyArtifact(
    const Json &value,
    std::string_view path_field,  // NOLINT(bugprone-easily-swappable-parameters)
    std::string_view sha_field,   // NOLINT(bugprone-easily-swappable-parameters)
    const std::filesystem::path &canonical_root) {
  const std::string relative_text = RequiredString(value, path_field);
  const std::string expected_sha = RequiredSha256(value, sha_field);
  const std::filesystem::path relative(relative_text);
  if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
      relative.empty()) {
    Invalid(std::string(path_field) + " must be a safe relative path");
  }
  std::filesystem::path inspected = canonical_root;
  for (const std::filesystem::path &component : relative) {
    if (component.empty() || component == "." || component == "..") {
      Invalid(std::string(path_field) + " contains an unsafe path component");
    }
    inspected /= component;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(inspected, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
      Invalid(std::string(path_field) + " does not exist below the artifact root");
    }
    if (std::filesystem::is_symlink(status)) {
      Invalid(std::string(path_field) + " traverses a symbolic link");
    }
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(inspected, error) || error) {
    Invalid(std::string(path_field) + " must identify a regular file");
  }
  const std::uintmax_t artifact_bytes = std::filesystem::file_size(inspected, error);
  if (error || artifact_bytes > kMaximumArtifactBytes) {
    Invalid(std::string(path_field) + " exceeds the artifact size bound");
  }
  const std::string actual_sha = Sha256File(inspected);
  if (actual_sha != expected_sha) {
    Invalid(std::string(path_field) + " SHA-256 does not match retained bytes");
  }
  return {.relative_path = relative.generic_string(), .sha256 = actual_sha};
}

bool Digits(std::string_view value) {
  return std::ranges::all_of(value,
                             [](char character) { return character >= '0' && character <= '9'; });
}

int DecimalDigits(std::string_view value) {
  int result = 0;
  // std::from_chars uses the standard contiguous character-range pointer pair.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (error != std::errc() || end != value.data() + value.size()) {
    Invalid("reviewed_at_utc contains invalid digits");
  }
  return result;
}

void ValidateUtcTimestamp(std::string_view value) {
  if (value.size() != 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != 'Z' || !Digits(value.substr(0U, 4U)) ||
      !Digits(value.substr(5U, 2U)) || !Digits(value.substr(8U, 2U)) ||
      !Digits(value.substr(11U, 2U)) || !Digits(value.substr(14U, 2U)) ||
      !Digits(value.substr(17U, 2U))) {
    Invalid("reviewed_at_utc must be an RFC 3339 UTC second timestamp");
  }
  const int year = DecimalDigits(value.substr(0U, 4U));
  const int month = DecimalDigits(value.substr(5U, 2U));
  const int day = DecimalDigits(value.substr(8U, 2U));
  const int hour = DecimalDigits(value.substr(11U, 2U));
  const int minute = DecimalDigits(value.substr(14U, 2U));
  const int second = DecimalDigits(value.substr(17U, 2U));
  if (year < 2000 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) {
    Invalid("reviewed_at_utc is outside the supported calendar range");
  }
  constexpr std::array<int, 12> kDaysPerMonth = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maximum_day = kDaysPerMonth[static_cast<std::size_t>(month - 1)];
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (month == 2 && leap) {
    ++maximum_day;
  }
  if (day < 1 || day > maximum_day) {
    Invalid("reviewed_at_utc contains an invalid calendar day");
  }
}

bool ValidatePhysicalReview(const Json &review) {
  RequireExactFields(review, {"state", "reviewer", "reviewed_at_utc", "reference_setup_verified"},
                     "physical review");
  const std::string review_state = RequiredString(review, "state");
  if (review_state != "approved" && review_state != "needs_review") {
    Invalid("physical review state must be approved or needs_review");
  }
  static_cast<void>(RequiredString(review, "reviewer"));
  ValidateUtcTimestamp(RequiredString(review, "reviewed_at_utc"));
  if (!review.at("reference_setup_verified").is_boolean()) {
    Invalid("reference_setup_verified must be boolean");
  }
  return review_state == "approved" && review.at("reference_setup_verified").get<bool>();
}

std::int64_t Decimal(const Json &value, std::string_view field) {
  const Json &encoded = value.at(field);
  if (!encoded.is_string()) {
    Invalid(std::string(field) + " must be a canonical decimal string");
  }
  const std::string text = encoded.get<std::string>();
  if (text.empty() || (text.size() > 1U && text.front() == '0') ||
      (text.front() == '-' && (text.size() == 1U || text[1] == '0'))) {
    Invalid(std::string(field) + " must be a canonical decimal string");
  }
  std::int64_t parsed = 0;
  // std::from_chars uses the standard contiguous character-range pointer pair.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (error != std::errc() || end != text.data() + text.size()) {
    Invalid(std::string(field) + " must be a signed 64-bit decimal string");
  }
  return parsed;
}

std::int64_t NonnegativeDecimal(const Json &value, std::string_view field) {
  const std::int64_t parsed = Decimal(value, field);
  if (parsed < 0) {
    Invalid(std::string(field) + " cannot be negative");
  }
  return parsed;
}

std::int64_t PositiveDecimal(const Json &value, std::string_view field) {
  const std::int64_t parsed = Decimal(value, field);
  if (parsed <= 0) {
    Invalid(std::string(field) + " must be positive");
  }
  return parsed;
}

std::int64_t CheckedAdd(std::int64_t left, std::int64_t right, std::string_view label) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    Invalid(std::string(label) + " overflows signed 64-bit arithmetic");
  }
  return left + right;
}

std::int64_t CheckedSubtract(std::int64_t left, std::int64_t right, std::string_view label) {
  if (right == std::numeric_limits<std::int64_t>::min()) {
    Invalid(std::string(label) + " overflows signed 64-bit arithmetic");
  }
  return CheckedAdd(left, -right, label);
}

std::int64_t RoundedRatio(std::int64_t numerator, std::int64_t denominator) {
  if (numerator < 0 || denominator <= 0 ||
      numerator > std::numeric_limits<std::int64_t>::max() - denominator / 2) {
    Invalid("propagation ratio is outside signed 64-bit arithmetic");
  }
  return (numerator + denominator / 2) / denominator;
}

std::int64_t FloorRatio(std::int64_t numerator, std::int64_t denominator) {
  if (numerator < 0 || denominator <= 0) {
    Invalid("propagation ratio must be nonnegative with a positive denominator");
  }
  return numerator / denominator;
}

std::int64_t CeilingRatio(std::int64_t numerator, std::int64_t denominator) {
  if (numerator < 0 || denominator <= 0 ||
      numerator > std::numeric_limits<std::int64_t>::max() - denominator + 1) {
    Invalid("propagation ratio is outside signed 64-bit arithmetic");
  }
  return (numerator + denominator - 1) / denominator;
}

std::int64_t ScaledDistance(std::int64_t distance_mm) {
  if (distance_mm > std::numeric_limits<std::int64_t>::max() / kNanosecondsPerSecond) {
    Invalid("microphone distance is too large");
  }
  return distance_mm * kNanosecondsPerSecond;
}

// Schema validation is intentionally linear so every accepted physical-evidence field is audited.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Json EvaluatePairClockThreshold(const Json &evidence, const std::filesystem::path &canonical_root) {
  RequireExactFields(evidence,
                     {"schema_version", "evidence_kind", "candidate_threshold_ns",
                      "maximum_acceptable_alignment_error_ns", "reference", "review", "trials"},
                     "pair-clock root");
  if (evidence.at("schema_version") != 1 ||
      evidence.at("evidence_kind") != "pair_clock_threshold") {
    Invalid("pair-clock root version or kind is invalid");
  }
  const std::int64_t threshold = PositiveDecimal(evidence, "candidate_threshold_ns");
  const std::int64_t maximum_alignment_error =
      PositiveDecimal(evidence, "maximum_acceptable_alignment_error_ns");
  const Json &reference = evidence.at("reference");
  RequireExactFields(
      reference,
      {"instrument_id", "method", "calibration_record_path", "calibration_record_sha256"},
      "pair-clock physical reference");
  static_cast<void>(RequiredString(reference, "instrument_id"));
  if (RequiredString(reference, "method") != "simultaneous_pair_alignment_reference") {
    Invalid("pair-clock physical reference method is unsupported");
  }
  static_cast<void>(VerifyArtifact(reference, "calibration_record_path",
                                   "calibration_record_sha256", canonical_root));
  const bool provenance_review_complete = ValidatePhysicalReview(evidence.at("review"));
  const Json &trials = evidence.at("trials");
  if (!trials.is_array()) {
    Invalid("pair-clock trials must be an array");
  }

  std::set<std::string> trial_ids;
  std::set<std::string> archives;
  std::set<std::string> networks;
  std::set<std::string> assignments;
  std::map<std::string, std::set<std::string>, std::less<>> device_roles;
  std::size_t reviewed_accept = 0U;
  std::size_t reviewed_reject = 0U;
  std::size_t false_accept = 0U;
  std::size_t false_reject = 0U;
  bool has_contention = false;
  bool has_uncontended = false;
  bool has_long_screen_off = false;
  Json scored_trials = Json::array();
  for (const Json &trial : trials) {
    RequireExactFields(trial,
                       {"trial_id",
                        "archive_path",
                        "archive_sha256",
                        "physical_reference_artifact_path",
                        "physical_reference_artifact_sha256",
                        "network_label",
                        "leader_device_model",
                        "shadow_device_model",
                        "leader_role",
                        "shadow_role",
                        "screen_off_duration_seconds",
                        "network_contention",
                        "reviewed_mapping_expectation",
                        "measured_alignment_error_ns",
                        "alignment_measurement_uncertainty_ns",
                        "mapping_uncertainty_ns",
                        "mapping_age_ns",
                        "minimum_round_trip_ns",
                        "maximum_round_trip_ns",
                        "selected_candidate_residual_ns",
                        "maximum_trigger_separation_ns"},
                       "pair-clock trial");
    const std::string trial_id = RequiredString(trial, "trial_id");
    const VerifiedArtifact archive =
        VerifyArtifact(trial, "archive_path", "archive_sha256", canonical_root);
    const VerifiedArtifact physical_reference =
        VerifyArtifact(trial, "physical_reference_artifact_path",
                       "physical_reference_artifact_sha256", canonical_root);
    if (!trial_ids.insert(trial_id).second || !archives.insert(archive.relative_path).second) {
      Invalid("pair-clock trials reuse an ID or archive");
    }
    networks.insert(RequiredString(trial, "network_label"));
    const std::string leader_device = RequiredString(trial, "leader_device_model");
    const std::string shadow_device = RequiredString(trial, "shadow_device_model");
    const std::string leader_role = RequiredString(trial, "leader_role");
    const std::string shadow_role = RequiredString(trial, "shadow_role");
    if ((leader_role != "face_on" && leader_role != "down_the_line") ||
        (shadow_role != "face_on" && shadow_role != "down_the_line") ||
        leader_role == shadow_role || leader_device == shadow_device) {
      Invalid("pair-clock trial device/role assignment is invalid");
    }
    std::string assignment = leader_device;
    assignment.append(":")
        .append(leader_role)
        .append("|")
        .append(shadow_device)
        .append(":")
        .append(shadow_role);
    assignments.insert(std::move(assignment));
    device_roles[leader_device].insert(leader_role);
    device_roles[shadow_device].insert(shadow_role);
    const std::int64_t screen_off = NonnegativeDecimal(trial, "screen_off_duration_seconds");
    has_long_screen_off = has_long_screen_off || screen_off >= kMinimumLongRunningScreenOffSeconds;
    if (!trial.at("network_contention").is_boolean()) {
      Invalid("network_contention must be boolean");
    }
    const bool contention = trial.at("network_contention").get<bool>();
    has_contention = has_contention || contention;
    has_uncontended = has_uncontended || !contention;
    const std::string expected = RequiredString(trial, "reviewed_mapping_expectation");
    if (expected != "accept" && expected != "reject") {
      Invalid("reviewed_mapping_expectation must be accept or reject");
    }
    const std::int64_t measured_alignment_error =
        NonnegativeDecimal(trial, "measured_alignment_error_ns");
    const std::int64_t alignment_uncertainty =
        NonnegativeDecimal(trial, "alignment_measurement_uncertainty_ns");
    const std::int64_t alignment_upper =
        CheckedAdd(measured_alignment_error, alignment_uncertainty, "alignment error bound");
    const std::int64_t alignment_lower = measured_alignment_error > alignment_uncertainty
                                             ? measured_alignment_error - alignment_uncertainty
                                             : 0;
    std::string reference_expectation = "indeterminate";
    if (alignment_upper <= maximum_alignment_error) {
      reference_expectation = "accept";
    } else if (alignment_lower > maximum_alignment_error) {
      reference_expectation = "reject";
    }
    if (reference_expectation == "indeterminate") {
      Invalid("physical alignment interval overlaps the acceptable-error boundary");
    }
    if (expected != reference_expectation) {
      Invalid("reviewed mapping expectation contradicts physical reference bounds");
    }
    reviewed_accept += expected == "accept" ? 1U : 0U;
    reviewed_reject += expected == "reject" ? 1U : 0U;
    const std::int64_t uncertainty = NonnegativeDecimal(trial, "mapping_uncertainty_ns");
    const std::int64_t age = NonnegativeDecimal(trial, "mapping_age_ns");
    const std::int64_t minimum_round_trip = NonnegativeDecimal(trial, "minimum_round_trip_ns");
    const std::int64_t maximum_round_trip = NonnegativeDecimal(trial, "maximum_round_trip_ns");
    if (maximum_round_trip < minimum_round_trip) {
      Invalid("pair-clock round-trip bounds are reversed");
    }
    for (const std::string_view optional_field :
         {"selected_candidate_residual_ns", "maximum_trigger_separation_ns"}) {
      if (!trial.at(optional_field).is_null()) {
        static_cast<void>(NonnegativeDecimal(trial, optional_field));
      }
    }
    const bool predicted_accept = uncertainty <= threshold && age <= kMaximumMappingAgeNs;
    false_accept += predicted_accept && expected == "reject" ? 1U : 0U;
    false_reject += !predicted_accept && expected == "accept" ? 1U : 0U;
    scored_trials.push_back({
        {"trial_id", trial_id},
        {"archive_path", archive.relative_path},
        {"archive_sha256", archive.sha256},
        {"physical_reference_artifact_path", physical_reference.relative_path},
        {"physical_reference_artifact_sha256", physical_reference.sha256},
        {"predicted_accept", predicted_accept},
        {"reviewed_mapping_expectation", expected},
        {"physical_alignment_lower_bound_ns", std::to_string(alignment_lower)},
        {"physical_alignment_upper_bound_ns", std::to_string(alignment_upper)},
    });
  }
  const auto view_swapped_device_count = static_cast<std::size_t>(std::ranges::count_if(
      device_roles, [](const auto &entry) { return entry.second.size() == 2U; }));
  const bool phone_view_swap_complete = view_swapped_device_count >= 2U;
  const bool coverage_complete = trials.size() >= kMinimumPairClockTrials &&
                                 reviewed_accept >= kMinimumPairClockAcceptTrials &&
                                 reviewed_reject >= kMinimumPairClockRejectTrials &&
                                 networks.size() >= 2U && assignments.size() >= 2U &&
                                 phone_view_swap_complete && has_contention && has_uncontended &&
                                 has_long_screen_off;
  return {
      {"schema_version", 1},
      {"report_type", "pair_clock_threshold_evidence_score"},
      {"artifact_root_verified", true},
      {"candidate_threshold_ns", std::to_string(threshold)},
      {"maximum_acceptable_alignment_error_ns", std::to_string(maximum_alignment_error)},
      {"trial_count", trials.size()},
      {"reviewed_accept_count", reviewed_accept},
      {"reviewed_reject_count", reviewed_reject},
      {"false_accept_count", false_accept},
      {"false_reject_count", false_reject},
      {"network_count", networks.size()},
      {"assignment_count", assignments.size()},
      {"view_swapped_device_count", view_swapped_device_count},
      {"phone_view_swap_complete", phone_view_swap_complete},
      {"has_contended_network", has_contention},
      {"has_uncontended_network", has_uncontended},
      {"has_long_running_screen_off", has_long_screen_off},
      {"provenance_review_complete", provenance_review_complete},
      {"coverage_complete", coverage_complete},
      {"threshold_selection_eligible",
       provenance_review_complete && coverage_complete && false_accept == 0U && false_reject == 0U},
      {"physical_truth_inferred", false},
      {"trials", std::move(scored_trials)},
  };
}

struct DeviceLatencySummary {
  std::vector<std::int64_t> latencies;
  std::int64_t maximum_uncertainty_ns = 0;
  std::int64_t minimum_distance_mm = std::numeric_limits<std::int64_t>::max();
  std::int64_t maximum_distance_mm = 0;
  std::set<std::string> roles;
};

// Schema validation is intentionally linear so every accepted physical-evidence field is audited.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Json EvaluateAbsoluteAudioLatency(const Json &evidence,
                                  const std::filesystem::path &canonical_root) {
  RequireExactFields(evidence,
                     {"schema_version", "evidence_kind", "calibration_id", "scope", "reference",
                      "review", "trials"},
                     "audio-latency root");
  if (evidence.at("schema_version") != 1 ||
      evidence.at("evidence_kind") != "absolute_audio_latency") {
    Invalid("audio-latency root version or kind is invalid");
  }
  const std::string calibration_id = RequiredString(evidence, "calibration_id");
  const std::string scope = RequiredString(evidence, "scope");
  if (scope != "source_acoustic_emission" && scope != "instrumented_ball_contact") {
    Invalid("audio-latency scope is unsupported");
  }
  const Json &reference = evidence.at("reference");
  RequireExactFields(
      reference,
      {"instrument_id", "method", "calibration_record_path", "calibration_record_sha256",
       "raw_reference_artifact_path", "raw_reference_artifact_sha256"},
      "physical reference");
  static_cast<void>(RequiredString(reference, "instrument_id"));
  const std::string reference_method = RequiredString(reference, "method");
  static_cast<void>(VerifyArtifact(reference, "calibration_record_path",
                                   "calibration_record_sha256", canonical_root));
  static_cast<void>(VerifyArtifact(reference, "raw_reference_artifact_path",
                                   "raw_reference_artifact_sha256", canonical_root));
  const bool reference_method_matches_scope =
      (scope == "source_acoustic_emission" &&
       reference_method == "physical_acoustic_emission_sensor") ||
      (scope == "instrumented_ball_contact" &&
       reference_method == "instrumented_ball_contact_sensor");
  if (!reference_method_matches_scope) {
    Invalid("physical reference method does not match calibration scope");
  }
  const bool provenance_review_complete = ValidatePhysicalReview(evidence.at("review"));

  const Json &trials = evidence.at("trials");
  if (!trials.is_array()) {
    Invalid("audio-latency trials must be an array");
  }
  std::set<std::string> trial_ids;
  std::set<std::string> artifacts;
  std::map<std::string, DeviceLatencySummary, std::less<>> devices;
  Json scored_trials = Json::array();
  for (const Json &trial : trials) {
    RequireExactFields(
        trial,
        {"trial_id", "raw_trial_artifact_path", "raw_trial_artifact_sha256", "node_id",
         "device_model", "role", "microphone_distance_mm", "distance_uncertainty_mm",
         "speed_of_sound_mm_per_second", "speed_of_sound_uncertainty_mm_per_second",
         "reference_event_ns", "reference_uncertainty_ns", "phone_audio_event_ns",
         "phone_audio_uncertainty_ns", "phone_minus_reference_clock_offset_ns",
         "clock_mapping_uncertainty_ns"},
        "audio-latency trial");
    const std::string trial_id = RequiredString(trial, "trial_id");
    const VerifiedArtifact artifact = VerifyArtifact(trial, "raw_trial_artifact_path",
                                                     "raw_trial_artifact_sha256", canonical_root);
    if (!trial_ids.insert(trial_id).second || !artifacts.insert(artifact.relative_path).second) {
      Invalid("audio-latency trials reuse an ID or raw artifact");
    }
    static_cast<void>(RequiredString(trial, "node_id"));
    const std::string device = RequiredString(trial, "device_model");
    const std::string role = RequiredString(trial, "role");
    if (role != "face_on" && role != "down_the_line") {
      Invalid("audio-latency role is invalid");
    }
    const std::int64_t distance = PositiveDecimal(trial, "microphone_distance_mm");
    const std::int64_t distance_uncertainty = NonnegativeDecimal(trial, "distance_uncertainty_mm");
    if (distance_uncertainty >= distance) {
      Invalid("distance uncertainty must be smaller than microphone distance");
    }
    const std::int64_t speed = PositiveDecimal(trial, "speed_of_sound_mm_per_second");
    const std::int64_t speed_uncertainty =
        NonnegativeDecimal(trial, "speed_of_sound_uncertainty_mm_per_second");
    if (speed_uncertainty >= speed) {
      Invalid("speed-of-sound uncertainty must be smaller than speed");
    }
    const std::int64_t propagation = RoundedRatio(ScaledDistance(distance), speed);
    const std::int64_t minimum_propagation =
        FloorRatio(ScaledDistance(distance - distance_uncertainty),
                   CheckedAdd(speed, speed_uncertainty, "speed of sound"));
    const std::int64_t maximum_propagation =
        CeilingRatio(ScaledDistance(CheckedAdd(distance, distance_uncertainty, "distance")),
                     speed - speed_uncertainty);
    const std::int64_t propagation_uncertainty =
        std::max(propagation - minimum_propagation, maximum_propagation - propagation);
    const std::int64_t reference_event = PositiveDecimal(trial, "reference_event_ns");
    const std::int64_t phone_event = PositiveDecimal(trial, "phone_audio_event_ns");
    const std::int64_t clock_offset = Decimal(trial, "phone_minus_reference_clock_offset_ns");
    const std::int64_t mapped_phone_event =
        CheckedSubtract(phone_event, clock_offset, "mapped phone event");
    const std::int64_t observed_delay =
        CheckedSubtract(mapped_phone_event, reference_event, "observed acoustic delay");
    const std::int64_t device_latency =
        CheckedSubtract(observed_delay, propagation, "device audio latency");
    if (device_latency < 0) {
      Invalid("derived device audio latency cannot be negative");
    }
    std::int64_t total_uncertainty = NonnegativeDecimal(trial, "reference_uncertainty_ns");
    total_uncertainty =
        CheckedAdd(total_uncertainty, NonnegativeDecimal(trial, "phone_audio_uncertainty_ns"),
                   "latency uncertainty");
    total_uncertainty =
        CheckedAdd(total_uncertainty, NonnegativeDecimal(trial, "clock_mapping_uncertainty_ns"),
                   "latency uncertainty");
    total_uncertainty =
        CheckedAdd(total_uncertainty, propagation_uncertainty, "latency uncertainty");
    DeviceLatencySummary &summary = devices[device];
    summary.latencies.push_back(device_latency);
    summary.maximum_uncertainty_ns = std::max(summary.maximum_uncertainty_ns, total_uncertainty);
    summary.minimum_distance_mm = std::min(summary.minimum_distance_mm, distance);
    summary.maximum_distance_mm = std::max(summary.maximum_distance_mm, distance);
    summary.roles.insert(role);
    scored_trials.push_back({
        {"trial_id", trial_id},
        {"raw_trial_artifact_path", artifact.relative_path},
        {"raw_trial_artifact_sha256", artifact.sha256},
        {"propagation_delay_ns", std::to_string(propagation)},
        {"propagation_uncertainty_ns", std::to_string(propagation_uncertainty)},
        {"mapped_phone_audio_event_ns", std::to_string(mapped_phone_event)},
        {"derived_device_latency_ns", std::to_string(device_latency)},
        {"derived_total_uncertainty_ns", std::to_string(total_uncertainty)},
    });
  }

  bool coverage_complete = !devices.empty();
  Json device_reports = Json::array();
  for (auto &[device, summary] : devices) {
    std::ranges::sort(summary.latencies);
    const std::int64_t median = summary.latencies[summary.latencies.size() / 2U];
    const bool device_coverage =
        summary.latencies.size() >= kMinimumLatencyTrialsPerDevice && summary.roles.size() == 2U &&
        summary.maximum_distance_mm - summary.minimum_distance_mm >= kMinimumDistanceSpanMm;
    coverage_complete = coverage_complete && device_coverage;
    device_reports.push_back({
        {"device_model", device},
        {"trial_count", summary.latencies.size()},
        {"role_count", summary.roles.size()},
        {"distance_span_mm",
         std::to_string(summary.maximum_distance_mm - summary.minimum_distance_mm)},
        {"minimum_derived_latency_ns", std::to_string(summary.latencies.front())},
        {"median_derived_latency_ns", std::to_string(median)},
        {"maximum_derived_latency_ns", std::to_string(summary.latencies.back())},
        {"maximum_derived_uncertainty_ns", std::to_string(summary.maximum_uncertainty_ns)},
        {"coverage_complete", device_coverage},
    });
  }
  const bool ball_contact_scope = scope == "instrumented_ball_contact";
  return {
      {"schema_version", 1},
      {"report_type", "absolute_audio_latency_evidence_score"},
      {"artifact_root_verified", true},
      {"calibration_id", calibration_id},
      {"scope", scope},
      {"trial_count", trials.size()},
      {"provenance_review_complete", provenance_review_complete},
      {"coverage_complete", coverage_complete},
      {"component_calibration_eligible", provenance_review_complete && coverage_complete},
      {"absolute_ball_impact_claim_eligible",
       ball_contact_scope && provenance_review_complete && coverage_complete},
      {"physical_truth_inferred", false},
      {"device_summaries", std::move(device_reports)},
      {"trials", std::move(scored_trials)},
  };
}

}  // namespace

Json EvaluateTimingCalibrationEvidence(std::string_view evidence_json,
                                       const std::filesystem::path &artifact_root) {
  std::error_code error;
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(artifact_root, error);
  if (error || std::filesystem::is_symlink(root_status) ||
      !std::filesystem::is_directory(root_status)) {
    Invalid("artifact root must be an existing non-symlink directory");
  }
  const std::filesystem::path canonical_root = std::filesystem::canonical(artifact_root, error);
  if (error || canonical_root.empty()) {
    Invalid("artifact root cannot be resolved");
  }
  Json evidence;
  try {
    evidence = Json::parse(evidence_json);
  } catch (const Json::exception &failure) {
    Invalid(std::string("cannot parse JSON: ") + failure.what());
  }
  const std::string kind = RequiredString(evidence, "evidence_kind");
  if (kind == "pair_clock_threshold") {
    return EvaluatePairClockThreshold(evidence, canonical_root);
  }
  if (kind == "absolute_audio_latency") {
    return EvaluateAbsoluteAudioLatency(evidence, canonical_root);
  }
  Invalid("evidence_kind is unsupported");
}

}  // namespace swing_capture::android::dual_hil
