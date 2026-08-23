#include "web/android_field_recording_browser_hil_report_validation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "web/android_browser_hil_credentials.h"

namespace swing_capture::web {
namespace {

using Json = nlohmann::json;  // NOLINT(misc-include-cleaner)

void RequireExactKeys(const Json &object, std::initializer_list<std::string_view> expected,
                      std::string_view context) {
  if (!object.is_object() || object.size() != expected.size()) {
    throw std::runtime_error(std::string(context) + " has an incomplete or extended schema");
  }
  for (const std::string_view key : expected) {
    if (!object.contains(key)) {
      throw std::runtime_error(std::string(context) + " is missing " + std::string(key));
    }
  }
}

bool IsSha256(std::string_view value) {
  return value.size() == 64U && std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
         });
}

std::string RequiredString(const Json &object, std::string_view key, std::string_view context) {
  const auto field = object.find(key);
  if (field == object.end() || !field->is_string()) {
    throw std::runtime_error(std::string(context) + " has an invalid " + std::string(key));
  }
  return field->get<std::string>();
}

bool RequiredBoolean(const Json &object, std::string_view key, std::string_view context) {
  const auto field = object.find(key);
  if (field == object.end() || !field->is_boolean()) {
    throw std::runtime_error(std::string(context) + " has an invalid " + std::string(key));
  }
  return field->get<bool>();
}

std::int64_t RequiredInteger(const Json &object, std::string_view key, std::string_view context) {
  const auto field = object.find(key);
  if (field == object.end() || !field->is_number_integer()) {
    throw std::runtime_error(std::string(context) + " has an invalid " + std::string(key));
  }
  return field->get<std::int64_t>();
}

double RequiredFiniteNumber(const Json &object, std::string_view key, std::string_view context) {
  const auto field = object.find(key);
  if (field == object.end() || !field->is_number()) {
    throw std::runtime_error(std::string(context) + " has an invalid " + std::string(key));
  }
  const double value = field->get<double>();
  if (!std::isfinite(value)) {
    throw std::runtime_error(std::string(context) + " has a non-finite " + std::string(key));
  }
  return value;
}

void RequireEmptyStringArray(const Json &value, std::string_view context) {
  if (!value.is_array() || !value.empty()) {
    throw std::runtime_error(std::string(context) + " must be an empty array");
  }
}

bool HasOnlyStatus(const Json &statuses, std::int64_t expected) {
  return statuses.is_array() && !statuses.empty() &&
         std::ranges::all_of(statuses, [expected](const Json &status) {
           return status.is_number_integer() && status.get<std::int64_t>() == expected;
         });
}

// The JSON values represent distinct browser event and HTTP status collections.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void RequireBoundedReviewMediaAborts(const Json &items, const Json &api_statuses,
                                     const std::array<std::string_view, 2> &origins) {
  constexpr std::size_t kMaximumExpectedAborts = 8U;
  constexpr std::string_view kSessionPrefix = "/api/v1/sessions/";
  if (!items.is_array() || items.size() > kMaximumExpectedAborts) {
    throw std::runtime_error("Android browser HIL historical media abort evidence is invalid");
  }
  for (const Json &item : items) {
    RequireExactKeys(item, {"origin", "path", "error"},
                     "Android browser HIL historical media abort evidence");
    const std::string origin = RequiredString(item, "origin", "historical media abort");
    const std::string path = RequiredString(item, "path", "historical media abort");
    const std::string error = RequiredString(item, "error", "historical media abort");
    const bool expected_origin = std::ranges::contains(origins, origin);
    const bool expected_suffix =
        path.ends_with("/down_the_line.mp4") || path.ends_with("/face_on.mp4");
    if (!expected_origin || !path.starts_with(kSessionPrefix) || !expected_suffix ||
        error != "net::ERR_ABORTED") {
      throw std::runtime_error("Android browser HIL classified an unexpected request failure");
    }
    std::string api_key = "GET ";
    api_key.append(origin).append(path);
    const auto statuses = api_statuses.find(api_key);
    if (statuses == api_statuses.end() || !HasOnlyStatus(*statuses, 206)) {
      throw std::runtime_error(
          "Android browser HIL media cancellation lacks a successful partial response");
    }
  }
}

