#ifndef SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_COMMAND_H_
#define SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_COMMAND_H_

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace swing_capture::android::dual_hil {

struct HilCommandResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

[[nodiscard]] HilCommandResult RunHilCommand(const std::filesystem::path &executable,
                                             const std::vector<std::string> &arguments,
                                             std::chrono::steady_clock::time_point deadline,
                                             std::string_view input = {});

[[nodiscard]] std::string RunRequiredHilCommand(const std::filesystem::path &executable,
                                                const std::vector<std::string> &arguments,
                                                std::chrono::steady_clock::time_point deadline);

}  // namespace swing_capture::android::dual_hil

#endif  // SWING_CAPTURE_ANDROID_DUAL_HIL_HIL_COMMAND_H_
