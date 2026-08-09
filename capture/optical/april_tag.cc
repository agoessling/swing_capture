#include "capture/optical/april_tag.h"

#include <apriltag.h>
#include <common/image_types.h>
#include <common/zarray.h>
#include <tag16h5.h>
#include <tag36h11.h>
#include <tagStandard41h12.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/image/image_quality.h"

namespace swing_capture::optical {
namespace {

apriltag_family_t *CreateFamily(AprilTagFamily family) {
  switch (family) {
    case AprilTagFamily::kTag16h5:
      return tag16h5_create();
    case AprilTagFamily::kTag36h11:
      return tag36h11_create();
    case AprilTagFamily::kTagStandard41h12:
      return tagStandard41h12_create();
  }
  throw std::invalid_argument("unsupported AprilTag family");
}

void DestroyFamily(AprilTagFamily family, apriltag_family_t *tag_family) {
  if (tag_family == nullptr) {
    return;
  }
  switch (family) {
    case AprilTagFamily::kTag16h5:
      tag16h5_destroy(tag_family);
      return;
    case AprilTagFamily::kTag36h11:
      tag36h11_destroy(tag_family);
      return;
    case AprilTagFamily::kTagStandard41h12:
      tagStandard41h12_destroy(tag_family);
      return;
  }
}

void ValidateDetectorOptions(const AprilTagDetectorOptions &options) {
  if (options.corrected_bits < 0 || options.corrected_bits > 2) {
    throw std::invalid_argument("AprilTag corrected bits must be between zero and two");
  }
  if (options.thread_count <= 0) {
    throw std::invalid_argument("AprilTag detector thread count must be positive");
  }
  if (!(options.quad_decimate > 0.0F)) {
    throw std::invalid_argument("AprilTag quad decimation must be positive");
  }
  if (options.decode_sharpening < 0.0) {
    throw std::invalid_argument("AprilTag decode sharpening cannot be negative");
  }
}

std::size_t ValidateImage(const image::Raw8ImageView &image) {
  if (image.width == 0 || image.height == 0) {
    throw std::invalid_argument("AprilTag image must have nonzero geometry");
  }
  const std::size_t stride = image.row_stride_bytes == 0 ? image.width : image.row_stride_bytes;
  if (stride < image.width) {
    throw std::invalid_argument("AprilTag image row stride is smaller than its width");
  }
  if (image.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
      image.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
      stride > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::invalid_argument("AprilTag image geometry exceeds the detector API limits");
  }
  if (static_cast<std::size_t>(image.height - 1U) >
      (std::numeric_limits<std::size_t>::max() - image.width) / stride) {
    throw std::invalid_argument("AprilTag image geometry overflows addressable memory");
  }
  const std::size_t required = static_cast<std::size_t>(image.height - 1U) * stride + image.width;
  if (image.pixels.size() < required) {
    throw std::invalid_argument("AprilTag image payload is smaller than its geometry");
  }
  return stride;
}

}  // namespace

struct AprilTagDetector::Impl {
  explicit Impl(const AprilTagDetectorOptions &requested_options)
      : options(requested_options), family(CreateFamily(options.family)) {
    if (family == nullptr) {
      throw std::runtime_error("AprilTag family allocation failed");
    }
    detector = apriltag_detector_create();
    if (detector == nullptr) {
      DestroyFamily(options.family, family);
      family = nullptr;
      throw std::runtime_error("AprilTag detector allocation failed");
    }
    detector->nthreads = options.thread_count;
    detector->quad_decimate = options.quad_decimate;
    detector->quad_sigma = options.quad_sigma;
    detector->refine_edges = options.refine_edges;
    detector->decode_sharpening = options.decode_sharpening;
    apriltag_detector_add_family_bits(detector, family, options.corrected_bits);
  }