// The JSON values represent distinct browser event and HTTP status collections.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void RequireBoundedFreshDecodeMediaAborts(
    const Json &items,  // NOLINT(bugprone-easily-swappable-parameters)
    const Json &api_statuses,
    const std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> &decoded) {
  constexpr std::size_t kMaximumExpectedAborts = 8U;
  if (!items.is_array() || items.size() > kMaximumExpectedAborts) {
    throw std::runtime_error("Android browser HIL fresh decoder abort evidence is invalid");
  }
  for (const Json &item : items) {
    RequireExactKeys(item, {"role", "origin", "media_identity", "error"},
                     "Android browser HIL fresh decoder abort evidence");
    const std::string role = RequiredString(item, "role", "fresh decoder abort");
    const std::string origin = RequiredString(item, "origin", "fresh decoder abort");
    const std::string identity = RequiredString(item, "media_identity", "fresh decoder abort");
    const std::string error = RequiredString(item, "error", "fresh decoder abort");
    const bool matches_decoded = std::ranges::any_of(decoded, [&](const auto &evidence) {
      return evidence.role == role && evidence.origin == origin &&
             evidence.media_identity == identity;
    });
    if (!matches_decoded || error != "net::ERR_ABORTED") {
      throw std::runtime_error("Android browser HIL classified an unexpected decoder failure");
    }
    const auto statuses = api_statuses.find("GET " + identity);
    if (statuses == api_statuses.end() || !HasOnlyStatus(*statuses, 206)) {
      throw std::runtime_error(
          "Android browser HIL decoder cancellation lacks a successful partial response");
    }
  }
}

void RequireOriginArrayEquals(const Json &origins, const std::array<std::string_view, 2> &expected,
                              bool ordered, std::string_view context) {
  if (!origins.is_array() || origins.size() != expected.size() ||
      !std::ranges::all_of(origins, [](const Json &origin) { return origin.is_string(); })) {
    throw std::runtime_error(std::string(context) + " does not contain exactly two origins");
  }
  if (ordered) {
    if (origins[0].get_ref<const std::string &>() != expected[0] ||
        origins[1].get_ref<const std::string &>() != expected[1]) {
      throw std::runtime_error(std::string(context) + " does not match configured origin order");
    }
    return;
  }
  std::set<std::string, std::less<>> actual;
  for (const Json &origin : origins) {
    actual.insert(origin.get<std::string>());
  }
  if (actual.size() != expected.size() || !actual.contains(expected[0]) ||
      !actual.contains(expected[1])) {
    throw std::runtime_error(std::string(context) + " does not match configured origins");
  }
}

