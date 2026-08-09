#ifndef SWING_CAPTURE_CAPTURE_OPTICAL_APRIL_TAG_H_
#define SWING_CAPTURE_CAPTURE_OPTICAL_APRIL_TAG_H_

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "capture/image/image_quality.h"

namespace swing_capture::optical {

enum class AprilTagFamily {
  kTag16h5,
  kTag36h11,
  kTagStandard41h12,
};

[[nodiscard]] std::string_view AprilTagFamilyName(AprilTagFamily family);

struct AprilTagDetectorOptions {
  AprilTagFamily family = AprilTagFamily::kTag36h11;
  int corrected_bits = 1;
  int thread_count = 1;
  float quad_decimate = 1.0F;
  float quad_sigma = 0.0F;
  bool refine_edges = true;
  double decode_sharpening = 0.25;
};

struct ImagePoint {
  double x = 0.0;
  double y = 0.0;
};

struct AprilTagDetection {
  std::string family;
  int id = -1;
  int hamming = 0;
  double decision_margin = 0.0;
  ImagePoint center;
  std::array<ImagePoint, 4> corners;
};

class AprilTagDetector final {
 public:
  explicit AprilTagDetector(const AprilTagDetectorOptions &options = {});
  ~AprilTagDetector();

  AprilTagDetector(const AprilTagDetector &) = delete;
  AprilTagDetector &operator=(const AprilTagDetector &) = delete;
  AprilTagDetector(AprilTagDetector &&) = delete;
  AprilTagDetector &operator=(AprilTagDetector &&) = delete;

  // The input must be a true single-channel intensity image. A color Bayer
  // mosaic must be demosaiced and converted to luminance upstream; treating
  // its alternating color sites as grayscale can prevent a valid decode. The
  // detector owns no reference to the pixels after return.
  [[nodiscard]] std::vector<AprilTagDetection> Detect(const image::Raw8ImageView &image);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct AprilTagPresenceCriteria {
  std::optional<int> expected_id;
  int maximum_hamming = 1;
  double minimum_decision_margin = 10.0;
};

struct AprilTagPresenceResult {
  bool present = false;
  std::string diagnostic;
  std::vector<AprilTagDetection> detections;
  std::optional<AprilTagDetection> accepted_detection;
};

// Verifies only that a tag matching the requested identity and decode-quality
// criteria is present. Corners are retained for diagnostics, but this function
// makes no pose, scale, camera-calibration, or geometric-calibration claim.
[[nodiscard]] AprilTagPresenceResult VerifyAprilTagPresence(
    const image::Raw8ImageView &representative_frame, AprilTagDetector &detector,
    const AprilTagPresenceCriteria &criteria = {});

}  // namespace swing_capture::optical

#endif  // SWING_CAPTURE_CAPTURE_OPTICAL_APRIL_TAG_H_
