#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/encoding/clip_session.h"
#include "capture/image/bayer_rg8.h"
#include "capture/preview/preview_image.h"
#include "vpx/vp8cx.h"
#include "vpx/vpx_encoder.h"
#include "vpx/vpx_image.h"

namespace swing_capture::encoding {
namespace {

constexpr std::uint64_t kWebmTimecodeScaleNanoseconds = 1'000;
constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
using ProfileClock = std::chrono::steady_clock;

double ElapsedMilliseconds(ProfileClock::time_point started) {
  return std::chrono::duration<double, std::milli>(ProfileClock::now() - started).count();
}

std::string CodecError(const vpx_codec_ctx_t &codec, std::string_view operation) {
  std::string message(operation);
  message.append(": ");
  message.append(vpx_codec_error(&codec));
  const char *detail = vpx_codec_error_detail(&codec);
  if (detail != nullptr) {
    message.append(" (");
    message.append(detail);
    message.push_back(')');
  }
  return message;
}

class CodecGuard final {
 public:
  [[nodiscard]] vpx_codec_ctx_t *get() noexcept { return &codec_; }
  [[nodiscard]] const vpx_codec_ctx_t &value() const noexcept { return codec_; }
  void MarkInitialized() noexcept { initialized_ = true; }
  ~CodecGuard() {
    if (initialized_) {
      static_cast<void>(vpx_codec_destroy(&codec_));
    }
  }

  CodecGuard(const CodecGuard &) = delete;
  CodecGuard &operator=(const CodecGuard &) = delete;
  CodecGuard(CodecGuard &&) = delete;
  CodecGuard &operator=(CodecGuard &&) = delete;
  CodecGuard() = default;

 private:
  vpx_codec_ctx_t codec_{};
  bool initialized_ = false;
};

class ImageGuard final {
 public:
  ImageGuard(std::uint32_t width, std::uint32_t height) {
    if (vpx_img_alloc(&image_, VPX_IMG_FMT_I420, width, height, 32) == nullptr) {
      throw std::runtime_error("libvpx could not allocate an I420 frame");
    }
    allocated_ = true;
  }
  ~ImageGuard() {
    if (allocated_) {
      vpx_img_free(&image_);
    }
  }

  ImageGuard(const ImageGuard &) = delete;
  ImageGuard &operator=(const ImageGuard &) = delete;
  ImageGuard(ImageGuard &&) = delete;
  ImageGuard &operator=(ImageGuard &&) = delete;

  [[nodiscard]] vpx_image_t *get() noexcept { return &image_; }

 private:
  vpx_image_t image_{};
  bool allocated_ = false;
};

std::uint8_t LimitedLuma(std::uint32_t red, std::uint32_t green, std::uint32_t blue) {
  return static_cast<std::uint8_t>(
      std::clamp<std::int32_t>(
          (66 * static_cast<std::int32_t>(red) + 129 * static_cast<std::int32_t>(green) +
           25 * static_cast<std::int32_t>(blue) + 128) >>
              8,
          0, 219) +
      16);
}

std::uint8_t LimitedChromaU(std::uint32_t red, std::uint32_t green, std::uint32_t blue) {
  return static_cast<std::uint8_t>(std::clamp<std::int32_t>(
      ((-38 * static_cast<std::int32_t>(red) - 74 * static_cast<std::int32_t>(green) +
        112 * static_cast<std::int32_t>(blue) + 128) >>
       8) +
          128,
      16, 240));
}

std::uint8_t LimitedChromaV(std::uint32_t red, std::uint32_t green, std::uint32_t blue) {
  return static_cast<std::uint8_t>(std::clamp<std::int32_t>(
      ((112 * static_cast<std::int32_t>(red) - 94 * static_cast<std::int32_t>(green) -
        18 * static_cast<std::int32_t>(blue) + 128) >>
       8) +
          128,
      16, 240));
}

image::Rgb8Image CropToEven(const image::Rgb8Image &source) {
  const std::uint32_t width = source.width & ~1U;
  const std::uint32_t height = source.height & ~1U;
  if (width < 2U || height < 2U) {
    throw std::invalid_argument("fitted clip dimensions must both be at least two");
  }
  if (width == source.width && height == source.height) {
    return source;
  }

  image::Rgb8Image output{
      .width = width,
      .height = height,
      .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3U),
  };
  const std::size_t input_row_bytes = static_cast<std::size_t>(source.width) * 3U;
  const std::size_t output_row_bytes = static_cast<std::size_t>(width) * 3U;
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto input = source.pixels.begin() + static_cast<std::ptrdiff_t>(y * input_row_bytes);
    const auto output_row =
        output.pixels.begin() + static_cast<std::ptrdiff_t>(y * output_row_bytes);
    std::ranges::copy_n(input, static_cast<std::ptrdiff_t>(output_row_bytes), output_row);
  }
  return output;
}