// This is intentionally a single schema-policy validator so every accepted attempt is checked.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ValidateLauncherHttpReadiness(const Json &readiness,
                                   const std::array<std::string_view, 2> &expected_origins) {
  constexpr std::int64_t kMaximumPerPhoneTimeoutMs = 3'000;
  constexpr std::size_t kMaximumAttemptsPerPhone = 64U;
  RequireExactKeys(readiness, {"passed", "probe_method", "probe_path", "nodes"},
                   "Android browser HIL HTTP readiness evidence");
  const Json &nodes = readiness.at("nodes");
  if (!RequiredBoolean(readiness, "passed", "HTTP readiness evidence") ||
      RequiredString(readiness, "probe_method", "HTTP readiness evidence") != "GET" ||
      RequiredString(readiness, "probe_path", "HTTP readiness evidence") != "/api/v1/node" ||
      !nodes.is_array() || nodes.size() != 2U) {
    throw std::runtime_error("Android browser HIL HTTP readiness did not cover both phones");
  }

  std::set<std::string, std::less<>> roles;
  for (const Json &node : nodes) {
    RequireExactKeys(node, {"role", "origin", "timeout_ms", "passed", "attempts"},
                     "Android browser HIL phone HTTP readiness evidence");
    const std::string role = RequiredString(node, "role", "phone HTTP readiness evidence");
    const std::string origin = RequiredString(node, "origin", "phone HTTP readiness evidence");
    const std::int64_t timeout_ms =
        RequiredInteger(node, "timeout_ms", "phone HTTP readiness evidence");
    const bool expected_role_origin = (role == "down_the_line" && origin == expected_origins[0]) ||
                                      (role == "face_on" && origin == expected_origins[1]);
    const Json &attempts = node.at("attempts");
    if (!expected_role_origin || !roles.insert(role).second || timeout_ms <= 0 ||
        timeout_ms > kMaximumPerPhoneTimeoutMs ||
        !RequiredBoolean(node, "passed", "phone HTTP readiness evidence") || !attempts.is_array() ||
        attempts.empty() || attempts.size() > kMaximumAttemptsPerPhone) {
      throw std::runtime_error("Android browser HIL phone HTTP readiness is invalid");
    }

    std::int64_t previous_elapsed_ms = -1;
    for (std::size_t index = 0U; index < attempts.size(); ++index) {
      const Json &attempt = attempts[index];
      RequireExactKeys(attempt, {"sequence", "elapsed_ms", "http_status", "outcome"},
                       "Android browser HIL HTTP readiness attempt");
      const std::int64_t sequence = RequiredInteger(attempt, "sequence", "HTTP readiness attempt");
      const std::int64_t elapsed_ms =
          RequiredInteger(attempt, "elapsed_ms", "HTTP readiness attempt");
      const std::string outcome = RequiredString(attempt, "outcome", "HTTP readiness attempt");
      if (!std::cmp_equal(sequence, index + 1U) || elapsed_ms < 0 ||
          elapsed_ms < previous_elapsed_ms || elapsed_ms > timeout_ms) {
        throw std::runtime_error("Android browser HIL HTTP readiness attempt timing is invalid");
      }
      previous_elapsed_ms = elapsed_ms;
      const bool final_attempt = index + 1U == attempts.size();
      if (outcome == "transport_error") {
        if (!attempt.at("http_status").is_null() || final_attempt) {
          throw std::runtime_error(
              "Android browser HIL transport readiness evidence is inconsistent");
        }
      } else if (outcome == "retryable_http_status") {
        if (!attempt.at("http_status").is_number_integer() ||
            attempt.at("http_status").get<int>() < 500 ||
            attempt.at("http_status").get<int>() > 599 || final_attempt) {
          throw std::runtime_error(
              "Android browser HIL retryable readiness evidence is inconsistent");
        }
      } else if (outcome == "ready") {
        if (!final_attempt || !attempt.at("http_status").is_number_integer() ||
            attempt.at("http_status").get<int>() != 200) {
          throw std::runtime_error("Android browser HIL ready evidence is inconsistent");
        }
      } else {
        throw std::runtime_error("Android browser HIL readiness outcome is invalid");
      }
    }
  }
}

AndroidFieldRecordingBrowserHilBrowserEvidence ValidateBrowserEvidence(const Json &browser) {
  RequireExactKeys(browser,
                   {"engine", "distribution", "version", "user_agent", "h264_decode_required"},
                   "Android browser HIL selected browser evidence");
  AndroidFieldRecordingBrowserHilBrowserEvidence evidence = {
      .engine = RequiredString(browser, "engine", "selected browser evidence"),
      .distribution = RequiredString(browser, "distribution", "selected browser evidence"),
      .version = RequiredString(browser, "version", "selected browser evidence"),
      .user_agent = RequiredString(browser, "user_agent", "selected browser evidence"),
      .h264_decode_required =
          RequiredBoolean(browser, "h264_decode_required", "selected browser evidence"),
  };
  const bool chrome_user_agent =
      evidence.user_agent.contains("Chrome/") || evidence.user_agent.contains("Chromium/");
  if (evidence.engine != "chromium" || evidence.distribution != "google_chrome" ||
      evidence.version.empty() || !chrome_user_agent || !evidence.h264_decode_required) {
    throw std::runtime_error("Android browser HIL did not select installed Google Chrome");
  }
  return evidence;
}

std::string ValidateMediaIdentity(std::string_view identity, std::string_view origin) {
  constexpr std::string_view prefix = "/api/v1/field-recordings/";
  constexpr std::string_view suffix = "/video.mp4";
  if (!identity.starts_with(origin)) {
    throw std::runtime_error("Android browser HIL fresh media identity has the wrong origin");
  }
  const std::string_view path = identity.substr(origin.size());
  if (!path.starts_with(prefix) || !path.ends_with(suffix) ||
      path.size() <= prefix.size() + suffix.size()) {
    throw std::runtime_error("Android browser HIL fresh media identity is not a phone MP4");
  }
  const std::string_view recording_id =
      path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (!std::ranges::all_of(recording_id, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
               character == '.';
      })) {
    throw std::runtime_error("Android browser HIL fresh media identity is not canonical");
  }
  return std::string(path);
}