  ~Impl() {
    apriltag_detector_destroy(detector);
    DestroyFamily(options.family, family);
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  AprilTagDetectorOptions options;
  apriltag_family_t *family = nullptr;
  apriltag_detector_t *detector = nullptr;
};

std::string_view AprilTagFamilyName(AprilTagFamily family) {
  switch (family) {
    case AprilTagFamily::kTag16h5:
      return "tag16h5";
    case AprilTagFamily::kTag36h11:
      return "tag36h11";
    case AprilTagFamily::kTagStandard41h12:
      return "tagStandard41h12";
  }
  return "unknown";
}

AprilTagDetector::AprilTagDetector(const AprilTagDetectorOptions &options) {
  ValidateDetectorOptions(options);
  impl_ = std::make_unique<Impl>(options);
}

AprilTagDetector::~AprilTagDetector() = default;

std::vector<AprilTagDetection> AprilTagDetector::Detect(const image::Raw8ImageView &image) {
  const std::size_t stride = ValidateImage(image);
  // The C API predates const-correct image views; detection treats this input
  // buffer as read-only and retains no pointer after returning.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
  image_u8_t header{
      .width = static_cast<std::int32_t>(image.width),
      .height = static_cast<std::int32_t>(image.height),
      .stride = static_cast<std::int32_t>(stride),
      .buf =
          const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(image.pixels.data())),
  };
  // NOLINTEND(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
  using DetectionsPtr = std::unique_ptr<zarray_t, decltype(&apriltag_detections_destroy)>;
  const DetectionsPtr detections(apriltag_detector_detect(impl_->detector, &header),
                                 &apriltag_detections_destroy);
  if (detections == nullptr) {
    throw std::runtime_error("AprilTag detection failed to allocate its result");
  }

  std::vector<AprilTagDetection> result;
  const int count = zarray_size(detections.get());
  result.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    apriltag_detection_t *detected = nullptr;
    zarray_get(detections.get(), index, static_cast<void *>(&detected));
    if (detected == nullptr) {
      throw std::runtime_error("AprilTag detector returned a null detection");
    }
    AprilTagDetection copied{
        .family = detected->family != nullptr && detected->family->name != nullptr
                      ? detected->family->name
                      : "unknown",
        .id = detected->id,
        .hamming = detected->hamming,
        .decision_margin = detected->decision_margin,
        .center = {.x = detected->c[0], .y = detected->c[1]},
        .corners = {},
    };
    for (std::size_t corner = 0; corner < copied.corners.size(); ++corner) {
      copied.corners[corner] = {
          .x = detected->p[corner][0],
          .y = detected->p[corner][1],
      };
    }
    result.push_back(std::move(copied));
  }
  return result;
}

AprilTagPresenceResult VerifyAprilTagPresence(const image::Raw8ImageView &representative_frame,
                                              AprilTagDetector &detector,
                                              const AprilTagPresenceCriteria &criteria) {
  if (criteria.maximum_hamming < 0 || criteria.maximum_hamming > 2) {
    throw std::invalid_argument("AprilTag presence maximum hamming must be between zero and two");
  }
  if (criteria.minimum_decision_margin < 0.0) {
    throw std::invalid_argument("AprilTag presence decision margin cannot be negative");
  }

  AprilTagPresenceResult result;
  result.detections = detector.Detect(representative_frame);
  auto accepted = result.detections.end();
  for (auto detection = result.detections.begin(); detection != result.detections.end();
       ++detection) {
    const bool meets_criteria =
        (!criteria.expected_id.has_value() || detection->id == *criteria.expected_id) &&
        detection->hamming <= criteria.maximum_hamming &&
        detection->decision_margin >= criteria.minimum_decision_margin;
    if (meets_criteria && (accepted == result.detections.end() ||
                           detection->decision_margin > accepted->decision_margin)) {
      accepted = detection;
    }
  }

  if (accepted != result.detections.end()) {
    result.present = true;
    result.accepted_detection = *accepted;
    std::ostringstream message;
    message << "detected " << accepted->family << " id=" << accepted->id
            << " with hamming=" << accepted->hamming
            << " and decision_margin=" << accepted->decision_margin;
    result.diagnostic = message.str();
    return result;
  }

  std::ostringstream message;
  if (result.detections.empty()) {
    message << "no AprilTag detections in representative frame";
  } else {
    message << result.detections.size() << " AprilTag detection(s), but none met";
    if (criteria.expected_id.has_value()) {
      message << " expected id=" << *criteria.expected_id << ',';
    }
    message << " maximum_hamming=" << criteria.maximum_hamming
            << " and minimum_decision_margin=" << criteria.minimum_decision_margin;
  }
  result.diagnostic = message.str();
  return result;
}

}  // namespace swing_capture::optical
