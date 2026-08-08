#ifndef SWING_CAPTURE_CAPTURE_DAHENG_DAHENG_SDK_RUNTIME_H_
#define SWING_CAPTURE_CAPTURE_DAHENG_DAHENG_SDK_RUNTIME_H_

#include <optional>
#include <string>

namespace swing_capture::daheng {

// Finds the directory containing the bundled GenTL producer in this binary's
// Bazel runfiles. Returns nullopt and describes the lookup failure in error
// when running outside a Bazel-built runfiles tree.
std::optional<std::string> FindBundledGentlDirectory(std::string *error);

}  // namespace swing_capture::daheng

#endif  // SWING_CAPTURE_CAPTURE_DAHENG_DAHENG_SDK_RUNTIME_H_