std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence>
ValidateFreshMediaDecodeEvidence(const Json &items,
                                 const std::array<std::string_view, 2> &origins) {
  if (!items.is_array() || items.size() != 2U) {
    throw std::runtime_error(
        "Android browser HIL requires exactly two fresh phone MP4 decode entries");
  }
  std::set<std::string, std::less<>> roles;
  std::set<std::string, std::less<>> decoded_origins;
  std::string common_path;
  std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> result;
  result.reserve(2U);
  for (const Json &item : items) {
    RequireExactKeys(
        item,
        {"role", "origin", "media_identity", "duration_seconds", "video_width", "video_height",
         "rvfc_media_time_seconds", "rvfc_presented_frames", "nonblack_fraction",
         "playback_start_seconds", "playback_end_seconds", "playback_advanced_seconds"},
        "Android browser HIL fresh media decode evidence");
    AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence evidence = {
        .role = RequiredString(item, "role", "fresh media decode evidence"),
        .origin = RequiredString(item, "origin", "fresh media decode evidence"),
        .media_identity = RequiredString(item, "media_identity", "fresh media decode evidence"),
        .duration_seconds =
            RequiredFiniteNumber(item, "duration_seconds", "fresh media decode evidence"),
        .video_width = RequiredInteger(item, "video_width", "fresh media decode evidence"),
        .video_height = RequiredInteger(item, "video_height", "fresh media decode evidence"),
        .rvfc_media_time_seconds =
            RequiredFiniteNumber(item, "rvfc_media_time_seconds", "fresh media decode evidence"),
        .rvfc_presented_frames =
            RequiredInteger(item, "rvfc_presented_frames", "fresh media decode evidence"),
        .nonblack_fraction =
            RequiredFiniteNumber(item, "nonblack_fraction", "fresh media decode evidence"),
        .playback_start_seconds =
            RequiredFiniteNumber(item, "playback_start_seconds", "fresh media decode evidence"),
        .playback_end_seconds =
            RequiredFiniteNumber(item, "playback_end_seconds", "fresh media decode evidence"),
        .playback_advanced_seconds =
            RequiredFiniteNumber(item, "playback_advanced_seconds", "fresh media decode evidence"),
    };
    const bool down_the_line = evidence.role == "down_the_line";
    const bool face_on = evidence.role == "face_on";
    const std::string_view expected_origin = down_the_line ? origins[0] : origins[1];
    if ((!down_the_line && !face_on) || evidence.origin != expected_origin ||
        !roles.insert(evidence.role).second || !decoded_origins.insert(evidence.origin).second) {
      throw std::runtime_error(
          "Android browser HIL fresh media roles and origins are not complementary");
    }
    ValidateDirectLanNodeOrigin(evidence.origin);
    const std::string path = ValidateMediaIdentity(evidence.media_identity, evidence.origin);
    if (!common_path.empty() && path != common_path) {
      throw std::runtime_error(
          "Android browser HIL fresh media does not share one recording identity");
    }
    common_path = path;

    constexpr double kTimeToleranceSeconds = 0.01;
    const double measured_advance = evidence.playback_end_seconds - evidence.playback_start_seconds;
    if (evidence.duration_seconds <= 0.0 || evidence.video_width <= 0 ||
        evidence.video_height <= 0 || evidence.rvfc_media_time_seconds < 0.0 ||
        evidence.rvfc_media_time_seconds > evidence.duration_seconds + kTimeToleranceSeconds ||
        evidence.rvfc_presented_frames < 1 || evidence.nonblack_fraction <= 0.01 ||
        evidence.nonblack_fraction > 1.0 || evidence.playback_start_seconds < 0.0 ||
        evidence.playback_end_seconds <= evidence.playback_start_seconds ||
        evidence.playback_end_seconds > evidence.duration_seconds + kTimeToleranceSeconds ||
        evidence.playback_advanced_seconds < 0.1 ||
        std::abs(evidence.playback_advanced_seconds - measured_advance) > kTimeToleranceSeconds) {
      throw std::runtime_error(
          "Android browser HIL fresh phone MP4 did not decode and advance visibly");
    }
    result.push_back(std::move(evidence));
  }
  return result;
}

