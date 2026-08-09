#include "capture/optical/april_tag.h"

#include <apriltag.h>
#include <tag16h5.h>
#include <tag36h11.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using swing_capture::image::Raw8ImageView;
using swing_capture::optical::AprilTagDetector;
using swing_capture::optical::AprilTagDetectorOptions;
using swing_capture::optical::AprilTagFamily;
using swing_capture::optical::AprilTagFamilyName;
using swing_capture::optical::AprilTagPresenceCriteria;
using swing_capture::optical::VerifyAprilTagPresence;

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct OwnedImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t stride = 0;
  std::vector<std::byte> pixels;

  [[nodiscard]] Raw8ImageView view() const {
    return {
        .pixels = pixels,
        .width = width,
        .height = height,
        .row_stride_bytes = stride,
    };
  }
};

OwnedImage RenderTag(apriltag_family_t *family, int id) {
  using ImagePtr = std::unique_ptr<image_u8_t, decltype(&image_u8_destroy)>;
  ImagePtr tag(apriltag_to_image(family, static_cast<std::uint32_t>(id)), &image_u8_destroy);
  if (tag == nullptr) {
    throw std::runtime_error("test could not render AprilTag image");
  }

  constexpr std::uint32_t kScale = 10;
  constexpr std::uint32_t kMargin = 30;
  OwnedImage image{
      .width = 2U * kMargin + static_cast<std::uint32_t>(tag->width) * kScale,
      .height = 2U * kMargin + static_cast<std::uint32_t>(tag->height) * kScale,
      .stride = 0,
      .pixels = {},
  };
  image.stride = image.width + 7U;
  image.pixels.assign(image.stride * image.height, std::byte{255});
  for (std::int32_t source_y = 0; source_y < tag->height; ++source_y) {
    for (std::int32_t source_x = 0; source_x < tag->width; ++source_x) {
      const std::byte value{tag->buf[static_cast<std::size_t>(source_y) * tag->stride + source_x]};
      for (std::uint32_t dy = 0; dy < kScale; ++dy) {
        for (std::uint32_t dx = 0; dx < kScale; ++dx) {
          const std::uint32_t output_x =
              kMargin + static_cast<std::uint32_t>(source_x) * kScale + dx;
          const std::uint32_t output_y =
              kMargin + static_cast<std::uint32_t>(source_y) * kScale + dy;
          image.pixels[static_cast<std::size_t>(output_y) * image.stride + output_x] = value;
        }
      }
    }
  }
  return image;
}

OwnedImage RenderTag36h11(int id) {
  using FamilyPtr = std::unique_ptr<apriltag_family_t, decltype(&tag36h11_destroy)>;
  FamilyPtr family(tag36h11_create(), &tag36h11_destroy);
  if (family == nullptr) {
    throw std::runtime_error("test could not allocate tag36h11 family");
  }
  return RenderTag(family.get(), id);
}

OwnedImage RenderTag16h5(int id) {
  using FamilyPtr = std::unique_ptr<apriltag_family_t, decltype(&tag16h5_destroy)>;
  FamilyPtr family(tag16h5_create(), &tag16h5_destroy);
  if (family == nullptr) {
    throw std::runtime_error("test could not allocate tag16h5 family");
  }
  return RenderTag(family.get(), id);
}

void FindsExpectedTagInRepresentativeFrame() {
  constexpr int kExpectedId = 7;
  const auto image = RenderTag36h11(kExpectedId);
  AprilTagDetector detector;
  const auto result = VerifyAprilTagPresence(image.view(), detector,
                                             {
                                                 .expected_id = kExpectedId,
                                                 .maximum_hamming = 0,
                                                 .minimum_decision_margin = 10.0,
                                             });

  Expect(result.present, "rendered AprilTag is present");
  Expect(result.detections.size() == 1, "one tag detected");
  Expect(result.accepted_detection.has_value(), "accepted detection retained");
  if (result.accepted_detection.has_value()) {
    Expect(result.accepted_detection->family == "tag36h11", "detected family");
    Expect(result.accepted_detection->id == kExpectedId, "detected ID");
    Expect(result.accepted_detection->hamming == 0, "error-free decode");
    Expect(result.accepted_detection->decision_margin >= 10.0, "decode margin retained");
    Expect(result.accepted_detection->center.x > 0.0 && result.accepted_detection->center.y > 0.0,
           "image-space diagnostic center retained");
  }
  Expect(result.diagnostic.find("tag36h11 id=7") != std::string::npos,
         "presence diagnostic identifies tag");
}