image::Rgb8Image PrepareRgb(const ClipFrameInput &frame, const SoftwareVp8WebmOptions &options) {
  return CropToEven(preview::DemosaicBayerRg8ToFit(
      frame.frame.payload,
      {.width = frame.frame.metadata.width, .height = frame.frame.metadata.height},
      {.width = options.maximum_width, .height = options.maximum_height}));
}

void ConvertRgbToI420(const image::Rgb8Image &rgb, vpx_image_t &i420) {
  if (rgb.width != i420.d_w || rgb.height != i420.d_h || (rgb.width & 1U) != 0U ||
      (rgb.height & 1U) != 0U) {
    throw std::invalid_argument("RGB and I420 clip frame dimensions do not match");
  }
  const auto luma_stride = i420.stride[VPX_PLANE_Y];
  const auto chroma_u_stride = i420.stride[VPX_PLANE_U];
  const auto chroma_v_stride = i420.stride[VPX_PLANE_V];
  if (std::cmp_less(luma_stride, rgb.width) || std::cmp_less(chroma_u_stride, rgb.width / 2U) ||
      std::cmp_less(chroma_v_stride, rgb.width / 2U)) {
    throw std::runtime_error("libvpx allocated invalid I420 plane strides");
  }
  const auto luma_stride_bytes = static_cast<std::size_t>(luma_stride);
  const auto chroma_u_stride_bytes = static_cast<std::size_t>(chroma_u_stride);
  const auto chroma_v_stride_bytes = static_cast<std::size_t>(chroma_v_stride);

  // libvpx exposes strided C plane pointers. Bounds are established by the
  // matching dimensions and vpx_img_alloc immediately above this boundary.
  // Initialize every stride byte, not just visible pixels: libvpx may inspect
  // row padding while filtering an edge, and allocator residue would make the
  // deterministic software fixture output depend on process scheduling.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (std::uint32_t y = 0; y < i420.h; ++y) {
    auto *luma_row = i420.planes[VPX_PLANE_Y] + static_cast<std::size_t>(y) * luma_stride_bytes;
    std::fill_n(luma_row, luma_stride_bytes, std::uint8_t{16});
  }
  const std::uint32_t chroma_height =
      (i420.h + (1U << i420.y_chroma_shift) - 1U) >> i420.y_chroma_shift;
  for (std::uint32_t y = 0; y < chroma_height; ++y) {
    auto *u_row = i420.planes[VPX_PLANE_U] + static_cast<std::size_t>(y) * chroma_u_stride_bytes;
    auto *v_row = i420.planes[VPX_PLANE_V] + static_cast<std::size_t>(y) * chroma_v_stride_bytes;
    std::fill_n(u_row, chroma_u_stride_bytes, std::uint8_t{128});
    std::fill_n(v_row, chroma_v_stride_bytes, std::uint8_t{128});
  }

  for (std::uint32_t y = 0; y < rgb.height; ++y) {
    auto *luma_row = i420.planes[VPX_PLANE_Y] + static_cast<std::size_t>(y) * luma_stride_bytes;
    for (std::uint32_t x = 0; x < rgb.width; ++x) {
      const std::size_t pixel = (static_cast<std::size_t>(y) * rgb.width + x) * 3U;
      luma_row[x] = LimitedLuma(rgb.pixels[pixel], rgb.pixels[pixel + 1U], rgb.pixels[pixel + 2U]);
    }
  }

  for (std::uint32_t y = 0; y < rgb.height; y += 2U) {
    auto *u_row =
        i420.planes[VPX_PLANE_U] + static_cast<std::size_t>(y / 2U) * chroma_u_stride_bytes;
    auto *v_row =
        i420.planes[VPX_PLANE_V] + static_cast<std::size_t>(y / 2U) * chroma_v_stride_bytes;
    for (std::uint32_t x = 0; x < rgb.width; x += 2U) {
      std::uint32_t red = 0;
      std::uint32_t green = 0;
      std::uint32_t blue = 0;
      for (std::uint32_t offset_y = 0; offset_y < 2U; ++offset_y) {
        for (std::uint32_t offset_x = 0; offset_x < 2U; ++offset_x) {
          const std::size_t pixel =
              (static_cast<std::size_t>(y + offset_y) * rgb.width + x + offset_x) * 3U;
          red += rgb.pixels[pixel];
          green += rgb.pixels[pixel + 1U];
          blue += rgb.pixels[pixel + 2U];
        }
      }
      red = (red + 2U) / 4U;
      green = (green + 2U) / 4U;
      blue = (blue + 2U) / 4U;
      u_row[x / 2U] = LimitedChromaU(red, green, blue);
      v_row[x / 2U] = LimitedChromaV(red, green, blue);
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

std::vector<std::uint64_t> MediaTimesMicroseconds(const CameraClipInput &input) {
  std::vector<std::uint64_t> media_times;
  media_times.reserve(input.frames.size());
  const std::chrono::nanoseconds first = input.frames.front().time_from_impact;
  for (const ClipFrameInput &frame : input.frames) {
    const auto rounded =
        std::chrono::round<std::chrono::microseconds>(frame.time_from_impact - first).count();
    if (!std::in_range<std::uint64_t>(rounded)) {
      throw std::invalid_argument("clip media time is negative or exceeds uint64");
    }
    media_times.push_back(static_cast<std::uint64_t>(rounded));
    if (media_times.size() > 1U && media_times.back() <= media_times[media_times.size() - 2U]) {
      throw std::invalid_argument("clip media times must increase at microsecond precision");
    }
  }
  return media_times;
}

std::uint64_t MedianDurationMicroseconds(std::span<const std::uint64_t> media_times) {
  std::vector<std::uint64_t> durations;
  durations.reserve(media_times.size() - 1U);
  for (std::size_t index = 1; index < media_times.size(); ++index) {
    durations.push_back(media_times[index] - media_times[index - 1U]);
  }
  std::ranges::sort(durations);
  const std::size_t middle = durations.size() / 2U;
  if ((durations.size() & 1U) != 0U) {
    return durations[middle];
  }
  return durations[middle - 1U] + (durations[middle] - durations[middle - 1U]) / 2U;
}

struct EncodedGeometry {
  std::uint32_t width;
  std::uint32_t height;
};

struct FrameCadence {
  std::uint64_t median_duration_us;
};

struct StreamFinishOptions {
  std::size_t expected_frames;
  std::uint64_t duration_us;
};

struct OutputExpectations {
  EncodedGeometry geometry;
  std::size_t frame_count;
  std::uint64_t last_media_time_us;
};

vpx_codec_enc_cfg_t BuildVpxConfig(EncodedGeometry geometry,
                                   const SoftwareVp8WebmOptions &options) {
  // vpx_codec_enc_config_default initializes every field, including enum
  // fields whose valid values deliberately do not contain zero.
  vpx_codec_enc_cfg_t config;
  const vpx_codec_err_t result = vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &config, 0);
  if (result != VPX_CODEC_OK) {
    throw std::runtime_error(std::string("libvpx default configuration failed: ") +
                             vpx_codec_err_to_string(result));
  }
  config.g_w = geometry.width;
  config.g_h = geometry.height;
  config.g_timebase.num = 1;
  config.g_timebase.den = static_cast<int>(kMicrosecondsPerSecond);
  config.g_threads = 1;
  config.g_lag_in_frames = 0;
  config.g_pass = VPX_RC_ONE_PASS;
  config.kf_mode = VPX_KF_DISABLED;
  config.rc_end_usage = VPX_VBR;
  config.rc_target_bitrate = options.target_bitrate_kbps;
  config.rc_min_quantizer = options.minimum_quantizer;
  config.rc_max_quantizer = options.maximum_quantizer;
  config.rc_dropframe_thresh = 0;
  config.rc_resize_allowed = 0;
  return config;
}

std::uint64_t ConfigureWebm(mkvmuxer::Segment &segment, mkvmuxer::MkvWriter &writer,
                            EncodedGeometry geometry, FrameCadence cadence) {
  if (!segment.Init(&writer)) {
    throw std::runtime_error("could not initialize WebM muxer");
  }
  segment.set_mode(mkvmuxer::Segment::kFile);
  segment.OutputCues(true);
  segment.AccurateClusterDuration(true);
  segment.GetSegmentInfo()->set_timecode_scale(kWebmTimecodeScaleNanoseconds);
  segment.GetSegmentInfo()->set_muxing_app("swing_capture");
  segment.GetSegmentInfo()->set_writing_app("swing_capture/libvpx");
  const std::uint64_t track_number = segment.AddVideoTrack(
      static_cast<std::int32_t>(geometry.width), static_cast<std::int32_t>(geometry.height), 1);
  auto *track = dynamic_cast<mkvmuxer::VideoTrack *>(segment.GetTrackByNumber(track_number));
  if (track_number == 0U || track == nullptr) {
    throw std::runtime_error("could not add WebM video track");
  }
  track->set_uid(1);
  track->set_default_duration(cadence.median_duration_us * kWebmTimecodeScaleNanoseconds);
  track->set_frame_rate(static_cast<double>(kMicrosecondsPerSecond) /
                        static_cast<double>(cadence.median_duration_us));
  return track_number;
}

class Vp8WebmStream final {
 public:
  Vp8WebmStream(const std::filesystem::path &path, EncodedGeometry geometry, FrameCadence cadence,
                const SoftwareVp8WebmOptions &options, ClipViewPipelineProfile &profile)
      : image_(geometry.width, geometry.height), geometry_(geometry), profile_(profile) {
    const vpx_codec_enc_cfg_t config = BuildVpxConfig(geometry, options);
    if (vpx_codec_enc_init(codec_.get(), vpx_codec_vp8_cx(), &config, 0) != VPX_CODEC_OK) {
      throw std::runtime_error(CodecError(codec_.value(), "libvpx initialization failed"));
    }
    codec_.MarkInitialized();
    if (vpx_codec_control(codec_.get(), VP8E_SET_CPUUSED, options.cpu_used) != VPX_CODEC_OK) {
      throw std::runtime_error(CodecError(codec_.value(), "libvpx CPU-speed setup failed"));
    }
    const std::string path_string = path.string();
    if (!writer_.Open(path_string.c_str())) {
      throw std::runtime_error("could not open WebM output: " + path_string);
    }
    track_number_ = ConfigureWebm(segment_, writer_, geometry, cadence);
  }

  ~Vp8WebmStream() = default;

  Vp8WebmStream(const Vp8WebmStream &) = delete;
  Vp8WebmStream &operator=(const Vp8WebmStream &) = delete;
  Vp8WebmStream(Vp8WebmStream &&) = delete;
  Vp8WebmStream &operator=(Vp8WebmStream &&) = delete;

  void EncodeFrame(const image::Rgb8Image &rgb, std::uint64_t timestamp_us,
                   std::uint64_t duration_us) {
    if (rgb.width != geometry_.width || rgb.height != geometry_.height) {
      throw std::invalid_argument("clip source geometry produced inconsistent encoded size");
    }
    if (!std::in_range<vpx_codec_pts_t>(timestamp_us) ||
        duration_us > std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error("clip timestamp or duration exceeds libvpx range");
    }
    // The libvpx C API specifies unsigned long for frame duration. We apply a
    // portable uint32 bound above before crossing that vendor API boundary.
    // NOLINTBEGIN(google-runtime-int)
    const auto conversion_started = ProfileClock::now();
    ConvertRgbToI420(rgb, *image_.get());
    profile_.rgb_to_yuv420_ms += ElapsedMilliseconds(conversion_started);
    const auto vpx_duration = static_cast<unsigned long>(duration_us);
    // NOLINTEND(google-runtime-int)
    const auto encode_started = ProfileClock::now();
    if (vpx_codec_encode(codec_.get(), image_.get(), static_cast<vpx_codec_pts_t>(timestamp_us),
                         vpx_duration, VPX_EFLAG_FORCE_KF, VPX_DL_BEST_QUALITY) != VPX_CODEC_OK) {
      throw std::runtime_error(CodecError(codec_.value(), "libvpx frame encoding failed"));
    }
    profile_.codec_encode_ms += ElapsedMilliseconds(encode_started);
    static_cast<void>(TimedDrainPackets());
  }

  void Finish(StreamFinishOptions options) {
    while (EncodeFlushPacket()) {
    }
    if (packet_count_ != options.expected_frames || keyframe_count_ != packet_count_) {
      throw std::runtime_error("libvpx all-intra output count does not match clip input");
    }
    const auto finalize_started = ProfileClock::now();
    segment_.set_duration(static_cast<double>(options.duration_us));
    if (!segment_.Finalize()) {
      throw std::runtime_error("could not finalize WebM clip");
    }
    writer_.Close();
    profile_.finalize_ms += ElapsedMilliseconds(finalize_started);
  }

 private:
  bool EncodeFlushPacket() {
    const auto encode_started = ProfileClock::now();
    if (vpx_codec_encode(codec_.get(), nullptr, 0, 0, 0, VPX_DL_BEST_QUALITY) != VPX_CODEC_OK) {
      throw std::runtime_error(CodecError(codec_.value(), "libvpx flush failed"));
    }
    profile_.codec_encode_ms += ElapsedMilliseconds(encode_started);
    return TimedDrainPackets();
  }

  bool TimedDrainPackets() {
    const auto mux_started = ProfileClock::now();
    const bool saw_packet = DrainPackets();
    profile_.webm_mux_ms += ElapsedMilliseconds(mux_started);
    return saw_packet;
  }

  bool DrainPackets() {
    bool saw_packet = false;
    vpx_codec_iter_t iterator = nullptr;
    while (const vpx_codec_cx_pkt_t *packet = vpx_codec_get_cx_data(codec_.get(), &iterator)) {
      if (packet->kind != VPX_CODEC_CX_FRAME_PKT) {
        continue;
      }
      // vpx_codec_cx_pkt_t is a tagged C union. The kind check above makes the
      // frame member active for every access in this block.
      // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
      if (packet->data.frame.pts < 0 || !std::in_range<std::uint64_t>(packet->data.frame.pts)) {
        throw std::runtime_error("libvpx emitted an invalid frame timestamp");
      }
      const auto timestamp_us = static_cast<std::uint64_t>(packet->data.frame.pts);
      const bool is_key = (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0U;
      if (packet_count_ > 0U) {
        segment_.ForceNewClusterOnNextFrame();
      }
      if (!segment_.AddFrame(static_cast<const std::uint8_t *>(packet->data.frame.buf),
                             packet->data.frame.sz, track_number_,
                             timestamp_us * kWebmTimecodeScaleNanoseconds, is_key)) {
        throw std::runtime_error("could not mux VP8 frame into WebM");
      }
      // NOLINTEND(cppcoreguidelines-pro-type-union-access)
      ++packet_count_;
      keyframe_count_ += static_cast<std::size_t>(is_key);
      saw_packet = true;
    }
    return saw_packet;
  }

  CodecGuard codec_;
  ImageGuard image_;
  mkvmuxer::MkvWriter writer_;
  mkvmuxer::Segment segment_;
  EncodedGeometry geometry_;
  std::uint64_t track_number_ = 0;
  std::size_t packet_count_ = 0;
  std::size_t keyframe_count_ = 0;
  ClipViewPipelineProfile &profile_;
};

std::uint64_t FrameDuration(std::span<const std::uint64_t> media_times, std::size_t index,
                            FrameCadence cadence) {
  if (index + 1U == media_times.size()) {
    return cadence.median_duration_us;
  }
  return media_times[index + 1U] - media_times[index];
}

EncodedClipMedia VerifyOutput(const std::filesystem::path &path,
                              const OutputExpectations &expected) {
  const WebmInspection inspection = InspectWebm(path);
  const auto expected_last_time =
      std::chrono::microseconds(static_cast<std::int64_t>(expected.last_media_time_us));
  if (inspection.codec != "vp8" || inspection.width != expected.geometry.width ||
      inspection.height != expected.geometry.height ||
      inspection.frame_count != expected.frame_count ||
      inspection.keyframe_count != expected.frame_count ||
      inspection.first_frame_time != std::chrono::nanoseconds::zero() ||
      inspection.last_frame_time != expected_last_time) {
    throw std::runtime_error("post-write WebM verification did not match encoded clip");
  }
  return {
      .width = inspection.width,
      .height = inspection.height,
      .frame_count = inspection.frame_count,
      .keyframe_count = inspection.keyframe_count,
      .encoded_bytes = std::filesystem::file_size(path),
      .pipeline_profile = {},
  };
}

class SoftwareVp8WebmEncoder final : public ClipMediaEncoder {
 public:
  explicit SoftwareVp8WebmEncoder(SoftwareVp8WebmOptions options) : options_(options) {}

  [[nodiscard]] ClipMediaEncoderCapabilities capabilities() const override {
    return {
        .encoder_id = "libvpx-vp8-all-intra-v1",
        .file_extension = "webm",
        .mime_type = "video/webm",
        .codec = "vp8",
        .hardware_accelerated = false,
        .deterministic = true,
        .all_frames_keyframes = true,
    };
  }

  [[nodiscard]] EncodedClipMedia Encode(const CameraClipInput &input,
                                        const std::filesystem::path &output_path) const override {
    // libvpx performs process-global run-time dispatch initialization. Keep
    // first use and subsequent software fixture encodes serialized so two
    // publication tasks cannot race that vendor boundary. Production's
    // independent VA-API contexts remain concurrent.
    const std::scoped_lock encode_lock(encode_mutex_);
    const auto total_started = ProfileClock::now();
    if (input.frames.size() < 2U) {
      throw std::invalid_argument("VP8 clip encoding requires at least two frames");
    }
    ClipViewPipelineProfile profile{
        .role = std::string(input.role),
        .frame_count = input.frames.size(),
    };
    const std::vector<std::uint64_t> media_times = MediaTimesMicroseconds(input);
    const FrameCadence cadence{.median_duration_us = MedianDurationMicroseconds(media_times)};
    auto demosaic_started = ProfileClock::now();
    const image::Rgb8Image first_rgb = PrepareRgb(input.frames.front(), options_);
    profile.bayer_fit_demosaic_ms += ElapsedMilliseconds(demosaic_started);
    const EncodedGeometry geometry{.width = first_rgb.width, .height = first_rgb.height};
    Vp8WebmStream stream(output_path, geometry, cadence, options_, profile);
    stream.EncodeFrame(first_rgb, media_times.front(), FrameDuration(media_times, 0, cadence));
    for (std::size_t index = 1; index < input.frames.size(); ++index) {
      demosaic_started = ProfileClock::now();
      const image::Rgb8Image rgb = PrepareRgb(input.frames[index], options_);
      profile.bayer_fit_demosaic_ms += ElapsedMilliseconds(demosaic_started);
      stream.EncodeFrame(rgb, media_times[index], FrameDuration(media_times, index, cadence));
    }
    stream.Finish({.expected_frames = input.frames.size(),
                   .duration_us = media_times.back() + cadence.median_duration_us});
    const auto verification_started = ProfileClock::now();
    EncodedClipMedia encoded =
        VerifyOutput(output_path, {.geometry = geometry,
                                   .frame_count = input.frames.size(),
                                   .last_media_time_us = media_times.back()});
    profile.output_verification_ms = ElapsedMilliseconds(verification_started);
    profile.total_ms = ElapsedMilliseconds(total_started);
    encoded.pipeline_profile = std::move(profile);
    return encoded;
  }

 private:
  SoftwareVp8WebmOptions options_;
  mutable std::mutex encode_mutex_;
};

void ValidateOptions(const SoftwareVp8WebmOptions &options) {
  if (options.maximum_width < 2U || options.maximum_height < 2U) {
    throw std::invalid_argument("clip encoder maximum dimensions must both be at least two");
  }
  if (options.target_bitrate_kbps == 0U) {
    throw std::invalid_argument("clip encoder target bitrate must be positive");
  }
  if (options.minimum_quantizer > options.maximum_quantizer || options.maximum_quantizer > 63U) {
    throw std::invalid_argument("clip encoder quantizers must satisfy 0 <= min <= max <= 63");
  }
  if (options.cpu_used < -5 || options.cpu_used > 5) {
    throw std::invalid_argument("deterministic software encoder cpu_used must be in [-5, 5]");
  }
}

}  // namespace

std::unique_ptr<ClipMediaEncoder> MakeSoftwareVp8WebmEncoder(SoftwareVp8WebmOptions options) {
  ValidateOptions(options);
  return std::make_unique<SoftwareVp8WebmEncoder>(options);
}

}  // namespace swing_capture::encoding
