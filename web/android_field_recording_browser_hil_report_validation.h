#ifndef SWING_CAPTURE_WEB_ANDROID_FIELD_RECORDING_BROWSER_HIL_REPORT_VALIDATION_H_
#define SWING_CAPTURE_WEB_ANDROID_FIELD_RECORDING_BROWSER_HIL_REPORT_VALIDATION_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::web {

struct AndroidFieldRecordingBrowserHilBrowserEvidence {
  std::string engine;
  std::string distribution;
  std::string version;
  std::string user_agent;
  bool h264_decode_required = false;
};

struct AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence {
  std::string role;
  std::string origin;
  std::string media_identity;
  double duration_seconds = 0.0;
  std::int64_t video_width = 0;
  std::int64_t video_height = 0;
  double rvfc_media_time_seconds = 0.0;
  std::int64_t rvfc_presented_frames = 0;
  double nonblack_fraction = 0.0;
  double playback_start_seconds = 0.0;
  double playback_end_seconds = 0.0;
  double playback_advanced_seconds = 0.0;
};

struct AndroidFieldRecordingBrowserHilChildReportEvidence {
  AndroidFieldRecordingBrowserHilBrowserEvidence browser;
  std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> fresh_media_decode;
};

struct AndroidFieldRecordingBrowserHilReportEvidence {
  std::string apk_sha256;
  bool installed_on_at_least_one_phone = false;
  AndroidFieldRecordingBrowserHilBrowserEvidence browser;
  std::vector<AndroidFieldRecordingBrowserHilFreshMediaDecodeEvidence> fresh_media_decode;
};

/** Validates the passing Playwright report against the exact configured phone origins. */
[[nodiscard]] AndroidFieldRecordingBrowserHilChildReportEvidence
ValidateAndroidFieldRecordingBrowserHilChildReport(std::string_view report_json,
                                                   std::string_view down_the_line_origin,
                                                   std::string_view face_on_origin);

/** Validates a passing launcher artifact; failure/partial reports are intentionally ineligible. */
[[nodiscard]] AndroidFieldRecordingBrowserHilReportEvidence
ValidateAndroidFieldRecordingBrowserHilLauncherReport(std::string_view report_json);

}  // namespace swing_capture::web

#endif  // SWING_CAPTURE_WEB_ANDROID_FIELD_RECORDING_BROWSER_HIL_REPORT_VALIDATION_H_