void FindsRenderedTag16h5WithoutChangingTheDefaultFamily() {
  constexpr int kExpectedId = 3;
  const auto image = RenderTag16h5(kExpectedId);
  AprilTagDetector detector(AprilTagDetectorOptions{.family = AprilTagFamily::kTag16h5});
  const auto result = VerifyAprilTagPresence(image.view(), detector,
                                             {
                                                 .expected_id = kExpectedId,
                                                 .maximum_hamming = 0,
                                                 .minimum_decision_margin = 10.0,
                                             });

  Expect(AprilTagFamilyName(AprilTagFamily::kTag16h5) == "tag16h5", "tag16h5 API name is stable");
  Expect(AprilTagDetectorOptions{}.family == AprilTagFamily::kTag36h11,
         "tag36h11 remains the default family");
  Expect(result.present, "rendered tag16h5 is present");
  Expect(result.detections.size() == 1, "one tag16h5 detected");
  Expect(result.accepted_detection.has_value(), "tag16h5 detection retained");
  if (result.accepted_detection.has_value()) {
    Expect(result.accepted_detection->family == "tag16h5", "detected tag16h5 family");
    Expect(result.accepted_detection->id == kExpectedId, "detected tag16h5 ID");
    Expect(result.accepted_detection->hamming == 0, "error-free tag16h5 decode");
    Expect(result.accepted_detection->decision_margin >= 10.0, "tag16h5 decode margin retained");
  }
  Expect(result.diagnostic.find("tag16h5 id=3") != std::string::npos,
         "presence diagnostic identifies tag16h5");
}

void RejectsWrongIdentityWithoutClaimingGeometry() {
  const auto image = RenderTag36h11(7);
  AprilTagDetector detector;
  const auto result = VerifyAprilTagPresence(image.view(), detector,
                                             {
                                                 .expected_id = 8,
                                                 .maximum_hamming = 1,
                                                 .minimum_decision_margin = 0.0,
                                             });

  Expect(!result.present, "wrong tag ID is not accepted");
  Expect(result.detections.size() == 1, "rejected detection remains diagnostic evidence");
  Expect(!result.accepted_detection.has_value(), "no rejected detection is marked accepted");
  Expect(result.diagnostic.find("expected id=8") != std::string::npos,
         "identity failure is explicit");
}

void ReportsNoDetectionForBlankFrame() {
  OwnedImage blank{
      .width = 160,
      .height = 120,
      .stride = 160,
      .pixels = std::vector<std::byte>(160U * 120U, std::byte{180}),
  };
  AprilTagDetector detector;
  const auto result = VerifyAprilTagPresence(blank.view(), detector);

  Expect(!result.present, "blank representative frame has no tag");
  Expect(result.detections.empty(), "blank frame yields no detections");
  Expect(result.diagnostic == "no AprilTag detections in representative frame",
         "blank-frame diagnostic is explicit");
}

template <typename Function>
void ExpectInvalidArgument(Function &&function, const std::string &message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Expect(rejected, message);
}

void RejectsMalformedImageAndCriteria() {
  AprilTagDetector detector;
  const std::vector<std::byte> pixels(10);
  const Raw8ImageView malformed{
      .pixels = pixels,
      .width = 20,
      .height = 20,
  };
  ExpectInvalidArgument([&] { (void)detector.Detect(malformed); }, "short tag image rejected");

  const auto image = RenderTag36h11(7);
  ExpectInvalidArgument(
      [&] {
        (void)VerifyAprilTagPresence(image.view(), detector,
                                     AprilTagPresenceCriteria{
                                         .expected_id = std::nullopt,
                                         .maximum_hamming = 3,
                                         .minimum_decision_margin = 10.0,
                                     });
      },
      "unsupported hamming threshold rejected");
}

}  // namespace

int main() {
  FindsExpectedTagInRepresentativeFrame();
  FindsRenderedTag16h5WithoutChangingTheDefaultFamily();
  RejectsWrongIdentityWithoutClaimingGeometry();
  ReportsNoDetectionForBlankFrame();
  RejectsMalformedImageAndCriteria();
  return failures == 0 ? 0 : 1;
}
