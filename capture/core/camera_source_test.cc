#include "capture/core/camera_source.h"

#include <cstdlib>
#include <iostream>

namespace swing_capture {
namespace {

bool DefaultsAreExplicitlyInvalid() {
  const CaptureProfile profile;

  return profile.width == 0U && profile.height == 0U && profile.frames_per_second == 0.0 &&
         profile.pixel_format.empty();
}

bool IncompleteUntilCaptureBackendMarksSuccess() {
  const FrameMetadata metadata;

  return !metadata.complete && metadata.frame_id == 0U && metadata.device_timestamp == 0U;
}

}  // namespace
}  // namespace swing_capture

int main() {
  if (!swing_capture::DefaultsAreExplicitlyInvalid()) {
    std::cerr << "CaptureProfile defaults are not explicitly invalid\n";
    return EXIT_FAILURE;
  }
  if (!swing_capture::IncompleteUntilCaptureBackendMarksSuccess()) {
    std::cerr << "FrameMetadata defaults incorrectly report a frame\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