// Report text and its diagnostic label are intentionally adjacent string views.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Json ParseReport(std::string_view report_json, std::string_view context) {
  try {
    return Json::parse(report_json);
  } catch (const Json::exception &) {
    throw std::runtime_error(std::string(context) + " is not valid JSON");
  }
}

}  // namespace

AndroidFieldRecordingBrowserHilChildReportEvidence
// This function validates one deliberately closed, cross-layer evidence schema.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters)
ValidateAndroidFieldRecordingBrowserHilChildReport(std::string_view report_json,
                                                   std::string_view down_the_line_origin,
                                                   std::string_view face_on_origin) {
  const Json report = ParseReport(report_json, "Android browser HIL child report");
  RequireExactKeys(report,
                   {"schema_version",
                    "report_type",
                    "passed",
                    "error",
                    "credential_bootstrap",
                    "credential_parameters_absent",
                    "application_origin",
                    "node_origins",
                    "phone_hosted_assets",
                    "browser",
                    "api_statuses",
                    "camera_stage_elapsed_ms",
                    "camera_stage_limit_ms",
                    "startup_convergence_timeout_ms",
                    "page_ready_elapsed_ms",
                    "start_convergence_elapsed_ms",
                    "stop_convergence_elapsed_ms",
                    "post_camera_validation_elapsed_ms",
                    "cleanup_elapsed_ms",
                    "primary_stop_evidence",
                    "watchdog_fired",
                    "completed_bundle_origins",
                    "range_retrievals",
                    "fresh_media_decode",
                    "fresh_decode_media_aborts",
                    "review_media_aborts",
                    "accessibility_violations",
                    "page_errors",
                    "failed_requests",
                    "screenshot",
                    "cleanup",
                    "completed_bundles_retained"},
                   "Android browser HIL child report");
  const std::array<std::string_view, 2> expected_origins = {down_the_line_origin, face_on_origin};
  if (down_the_line_origin == face_on_origin) {
    throw std::runtime_error("Android browser HIL child report requires distinct origins");
  }
  ValidateDirectLanNodeOrigin(down_the_line_origin);
  ValidateDirectLanNodeOrigin(face_on_origin);
  if (RequiredInteger(report, "schema_version", "child report") != 1 ||
      RequiredString(report, "report_type", "child report") !=
          "android_field_recording_browser_hil" ||
      !RequiredBoolean(report, "passed", "child report") ||
      !RequiredString(report, "error", "child report").empty() ||
      RequiredString(report, "credential_bootstrap", "child report") !=
          "tab_scoped_session_storage_from_adb_injected_environment" ||
      !RequiredBoolean(report, "credential_parameters_absent", "child report") ||
      RequiredString(report, "application_origin", "child report") != down_the_line_origin ||
      RequiredBoolean(report, "watchdog_fired", "child report") ||
      RequiredString(report, "screenshot", "child report") !=
          "android-field-recording-browser-hil.png" ||
      !RequiredBoolean(report, "completed_bundles_retained", "child report")) {
    throw std::runtime_error("Android browser HIL child top-level policy did not pass");
  }
  RequireOriginArrayEquals(report.at("node_origins"), expected_origins, true,
                           "Android browser HIL child node origins");

  const Json &assets = report.at("phone_hosted_assets");
  RequireExactKeys(assets, {"/app.css", "/app.js"}, "Android browser HIL phone-hosted assets");
  if (RequiredInteger(assets, "/app.css", "phone-hosted assets") != 200 ||
      RequiredInteger(assets, "/app.js", "phone-hosted assets") != 200) {
    throw std::runtime_error("Android browser HIL did not load both phone-hosted assets");
  }

  const Json &api_statuses = report.at("api_statuses");
  if (!api_statuses.is_object() || api_statuses.empty()) {
    throw std::runtime_error("Android browser HIL child API status evidence is missing");
  }
  for (const auto &[request, statuses] : api_statuses.items()) {
    if (request.empty() || !statuses.is_array() || statuses.empty() ||
        !std::ranges::all_of(statuses, [](const Json &status) {
          return status.is_number_integer() && status.get<std::int64_t>() >= 200 &&
                 status.get<std::int64_t>() < 300;
        })) {
      throw std::runtime_error("Android browser HIL child API status evidence is invalid");
    }
  }
  for (const std::string_view origin : expected_origins) {
    for (const std::string_view operation : {"start", "stop"}) {
      const std::string key =
          "POST " + std::string(origin) + "/api/v1/field-recording/" + std::string(operation);
      const auto statuses = api_statuses.find(key);
      if (statuses == api_statuses.end() || !HasOnlyStatus(*statuses, 202)) {
        throw std::runtime_error(
            "Android browser HIL child did not retain both accepted recording mutations");
      }
    }
  }
  RequireBoundedReviewMediaAborts(report.at("review_media_aborts"), api_statuses, expected_origins);

  const std::int64_t camera_elapsed =
      RequiredInteger(report, "camera_stage_elapsed_ms", "child timing evidence");
  const std::int64_t camera_limit =
      RequiredInteger(report, "camera_stage_limit_ms", "child timing evidence");
  const std::int64_t startup_limit =
      RequiredInteger(report, "startup_convergence_timeout_ms", "child timing evidence");
  const std::int64_t page_ready =
      RequiredInteger(report, "page_ready_elapsed_ms", "child timing evidence");
  const std::int64_t start_elapsed =
      RequiredInteger(report, "start_convergence_elapsed_ms", "child timing evidence");
  const std::int64_t stop_elapsed =
      RequiredInteger(report, "stop_convergence_elapsed_ms", "child timing evidence");
  const std::int64_t validation_elapsed =
      RequiredInteger(report, "post_camera_validation_elapsed_ms", "child timing evidence");
  const std::int64_t cleanup_elapsed =
      RequiredInteger(report, "cleanup_elapsed_ms", "child timing evidence");
  if (camera_limit != 15'000 || startup_limit != 6'000 || camera_elapsed < 0 ||
      camera_elapsed >= camera_limit || page_ready < 0 || start_elapsed < 0 ||
      start_elapsed > startup_limit || stop_elapsed < 0 || stop_elapsed > 8'000 ||
      validation_elapsed < 0 || cleanup_elapsed < 0) {
    throw std::runtime_error("Android browser HIL child timing policy did not pass");
  }

  const Json &primary_stop = report.at("primary_stop_evidence");
  if (!primary_stop.is_array() || primary_stop.size() != 2U) {
    throw std::runtime_error("Android browser HIL child primary stop evidence is incomplete");
  }
  std::set<std::string, std::less<>> primary_origins;
  for (const Json &item : primary_stop) {
    RequireExactKeys(item, {"origin", "statuses"},
                     "Android browser HIL child primary stop evidence");
    const std::string origin = RequiredString(item, "origin", "primary stop evidence");
    if ((!origin.starts_with(down_the_line_origin) && !origin.starts_with(face_on_origin)) ||
        !primary_origins.insert(origin).second || !HasOnlyStatus(item.at("statuses"), 202)) {
      throw std::runtime_error("Android browser HIL child primary stop evidence is invalid");
    }
  }
  if (!primary_origins.contains(down_the_line_origin) ||
      !primary_origins.contains(face_on_origin)) {
    throw std::runtime_error("Android browser HIL child primary stop origins are incomplete");
  }

  RequireOriginArrayEquals(report.at("completed_bundle_origins"), expected_origins, false,
                           "Android browser HIL completed bundle origins");
  const Json &ranges = report.at("range_retrievals");
  if (!ranges.is_array() || ranges.size() != 4U) {
    throw std::runtime_error("Android browser HIL child Range evidence is incomplete");
  }
  std::set<std::pair<std::string, std::string>> range_keys;
  for (const Json &item : ranges) {
    RequireExactKeys(item, {"origin", "kind", "status", "bytes"},
                     "Android browser HIL child Range evidence");
    const std::string origin = RequiredString(item, "origin", "Range evidence");
    const std::string kind = RequiredString(item, "kind", "Range evidence");
    if ((!std::ranges::contains(expected_origins, origin)) ||
        (kind != "video" && kind != "audio") || !range_keys.emplace(origin, kind).second ||
        RequiredInteger(item, "status", "Range evidence") != 206 ||
        RequiredInteger(item, "bytes", "Range evidence") <= 0) {
      throw std::runtime_error("Android browser HIL child Range evidence is invalid");
    }
  }

  RequireEmptyStringArray(report.at("accessibility_violations"),
                          "Android browser HIL accessibility violations");
  RequireEmptyStringArray(report.at("page_errors"), "Android browser HIL page errors");
  RequireEmptyStringArray(report.at("failed_requests"), "Android browser HIL failed requests");

  const Json &cleanup = report.at("cleanup");
  if (!cleanup.is_array() || cleanup.size() != 2U) {
    throw std::runtime_error("Android browser HIL child cleanup evidence is incomplete");
  }
  std::set<std::string, std::less<>> cleanup_origins;
  for (const Json &item : cleanup) {
    RequireExactKeys(item, {"origin", "stop_status", "final_state"},
                     "Android browser HIL child cleanup evidence");
    const std::string origin = RequiredString(item, "origin", "child cleanup evidence");
    const std::string final_state = RequiredString(item, "final_state", "child cleanup evidence");
    const Json &stop_status = item.at("stop_status");
    const bool valid_stop_status =
        stop_status.is_null() ||
        (stop_status.is_number_integer() && stop_status.get<std::int64_t>() >= 200 &&
         stop_status.get<std::int64_t>() < 300);
    if (!std::ranges::contains(expected_origins, origin) ||
        !cleanup_origins.insert(origin).second ||
        (final_state != "ready" && final_state != "idle") || !valid_stop_status) {
      throw std::runtime_error("Android browser HIL child cleanup evidence is invalid");
    }
  }

  std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> fresh_media_decode =
      ValidateFreshMediaDecodeEvidence(report.at("fresh_media_decode"), expected_origins);
  RequireBoundedFreshDecodeMediaAborts(report.at("fresh_decode_media_aborts"), api_statuses,
                                       fresh_media_decode);
  return {.browser = ValidateBrowserEvidence(report.at("browser")),
          .fresh_media_decode = std::move(fresh_media_decode)};
}

