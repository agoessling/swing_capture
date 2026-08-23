#include "web/android_field_recording_browser_hil_report_validation.h"

#include <cassert>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

using Json = nlohmann::json;
using swing_capture::web::ValidateAndroidFieldRecordingBrowserHilChildReport;
using swing_capture::web::ValidateAndroidFieldRecordingBrowserHilLauncherReport;

constexpr const char *kDownOrigin = "http://10.0.0.1:8088";
constexpr const char *kFaceOrigin = "http://10.0.0.2:8088";

Json PassingBrowserEvidence() {
  return {{"engine", "chromium"},
          {"distribution", "google_chrome"},
          {"version", "140.0.7339.80"},
          {"user_agent",
           "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) HeadlessChrome/140.0.0.0 Safari/537.36"},
          {"h264_decode_required", true}};
}

Json PassingFreshMediaDecode() {
  return Json::array({Json{{"role", "down_the_line"},
                           {"origin", kDownOrigin},
                           {"media_identity", std::string(kDownOrigin) +
                                                  "/api/v1/field-recordings/field-1234/video.mp4"},
                           {"duration_seconds", 1.25},
                           {"video_width", 1280},
                           {"video_height", 720},
                           {"rvfc_media_time_seconds", 0.04},
                           {"rvfc_presented_frames", 1},
                           {"nonblack_fraction", 0.82},
                           {"playback_start_seconds", 0.0},
                           {"playback_end_seconds", 0.15},
                           {"playback_advanced_seconds", 0.15}},
                      Json{{"role", "face_on"},
                           {"origin", kFaceOrigin},
                           {"media_identity", std::string(kFaceOrigin) +
                                                  "/api/v1/field-recordings/field-1234/video.mp4"},
                           {"duration_seconds", 1.25},
                           {"video_width", 1920},
                           {"video_height", 1080},
                           {"rvfc_media_time_seconds", 0.05},
                           {"rvfc_presented_frames", 2},
                           {"nonblack_fraction", 0.77},
                           {"playback_start_seconds", 0.0},
                           {"playback_end_seconds", 0.16},
                           {"playback_advanced_seconds", 0.16}}});
}

