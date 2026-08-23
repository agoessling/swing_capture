#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_ANDROID_APK_INSTALL_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_ANDROID_APK_INSTALL_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace swing_capture::android::dual_hil {

struct AndroidApkInstallEvidence {
  bool installed = false;
  std::string reason;
  std::string sha256;
  std::optional<std::string> installed_before_sha256;
  std::int64_t elapsed_ms = 0;
};

// The Bazel tool paths intentionally remain adjacent to keep the HIL call sites compact.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] AndroidApkInstallEvidence InstallAndroidApk(
    const std::filesystem::path &installer, const std::filesystem::path &adb,
    const std::filesystem::path &apk, std::string_view serial, bool skip_exact_match,
    std::chrono::steady_clock::time_point deadline);

/** Read-only exact-identity gate. Never installs or otherwise changes the phone. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] AndroidApkInstallEvidence VerifyInstalledAndroidApk(
    const std::filesystem::path &installer, const std::filesystem::path &adb,
    const std::filesystem::path &apk, std::string_view serial,
    std::chrono::steady_clock::time_point deadline);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_ANDROID_APK_INSTALL_H_