// This function validates one deliberately closed, cross-layer evidence schema.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
AndroidFieldRecordingBrowserHilReportEvidence ValidateAndroidFieldRecordingBrowserHilLauncherReport(
    std::string_view report_json) {
  const Json report = ParseReport(report_json, "Android browser HIL launcher report");
  RequireExactKeys(
      report,
      {"schema_version", "report_type", "passed", "credential_transport", "credentials_retained",
       "origins", "exact_bazel_apk_verified", "apk", "primary", "phone_preparation_elapsed_ms",
       "http_readiness", "cleanup", "browser", "fresh_media_decode", "evidence_contract_validated"},
      "Android browser HIL launcher report");
  if (!report.at("schema_version").is_number_integer() ||
      report.at("schema_version").get<int>() != 1 ||
      RequiredString(report, "report_type", "launcher report") !=
          "android_field_recording_browser_hil_launcher" ||
      !RequiredBoolean(report, "passed", "launcher report") ||
      RequiredString(report, "credential_transport", "launcher report") !=
          "adb_run_as_to_child_environment" ||
      RequiredBoolean(report, "credentials_retained", "launcher report") ||
      !RequiredBoolean(report, "exact_bazel_apk_verified", "launcher report") ||
      !RequiredBoolean(report, "evidence_contract_validated", "launcher report")) {
    throw std::runtime_error("Android browser HIL launcher top-level policy did not pass");
  }
  constexpr std::int64_t kMaximumPhonePreparationElapsedMs = 15'000;
  const std::int64_t phone_preparation_elapsed_ms =
      RequiredInteger(report, "phone_preparation_elapsed_ms", "launcher report");
  if (phone_preparation_elapsed_ms < 0 ||
      phone_preparation_elapsed_ms >= kMaximumPhonePreparationElapsedMs) {
    throw std::runtime_error("Android browser HIL phone preparation timing is invalid");
  }

  const Json &origins = report.at("origins");
  if (!origins.is_array() || origins.size() != 2U || !origins[0].is_string() ||
      !origins[1].is_string() || origins[0] == origins[1]) {
    throw std::runtime_error("Android browser HIL launcher requires two distinct origins");
  }
  ValidateDirectLanNodeOrigin(origins[0].get_ref<const std::string &>());
  ValidateDirectLanNodeOrigin(origins[1].get_ref<const std::string &>());
  const std::array<std::string_view, 2> expected_origins = {
      origins[0].get_ref<const std::string &>(), origins[1].get_ref<const std::string &>()};
  ValidateLauncherHttpReadiness(report.at("http_readiness"), expected_origins);
  AndroidFieldRecordingBrowserHilBrowserEvidence browser =
      ValidateBrowserEvidence(report.at("browser"));
  std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> fresh_media_decode =
      ValidateFreshMediaDecodeEvidence(report.at("fresh_media_decode"), expected_origins);

  const Json &apk = report.at("apk");
  if (!apk.is_array() || apk.size() != 2U) {
    throw std::runtime_error("Android browser HIL launcher requires two APK identities");
  }
  std::set<std::string, std::less<>> apk_roles;
  std::string common_sha;
  bool installed = false;
  for (const Json &node : apk) {
    RequireExactKeys(node,
                     {"role", "installed", "reason", "sha256", "installed_before_sha256",
                      "elapsed_ms", "installed_after_matches_bazel_apk"},
                     "Android browser HIL APK evidence");
    const std::string role = RequiredString(node, "role", "APK evidence");
    if ((role != "down_the_line" && role != "face_on") || !apk_roles.insert(role).second) {
      throw std::runtime_error("Android browser HIL APK roles are not complementary");
    }
    const bool node_installed = RequiredBoolean(node, "installed", "APK evidence");
    const std::string reason = RequiredString(node, "reason", "APK evidence");
    const std::string sha = RequiredString(node, "sha256", "APK evidence");
    if (!IsSha256(sha) || (!common_sha.empty() && sha != common_sha) ||
        !node.at("elapsed_ms").is_number_integer() ||
        node.at("elapsed_ms").get<std::int64_t>() < 0 ||
        !RequiredBoolean(node, "installed_after_matches_bazel_apk", "APK evidence")) {
      throw std::runtime_error("Android browser HIL APK identity is invalid");
    }
    common_sha = sha;
    if (reason == "exact_match") {
      if (node_installed || !node.at("installed_before_sha256").is_string() ||
          node.at("installed_before_sha256").get<std::string>() != sha) {
        throw std::runtime_error("Android browser HIL exact-match evidence is inconsistent");
      }
    } else if (reason == "different_or_unverifiable") {
      if (!node_installed ||
          (!node.at("installed_before_sha256").is_null() &&
           (!node.at("installed_before_sha256").is_string() ||
            !IsSha256(node.at("installed_before_sha256").get_ref<const std::string &>())))) {
        throw std::runtime_error("Android browser HIL replacement evidence is inconsistent");
      }
    } else {
      throw std::runtime_error("Android browser HIL APK install reason is ineligible");
    }
    installed = installed || node_installed;
  }

  const Json &primary = report.at("primary");
  RequireExactKeys(primary, {"passed", "timed_out", "exit_code", "error"},
                   "Android browser HIL primary evidence");
  if (!RequiredBoolean(primary, "passed", "primary evidence") ||
      RequiredBoolean(primary, "timed_out", "primary evidence") ||
      !primary.at("exit_code").is_number_integer() || primary.at("exit_code").get<int>() != 0 ||
      !RequiredString(primary, "error", "primary evidence").empty()) {
    throw std::runtime_error("Android browser HIL Playwright primary did not pass cleanly");
  }

  const Json &cleanup = report.at("cleanup");
  RequireExactKeys(cleanup, {"passed", "attempts"}, "Android browser HIL cleanup evidence");
  const Json &attempts = cleanup.at("attempts");
  if (!RequiredBoolean(cleanup, "passed", "cleanup evidence") || !attempts.is_array() ||
      attempts.size() != 2U) {
    throw std::runtime_error("Android browser HIL cleanup did not cover both phones");
  }
  std::set<std::string, std::less<>> cleanup_roles;
  for (const Json &attempt : attempts) {
    RequireExactKeys(attempt, {"role", "screen_sleep_restored"},
                     "Android browser HIL cleanup attempt");
    const std::string role = RequiredString(attempt, "role", "cleanup attempt");
    if ((role != "down_the_line" && role != "face_on") || !cleanup_roles.insert(role).second ||
        !RequiredBoolean(attempt, "screen_sleep_restored", "cleanup attempt")) {
      throw std::runtime_error("Android browser HIL screen cleanup is incomplete");
    }
  }
  return {.apk_sha256 = common_sha,
          .installed_on_at_least_one_phone = installed,
          .browser = std::move(browser),
          .fresh_media_decode = std::move(fresh_media_decode)};
}

}  // namespace swing_capture::web