Json PassingHttpReadiness() {
  const Json down_attempts = Json::array(
      {Json{{"sequence", 1},
            {"elapsed_ms", 0},
            {"http_status", nullptr},
            {"outcome", "transport_error"}},
       Json{{"sequence", 2}, {"elapsed_ms", 100}, {"http_status", 200}, {"outcome", "ready"}}});
  const Json face_attempts = Json::array(
      {Json{{"sequence", 1}, {"elapsed_ms", 0}, {"http_status", 200}, {"outcome", "ready"}}});
  return {{"passed", true},
          {"probe_method", "GET"},
          {"probe_path", "/api/v1/node"},
          {"nodes", Json::array({Json{{"role", "down_the_line"},
                                      {"origin", kDownOrigin},
                                      {"timeout_ms", 3'000},
                                      {"passed", true},
                                      {"attempts", down_attempts}},
                                 Json{{"role", "face_on"},
                                      {"origin", kFaceOrigin},
                                      {"timeout_ms", 3'000},
                                      {"passed", true},
                                      {"attempts", face_attempts}}})}};
}

Json PassingReport() {
  constexpr const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  return {
      {"schema_version", 1},
      {"report_type", "android_field_recording_browser_hil_launcher"},
      {"passed", true},
      {"credential_transport", "adb_run_as_to_child_environment"},
      {"credentials_retained", false},
      {"origins", Json::array({kDownOrigin, kFaceOrigin})},
      {"exact_bazel_apk_verified", true},
      {"apk", Json::array({Json{{"role", "down_the_line"},
                                {"installed", false},
                                {"reason", "exact_match"},
                                {"sha256", sha},
                                {"installed_before_sha256", sha},
                                {"elapsed_ms", 7},
                                {"installed_after_matches_bazel_apk", true}},
                           Json{{"role", "face_on"},
                                {"installed", true},
                                {"reason", "different_or_unverifiable"},
                                {"sha256", sha},
                                {"installed_before_sha256", nullptr},
                                {"elapsed_ms", 21},
                                {"installed_after_matches_bazel_apk", true}}})},
      {"phone_preparation_elapsed_ms", 3'353},
      {"http_readiness", PassingHttpReadiness()},
      {"primary", {{"passed", true}, {"timed_out", false}, {"exit_code", 0}, {"error", ""}}},
      {"cleanup",
       {{"passed", true},
        {"attempts", Json::array({Json{{"role", "down_the_line"}, {"screen_sleep_restored", true}},
                                  Json{{"role", "face_on"}, {"screen_sleep_restored", true}}})}}},
      {"browser", PassingBrowserEvidence()},
      {"fresh_media_decode", PassingFreshMediaDecode()},
      {"evidence_contract_validated", true},
  };
}

Json PassingChildReport() {
  return {
      {"schema_version", 1},
      {"report_type", "android_field_recording_browser_hil"},
      {"passed", true},
      {"error", ""},
      {"credential_bootstrap", "tab_scoped_session_storage_from_adb_injected_environment"},
      {"credential_parameters_absent", true},
      {"application_origin", kDownOrigin},
      {"node_origins", Json::array({kDownOrigin, kFaceOrigin})},
      {"phone_hosted_assets", {{"/app.css", 200}, {"/app.js", 200}}},
      {"browser", PassingBrowserEvidence()},
      {"api_statuses",
       {{std::string("POST ") + kDownOrigin + "/api/v1/field-recording/start", Json::array({202})},
        {std::string("POST ") + kDownOrigin + "/api/v1/field-recording/stop", Json::array({202})},
        {std::string("POST ") + kFaceOrigin + "/api/v1/field-recording/start", Json::array({202})},
        {std::string("POST ") + kFaceOrigin + "/api/v1/field-recording/stop", Json::array({202})},
        {std::string("GET ") + kDownOrigin + "/api/v1/field-recordings", Json::array({200})}}},
      {"camera_stage_elapsed_ms", 5'500},
      {"camera_stage_limit_ms", 15'000},
      {"startup_convergence_timeout_ms", 6'000},
      {"page_ready_elapsed_ms", 2'800},
      {"start_convergence_elapsed_ms", 2'900},
      {"stop_convergence_elapsed_ms", 900},
      {"post_camera_validation_elapsed_ms", 1'200},
      {"cleanup_elapsed_ms", 70},
      {"primary_stop_evidence",
       Json::array({Json{{"origin", kDownOrigin}, {"statuses", Json::array({202})}},
                    Json{{"origin", kFaceOrigin}, {"statuses", Json::array({202})}}})},
      {"watchdog_fired", false},
      {"completed_bundle_origins", Json::array({kFaceOrigin, kDownOrigin})},
      {"range_retrievals",
       Json::array(
           {Json{{"origin", kDownOrigin}, {"kind", "video"}, {"status", 206}, {"bytes", 128}},
            Json{{"origin", kFaceOrigin}, {"kind", "video"}, {"status", 206}, {"bytes", 128}},
            Json{{"origin", kDownOrigin}, {"kind", "audio"}, {"status", 206}, {"bytes", 128}},
            Json{{"origin", kFaceOrigin}, {"kind", "audio"}, {"status", 206}, {"bytes", 128}}})},
      {"fresh_media_decode", PassingFreshMediaDecode()},
      {"fresh_decode_media_aborts", Json::array()},
      {"review_media_aborts", Json::array()},
      {"accessibility_violations", Json::array()},
      {"page_errors", Json::array()},
      {"failed_requests", Json::array()},
      {"screenshot", "android-field-recording-browser-hil.png"},
      {"cleanup",
       Json::array(
           {Json{{"origin", kDownOrigin}, {"stop_status", 202}, {"final_state", "ready"}},
            Json{{"origin", kFaceOrigin}, {"stop_status", nullptr}, {"final_state", "idle"}}})},
      {"completed_bundles_retained", true},
  };
}

void Rejects(const std::function<void(Json &)> &mutate) {
  Json report = PassingReport();
  mutate(report);
  try {
    static_cast<void>(ValidateAndroidFieldRecordingBrowserHilLauncherReport(report.dump()));
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("invalid Android browser HIL launcher report was accepted");
}

void RejectsChild(const std::function<void(Json &)> &mutate) {
  Json report = PassingChildReport();
  mutate(report);
  try {
    static_cast<void>(ValidateAndroidFieldRecordingBrowserHilChildReport(report.dump(), kDownOrigin,
                                                                         kFaceOrigin));
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("invalid Android browser HIL child report was accepted");
}

}  // namespace

int main() {
  const auto evidence =
      ValidateAndroidFieldRecordingBrowserHilLauncherReport(PassingReport().dump());
  assert(!evidence.apk_sha256.empty());
  assert(evidence.installed_on_at_least_one_phone);
  assert(evidence.browser.distribution == "google_chrome");
  assert(evidence.fresh_media_decode.size() == 2U);

  const auto child_evidence = ValidateAndroidFieldRecordingBrowserHilChildReport(
      PassingChildReport().dump(), kDownOrigin, kFaceOrigin);
  assert(child_evidence.browser.engine == "chromium");
  assert(child_evidence.fresh_media_decode.size() == 2U);

  Json expected_abort = PassingChildReport();
  const std::string aborted_path = "/api/v1/sessions/session-1/down_the_line.mp4";
  expected_abort["review_media_aborts"].push_back(
      {{"origin", kDownOrigin}, {"path", aborted_path}, {"error", "net::ERR_ABORTED"}});
  expected_abort["api_statuses"][std::string("GET ") + kDownOrigin + aborted_path] =
      Json::array({206});
  const std::string decoded_identity =
      std::string(kDownOrigin) + "/api/v1/field-recordings/field-1234/video.mp4";
  expected_abort["fresh_decode_media_aborts"].push_back({{"role", "down_the_line"},
                                                         {"origin", kDownOrigin},
                                                         {"media_identity", decoded_identity},
                                                         {"error", "net::ERR_ABORTED"}});
  expected_abort["api_statuses"]["GET " + decoded_identity] = Json::array({206});
  static_cast<void>(ValidateAndroidFieldRecordingBrowserHilChildReport(expected_abort.dump(),
                                                                       kDownOrigin, kFaceOrigin));

  Rejects([](Json &report) { report["unexpected"] = "not schema-bound"; });
  Rejects([](Json &report) { report["passed"] = false; });
  Rejects([](Json &report) { report["credentials_retained"] = true; });
  Rejects([](Json &report) { report.erase("phone_preparation_elapsed_ms"); });
  Rejects([](Json &report) { report["phone_preparation_elapsed_ms"] = nullptr; });
  Rejects([](Json &report) { report["phone_preparation_elapsed_ms"] = -1; });
  Rejects([](Json &report) { report["phone_preparation_elapsed_ms"] = 15'000; });
  Rejects([](Json &report) { report["origins"][1] = report["origins"][0]; });
  Rejects([](Json &report) { report["origins"][1] = "http://127.0.0.1:8088"; });
  Rejects([](Json &report) { report["exact_bazel_apk_verified"] = false; });
  Rejects([](Json &report) { report["apk"].erase(1); });
  Rejects([](Json &report) { report["apk"][1]["role"] = "down_the_line"; });
  Rejects([](Json &report) { report["apk"][1]["sha256"] = std::string(64, 'f'); });
  Rejects([](Json &report) { report["apk"][0]["installed"] = true; });
  Rejects([](Json &report) { report["http_readiness"]["passed"] = false; });
  Rejects([](Json &report) { report["http_readiness"]["probe_path"] = "/"; });
  Rejects([](Json &report) { report["http_readiness"]["nodes"].erase(1); });
  Rejects([](Json &report) { report["http_readiness"]["nodes"][1]["origin"] = kDownOrigin; });
  Rejects([](Json &report) { report["http_readiness"]["nodes"][0]["timeout_ms"] = 3'001; });
  Rejects([](Json &report) {
    report["http_readiness"]["nodes"][0]["attempts"][0]["http_status"] = 503;
  });
  Rejects([](Json &report) {
    report["http_readiness"]["nodes"][0]["attempts"].back()["outcome"] = "transport_error";
  });
  Rejects([](Json &report) { report["primary"]["timed_out"] = true; });
  Rejects([](Json &report) { report["primary"]["exit_code"] = 1; });
  Rejects([](Json &report) { report["cleanup"]["attempts"].erase(1); });
  Rejects([](Json &report) { report["cleanup"]["attempts"][1]["screen_sleep_restored"] = false; });
  Rejects([](Json &report) { report["evidence_contract_validated"] = false; });
  Rejects([](Json &report) { report["browser"]["distribution"] = "chromium_shell"; });
  Rejects([](Json &report) { report["browser"]["version"] = ""; });
  Rejects([](Json &report) { report["browser"]["user_agent"] = "Firefox/153.0"; });
  Rejects([](Json &report) { report["browser"]["h264_decode_required"] = false; });
  Rejects([](Json &report) { report["browser"]["unexpected"] = true; });
  Rejects([](Json &report) { report["fresh_media_decode"].erase(1); });
  Rejects([](Json &report) { report["fresh_media_decode"][0]["unexpected"] = true; });
  Rejects([](Json &report) { report["fresh_media_decode"][1]["role"] = "down_the_line"; });
  Rejects([](Json &report) { report["fresh_media_decode"][1]["origin"] = kDownOrigin; });
  Rejects([](Json &report) {
    report["fresh_media_decode"][0]["media_identity"] =
        "http://127.0.0.1:9000/fixtures/down-the-line.mp4";
  });
  Rejects([](Json &report) {
    report["fresh_media_decode"][0]["media_identity"] =
        std::string(kDownOrigin) + "/fixtures/down-the-line.mp4";
  });
  Rejects([](Json &report) { report["fresh_media_decode"][0]["duration_seconds"] = 0.0; });
  Rejects([](Json &report) { report["fresh_media_decode"][0]["video_width"] = 0; });
  Rejects([](Json &report) { report["fresh_media_decode"][0]["rvfc_presented_frames"] = 0; });
  Rejects([](Json &report) { report["fresh_media_decode"][0]["nonblack_fraction"] = 0.0; });
  Rejects([](Json &report) {
    report["fresh_media_decode"][0]["playback_end_seconds"] = 0.05;
    report["fresh_media_decode"][0]["playback_advanced_seconds"] = 0.05;
  });
  Rejects([](Json &report) {
    report["fresh_media_decode"][1]["media_identity"] =
        std::string(kFaceOrigin) + "/api/v1/field-recordings/stale/video.mp4";
  });

  RejectsChild([](Json &report) { report["unexpected"] = true; });
  RejectsChild([](Json &report) { report["passed"] = false; });
  RejectsChild([](Json &report) { report["error"] = "decode failed"; });
  RejectsChild([](Json &report) { report["credential_parameters_absent"] = false; });
  RejectsChild([](Json &report) { report["node_origins"][1] = kDownOrigin; });
  RejectsChild([](Json &report) { report["phone_hosted_assets"]["/app.js"] = 404; });
  RejectsChild([](Json &report) {
    report["api_statuses"][std::string("POST ") + kFaceOrigin + "/api/v1/field-recording/start"] =
        Json::array({409});
  });
  RejectsChild([](Json &report) { report["camera_stage_elapsed_ms"] = 15'000; });
  RejectsChild([](Json &report) { report["page_ready_elapsed_ms"] = nullptr; });
  RejectsChild([](Json &report) { report["primary_stop_evidence"].erase(1); });
  RejectsChild([](Json &report) { report["watchdog_fired"] = true; });
  RejectsChild([](Json &report) { report["completed_bundle_origins"].erase(1); });
  RejectsChild([](Json &report) { report["range_retrievals"][0]["status"] = 200; });
  RejectsChild([](Json &report) { report["accessibility_violations"] = {"color-contrast"}; });
  RejectsChild([](Json &report) { report["page_errors"] = {"decoder crashed"}; });
  RejectsChild([](Json &report) { report["failed_requests"] = {"video.mp4"}; });
  RejectsChild([](Json &report) { report["cleanup"][1]["final_state"] = "recording"; });
  RejectsChild([](Json &report) { report["completed_bundles_retained"] = false; });
  RejectsChild([](Json &report) { report["browser"]["distribution"] = "chromium_shell"; });
  RejectsChild([](Json &report) { report["browser"]["unexpected"] = true; });
  RejectsChild([](Json &report) { report["fresh_media_decode"].erase(0); });
  RejectsChild([](Json &report) {
    report["fresh_decode_media_aborts"].push_back(
        {{"role", "down_the_line"},
         {"origin", kDownOrigin},
         {"media_identity", std::string(kDownOrigin) + "/api/v1/field-recordings/other/video.mp4"},
         {"error", "net::ERR_ABORTED"}});
  });
  RejectsChild([](Json &report) { report["fresh_media_decode"][0]["unexpected"] = true; });
  RejectsChild([](Json &report) {
    report["fresh_media_decode"][0]["media_identity"] =
        report["fresh_media_decode"][0]["media_identity"].get<std::string>() +
        "?media_access=secret";
  });
  RejectsChild([](Json &report) { report["fresh_media_decode"][1]["video_height"] = 0; });
  RejectsChild(
      [](Json &report) { report["fresh_media_decode"][1]["rvfc_media_time_seconds"] = -0.01; });
  RejectsChild(
      [](Json &report) { report["fresh_media_decode"][1]["playback_advanced_seconds"] = 0.4; });
  RejectsChild([](Json &report) {
    report["review_media_aborts"].push_back(
        {{"origin", kDownOrigin},
         {"path", "/api/v1/sessions/session-1/down_the_line.mp4"},
         {"error", "net::ERR_ABORTED"}});
  });
  RejectsChild([](Json &report) {
    const std::string path = "/api/v1/field-recordings/field-1/video.mp4";
    report["review_media_aborts"].push_back(
        {{"origin", kDownOrigin}, {"path", path}, {"error", "net::ERR_ABORTED"}});
    report["api_statuses"][std::string("GET ") + kDownOrigin + path] = Json::array({206});
  });
  RejectsChild([](Json &report) {
    const std::string path = "/api/v1/sessions/session-1/down_the_line.mp4";
    report["api_statuses"][std::string("GET ") + kDownOrigin + path] = Json::array({206});
    for (int index = 0; index < 9; ++index) {
      report["review_media_aborts"].push_back(
          {{"origin", kDownOrigin}, {"path", path}, {"error", "net::ERR_ABORTED"}});
    }
  });
}
