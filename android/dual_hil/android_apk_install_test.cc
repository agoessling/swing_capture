#include "android/dual_hil/android_apk_install.h"

#include <cassert>
#include <chrono>
#include <filesystem>

namespace {

using namespace std::chrono_literals;
using swing_capture::android::dual_hil::InstallAndroidApk;
using swing_capture::android::dual_hil::VerifyInstalledAndroidApk;

}  // namespace

int main(int argument_count, char **arguments) {
  assert(argument_count == 2);
  const std::filesystem::path installer(arguments[1]);
  const auto forced = InstallAndroidApk(installer, "adb", "app.apk", "serial", false,
                                        std::chrono::steady_clock::now() + 1s);
  assert(forced.installed);
  assert(forced.reason == "forced");
  const auto exact = InstallAndroidApk(installer, "adb", "app.apk", "serial", true,
                                       std::chrono::steady_clock::now() + 1s);
  assert(!exact.installed);
  assert(exact.reason == "exact_match");
  assert(exact.installed_before_sha256 == exact.sha256);
  const auto verified = VerifyInstalledAndroidApk(installer, "adb", "app.apk", "serial",
                                                  std::chrono::steady_clock::now() + 1s);
  assert(!verified.installed);
  assert(verified.reason == "exact_match");
}
