#include <fcntl.h>
#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>
#include <unistd.h>
// libva's public C ABI intentionally uses flexible arrays and anonymous
// structs. Keep the warning waiver at this pinned vendor-header boundary; all
// first-party code below remains under the repository's pedantic policy.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <va/drm/va_drm.h>
#include <va/va.h>
#include <va/va_enc_vp9.h>
#pragma GCC diagnostic pop

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "capture/encoding/clip_session.h"
#include "capture/image/bayer_rg8.h"
#include "capture/preview/preview_image.h"

namespace swing_capture::encoding {
namespace {

using ProfileClock = std::chrono::steady_clock;
constexpr std::uint64_t kWebmTimecodeScaleNanoseconds = 1'000;
constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

double ElapsedMilliseconds(ProfileClock::time_point started) {
  return std::chrono::duration<double, std::milli>(ProfileClock::now() - started).count();
}

class SharedLibrary final {
 public:
  explicit SharedLibrary(const char *name) : handle_(dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
    if (handle_ == nullptr) {
      const char *const detail = dlerror();  // NOLINT(concurrency-mt-unsafe)
      throw std::runtime_error(std::string("could not load ") + name + ": " +
                               (detail == nullptr ? "unknown loader error" : detail));
    }
  }

  ~SharedLibrary() {
    if (handle_ != nullptr) {
      static_cast<void>(dlclose(handle_));
    }
  }

  SharedLibrary(const SharedLibrary &) = delete;
  SharedLibrary &operator=(const SharedLibrary &) = delete;
  SharedLibrary(SharedLibrary &&) = delete;
  SharedLibrary &operator=(SharedLibrary &&) = delete;

  template <typename FunctionPointer>
  [[nodiscard]] FunctionPointer Symbol(const char *name) const {
    static_assert(sizeof(FunctionPointer) == sizeof(void *));
    dlerror();  // NOLINT(concurrency-mt-unsafe)
    const void *const symbol = dlsym(handle_, name);
    const char *const detail = dlerror();  // NOLINT(concurrency-mt-unsafe)
    if (detail != nullptr || symbol == nullptr) {
      throw std::runtime_error(std::string("could not resolve ") + name + ": " +
                               (detail == nullptr ? "missing symbol" : detail));
    }
    // POSIX specifies that dlsym results can be converted to the corresponding
    // function pointer; ISO C++ has no equivalent portable loader primitive.
    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
    return std::bit_cast<FunctionPointer>(symbol);
  }

 private:
  void *handle_ = nullptr;
};

struct VaApi final {
  VaApi()
      : va_library("libva.so.2"),
        drm_library("libva-drm.so.2"),
        get_display_drm(drm_library.Symbol<decltype(&vaGetDisplayDRM)>("vaGetDisplayDRM")),
        initialize(va_library.Symbol<decltype(&vaInitialize)>("vaInitialize")),
        terminate(va_library.Symbol<decltype(&vaTerminate)>("vaTerminate")),
        error_string(va_library.Symbol<decltype(&vaErrorStr)>("vaErrorStr")),
        max_num_entrypoints(
            va_library.Symbol<decltype(&vaMaxNumEntrypoints)>("vaMaxNumEntrypoints")),
        query_config_entrypoints(
            va_library.Symbol<decltype(&vaQueryConfigEntrypoints)>("vaQueryConfigEntrypoints")),
        get_config_attributes(
            va_library.Symbol<decltype(&vaGetConfigAttributes)>("vaGetConfigAttributes")),
        create_config(va_library.Symbol<decltype(&vaCreateConfig)>("vaCreateConfig")),
        destroy_config(va_library.Symbol<decltype(&vaDestroyConfig)>("vaDestroyConfig")),
        create_surfaces(va_library.Symbol<decltype(&vaCreateSurfaces)>("vaCreateSurfaces")),
        destroy_surfaces(va_library.Symbol<decltype(&vaDestroySurfaces)>("vaDestroySurfaces")),
        create_context(va_library.Symbol<decltype(&vaCreateContext)>("vaCreateContext")),
        destroy_context(va_library.Symbol<decltype(&vaDestroyContext)>("vaDestroyContext")),
        create_buffer(va_library.Symbol<decltype(&vaCreateBuffer)>("vaCreateBuffer")),
        destroy_buffer(va_library.Symbol<decltype(&vaDestroyBuffer)>("vaDestroyBuffer")),
        map_buffer(va_library.Symbol<decltype(&vaMapBuffer)>("vaMapBuffer")),
        unmap_buffer(va_library.Symbol<decltype(&vaUnmapBuffer)>("vaUnmapBuffer")),
        begin_picture(va_library.Symbol<decltype(&vaBeginPicture)>("vaBeginPicture")),
        render_picture(va_library.Symbol<decltype(&vaRenderPicture)>("vaRenderPicture")),
        end_picture(va_library.Symbol<decltype(&vaEndPicture)>("vaEndPicture")),
        sync_surface(va_library.Symbol<decltype(&vaSyncSurface)>("vaSyncSurface")),
        derive_image(va_library.Symbol<decltype(&vaDeriveImage)>("vaDeriveImage")),
        destroy_image(va_library.Symbol<decltype(&vaDestroyImage)>("vaDestroyImage")) {}

  SharedLibrary va_library;
  SharedLibrary drm_library;
  decltype(&vaGetDisplayDRM) get_display_drm;
  decltype(&vaInitialize) initialize;
  decltype(&vaTerminate) terminate;
  decltype(&vaErrorStr) error_string;
  decltype(&vaMaxNumEntrypoints) max_num_entrypoints;
  decltype(&vaQueryConfigEntrypoints) query_config_entrypoints;
  decltype(&vaGetConfigAttributes) get_config_attributes;
  decltype(&vaCreateConfig) create_config;
  decltype(&vaDestroyConfig) destroy_config;
  decltype(&vaCreateSurfaces) create_surfaces;
  decltype(&vaDestroySurfaces) destroy_surfaces;
  decltype(&vaCreateContext) create_context;
  decltype(&vaDestroyContext) destroy_context;
  decltype(&vaCreateBuffer) create_buffer;
  decltype(&vaDestroyBuffer) destroy_buffer;
  decltype(&vaMapBuffer) map_buffer;
  decltype(&vaUnmapBuffer) unmap_buffer;
  decltype(&vaBeginPicture) begin_picture;
  decltype(&vaRenderPicture) render_picture;
  decltype(&vaEndPicture) end_picture;
  decltype(&vaSyncSurface) sync_surface;
  decltype(&vaDeriveImage) derive_image;
  decltype(&vaDestroyImage) destroy_image;
};

void RequireVa(const VaApi &api, VAStatus status, std::string_view operation) {
  if (status != VA_STATUS_SUCCESS) {
    const char *const detail = api.error_string(status);
    throw std::runtime_error(std::string(operation) + ": " +
                             (detail == nullptr ? "unknown VA-API error" : detail));
  }
}

class FileDescriptor final {
 public:
  explicit FileDescriptor(const std::filesystem::path &path) {
    const std::string path_string = path.string();
    value_ = open(path_string.c_str(), O_RDWR | O_CLOEXEC);
    if (value_ < 0) {
      throw std::runtime_error("could not open VA-API render node: " + path_string);
    }
  }
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(close(value_));
    }
  }

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&) = delete;
  FileDescriptor &operator=(FileDescriptor &&) = delete;

  [[nodiscard]] int value() const noexcept { return value_; }

 private:
  int value_ = -1;
};

struct EncodedGeometry {
  std::uint32_t width;
  std::uint32_t height;
};

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
      .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3U)};
  const std::size_t source_row_bytes = static_cast<std::size_t>(source.width) * 3U;
  const std::size_t output_row_bytes = static_cast<std::size_t>(width) * 3U;
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto source_row =
        source.pixels.begin() + static_cast<std::ptrdiff_t>(y * source_row_bytes);
    const auto output_row =
        output.pixels.begin() + static_cast<std::ptrdiff_t>(y * output_row_bytes);
    std::ranges::copy_n(source_row, static_cast<std::ptrdiff_t>(output_row_bytes), output_row);
  }
  return output;
}

image::Rgb8Image PrepareRgb(const ClipFrameInput &frame, const VaapiVp9WebmOptions &options) {
  return CropToEven(preview::DemosaicBayerRg8ToFit(
      frame.frame.payload,
      {.width = frame.frame.metadata.width, .height = frame.frame.metadata.height},
      {.width = options.maximum_width, .height = options.maximum_height}));
}

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

struct Nv12Image {
  std::uint32_t width;
  std::uint32_t height;
  std::vector<std::uint8_t> bytes;
};

Nv12Image ConvertRgbToNv12(const image::Rgb8Image &rgb) {
  if ((rgb.width & 1U) != 0U || (rgb.height & 1U) != 0U) {
    throw std::invalid_argument("NV12 clip dimensions must be even");
  }
  const std::size_t luma_size = static_cast<std::size_t>(rgb.width) * rgb.height;
  Nv12Image output{.width = rgb.width,
                   .height = rgb.height,
                   .bytes = std::vector<std::uint8_t>(luma_size + luma_size / 2U)};
  for (std::uint32_t y = 0; y < rgb.height; ++y) {
    for (std::uint32_t x = 0; x < rgb.width; ++x) {
      const std::size_t pixel = (static_cast<std::size_t>(y) * rgb.width + x) * 3U;
      output.bytes[static_cast<std::size_t>(y) * rgb.width + x] =
          LimitedLuma(rgb.pixels[pixel], rgb.pixels[pixel + 1U], rgb.pixels[pixel + 2U]);
    }
  }
  for (std::uint32_t y = 0; y < rgb.height; y += 2U) {
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
      const std::size_t chroma = luma_size + static_cast<std::size_t>(y / 2U) * rgb.width + x;
      output.bytes[chroma] = LimitedChromaU((red + 2U) / 4U, (green + 2U) / 4U, (blue + 2U) / 4U);
      output.bytes[chroma + 1U] =
          LimitedChromaV((red + 2U) / 4U, (green + 2U) / 4U, (blue + 2U) / 4U);
    }
  }
  return output;
}

struct PreparedFrame {
  Nv12Image image;
  double bayer_fit_demosaic_ms = 0.0;
  double rgb_to_nv12_ms = 0.0;
};

PreparedFrame PrepareFrame(const ClipFrameInput &frame, const VaapiVp9WebmOptions &options) {
  const auto demosaic_started = ProfileClock::now();
  const image::Rgb8Image rgb = PrepareRgb(frame, options);
  const double demosaic_ms = ElapsedMilliseconds(demosaic_started);
  const auto conversion_started = ProfileClock::now();
  Nv12Image nv12 = ConvertRgbToNv12(rgb);
  return {
      .image = std::move(nv12),
      .bayer_fit_demosaic_ms = demosaic_ms,
      .rgb_to_nv12_ms = ElapsedMilliseconds(conversion_started),
  };
}

class BoundedFramePreprocessor final {
 public:
  BoundedFramePreprocessor(const CameraClipInput &input, VaapiVp9WebmOptions options,
                           PreparedFrame first)
      : input_(input),
        options_(std::move(options)),
        maximum_in_flight_(std::max<std::size_t>(2U, options_.preprocessing_threads * 2U)),
        slots_(input.frames.size()) {
    slots_.front() = std::move(first);
    workers_.reserve(options_.preprocessing_threads);
    for (std::size_t index = 0; index < options_.preprocessing_threads; ++index) {
      workers_.emplace_back([this](const std::stop_token &stop_token) { Run(stop_token); });
    }
  }

  ~BoundedFramePreprocessor() {
    for (std::jthread &worker : workers_) {
      worker.request_stop();
    }
    changed_.notify_all();
  }

  BoundedFramePreprocessor(const BoundedFramePreprocessor &) = delete;
  BoundedFramePreprocessor &operator=(const BoundedFramePreprocessor &) = delete;
  BoundedFramePreprocessor(BoundedFramePreprocessor &&) = delete;
  BoundedFramePreprocessor &operator=(BoundedFramePreprocessor &&) = delete;

  [[nodiscard]] PreparedFrame Take(std::size_t index) {
    if (index >= slots_.size()) {
      throw std::out_of_range("prepared clip frame index is out of range");
    }
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this, index] { return failure_ != nullptr || slots_[index].has_value(); });
    if (failure_ != nullptr) {
      std::rethrow_exception(failure_);
    }
    if (!slots_[index].has_value()) {
      throw std::logic_error("prepared clip frame notification had no frame");
    }
    PreparedFrame result = std::move(slots_[index]).value_or(PreparedFrame{});
    slots_[index].reset();
    next_to_consume_ = index + 1U;
    lock.unlock();
    changed_.notify_all();
    return result;
  }

 private:
  void Run(const std::stop_token &stop_token) noexcept {
    while (!stop_token.stop_requested()) {
      const std::optional<std::size_t> index = Reserve(stop_token);
      if (!index.has_value()) {
        return;
      }
      try {
        PreparedFrame prepared = PrepareFrame(input_.frames[*index], options_);
        {
          const std::scoped_lock lock(mutex_);
          slots_[*index] = std::move(prepared);
        }
        changed_.notify_all();
      } catch (...) {
        {
          const std::scoped_lock lock(mutex_);
          if (failure_ == nullptr) {
            failure_ = std::current_exception();
          }
        }
        changed_.notify_all();
        return;
      }
    }
  }

  [[nodiscard]] std::optional<std::size_t> Reserve(const std::stop_token &stop_token) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, stop_token, [this] {
      return failure_ != nullptr || next_to_prepare_ >= slots_.size() ||
             next_to_prepare_ < next_to_consume_ + maximum_in_flight_;
    });
    if (stop_token.stop_requested() || failure_ != nullptr || next_to_prepare_ >= slots_.size()) {
      return std::nullopt;
    }
    return next_to_prepare_++;
  }

  const CameraClipInput &input_;
  VaapiVp9WebmOptions options_;
  std::size_t maximum_in_flight_;
  std::vector<std::optional<PreparedFrame>> slots_;
  std::vector<std::jthread> workers_;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::exception_ptr failure_;
  std::size_t next_to_prepare_ = 1;
  std::size_t next_to_consume_ = 0;
};

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

class VaEncoderContext final {
 public:
  explicit VaEncoderContext(const VaApi &api) : api_(api) {}
  ~VaEncoderContext() { Cleanup(); }

  VaEncoderContext(const VaEncoderContext &) = delete;
  VaEncoderContext &operator=(const VaEncoderContext &) = delete;
  VaEncoderContext(VaEncoderContext &&) = delete;
  VaEncoderContext &operator=(VaEncoderContext &&) = delete;

  struct PendingFrame {
    std::size_t surface_index;
    VABufferID coded_buffer;
  };

  struct SubmitOptions {
    std::uint32_t quality_index;
    std::size_t surface_index;
  };

  void Initialize(int render_fd, EncodedGeometry geometry, std::size_t queue_depth) {
    display_ = api_.get_display_drm(render_fd);
    if (display_ == nullptr) {
      throw std::runtime_error("vaGetDisplayDRM returned no display");
    }
    int major = 0;
    int minor = 0;
    RequireVa(api_, api_.initialize(display_, &major, &minor), "vaInitialize");
    initialized_ = true;
    RequireEntrypoint();
    CreateConfig();
    CreateResources(geometry, queue_depth);
  }

  void Upload(const Nv12Image &nv12, std::size_t surface_index) const {
    const SurfacePair &surfaces = SurfaceAt(surface_index);
    RequireVa(api_, api_.sync_surface(display_, surfaces.input), "vaSyncSurface before upload");
    VAImage image{};
    RequireVa(api_, api_.derive_image(display_, surfaces.input, &image), "vaDeriveImage");
    try {
      UploadMappedImage(nv12, image);
    } catch (...) {
      static_cast<void>(api_.destroy_image(display_, image.image_id));
      throw;
    }
    RequireVa(api_, api_.destroy_image(display_, image.image_id), "vaDestroyImage");
  }

  [[nodiscard]] PendingFrame SubmitKeyframe(SubmitOptions options) {
    const SurfacePair &surfaces = SurfaceAt(options.surface_index);
    const std::size_t coded_capacity = std::max<std::size_t>(
        1U << 20U, static_cast<std::size_t>(geometry_.width) * geometry_.height * 4U);
    if (!std::in_range<unsigned int>(coded_capacity)) {
      throw std::overflow_error("VA-API coded buffer size exceeds unsigned int");
    }
    VABufferID coded_buffer = VA_INVALID_ID;
    RequireVa(
        api_,
        api_.create_buffer(display_, context_, VAEncCodedBufferType,
                           static_cast<unsigned int>(coded_capacity), 1, nullptr, &coded_buffer),
        "vaCreateBuffer coded data");
    try {
      const std::array<VABufferID, 3> parameters =
          CreateFrameParameters({.coded_buffer = coded_buffer,
                                 .reconstructed_surface = surfaces.reconstructed,
                                 .quality_index = options.quality_index});
      try {
        Submit(parameters, surfaces.input);
      } catch (...) {
        DestroyBuffers(parameters);
        throw;
      }
      DestroyBuffers(parameters);
      pending_coded_buffers_.push_back(coded_buffer);
      return {.surface_index = options.surface_index, .coded_buffer = coded_buffer};
    } catch (...) {
      static_cast<void>(api_.destroy_buffer(display_, coded_buffer));
      throw;
    }
  }

  [[nodiscard]] std::vector<std::uint8_t> Complete(PendingFrame pending) {
    const SurfacePair &surfaces = SurfaceAt(pending.surface_index);
    RequireVa(api_, api_.sync_surface(display_, surfaces.input), "vaSyncSurface encoded frame");
    try {
      std::vector<std::uint8_t> output = CopyCodedBuffer(pending.coded_buffer);
      DestroyPendingBuffer(pending.coded_buffer);
      return output;
    } catch (...) {
      DiscardPendingBuffer(pending.coded_buffer);
      throw;
    }
  }

 private:
  struct SurfacePair {
    VASurfaceID input;
    VASurfaceID reconstructed;
  };

  void RequireEntrypoint() const {
    const int maximum = api_.max_num_entrypoints(display_);
    if (maximum <= 0) {
      throw std::runtime_error("VA-API reports no entrypoint capacity");
    }
    std::vector<VAEntrypoint> entrypoints(static_cast<std::size_t>(maximum));
    int count = maximum;
    RequireVa(
        api_,
        api_.query_config_entrypoints(display_, VAProfileVP9Profile0, entrypoints.data(), &count),
        "vaQueryConfigEntrypoints VP9");
    if (count < 0 || count > maximum ||
        !std::ranges::contains(std::span(entrypoints).first(static_cast<std::size_t>(count)),
                               VAEntrypointEncSliceLP)) {
      throw std::runtime_error("VA-API driver does not expose VP9 low-power encoding");
    }
  }

  void CreateConfig() {
    std::array<VAConfigAttrib, 2> attributes{{
        {.type = VAConfigAttribRTFormat, .value = 0},
        {.type = VAConfigAttribRateControl, .value = 0},
    }};
    RequireVa(api_,
              api_.get_config_attributes(display_, VAProfileVP9Profile0, VAEntrypointEncSliceLP,
                                         attributes.data(), static_cast<int>(attributes.size())),
              "vaGetConfigAttributes VP9");
    if ((attributes[0].value & VA_RT_FORMAT_YUV420) == 0U ||
        (attributes[1].value & VA_RC_CQP) == 0U) {
      throw std::runtime_error("VA-API VP9 encoder lacks YUV420 CQP support");
    }
    attributes[0].value = VA_RT_FORMAT_YUV420;
    attributes[1].value = VA_RC_CQP;
    RequireVa(api_,
              api_.create_config(display_, VAProfileVP9Profile0, VAEntrypointEncSliceLP,
                                 attributes.data(), static_cast<int>(attributes.size()), &config_),
              "vaCreateConfig VP9");
  }

  void CreateResources(EncodedGeometry geometry, std::size_t queue_depth) {
    geometry_ = geometry;
    VASurfaceAttrib pixel_format{
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value = {.i = VA_FOURCC_NV12}},
    };
    std::vector<VASurfaceID> surfaces(queue_depth * 2U, VA_INVALID_ID);
    RequireVa(api_,
              api_.create_surfaces(display_, VA_RT_FORMAT_YUV420, geometry.width, geometry.height,
                                   surfaces.data(), static_cast<unsigned int>(surfaces.size()),
                                   &pixel_format, 1),
              "vaCreateSurfaces VP9");
    surfaces_.reserve(queue_depth);
    for (std::size_t index = 0; index < queue_depth; ++index) {
      surfaces_.push_back(
          {.input = surfaces[index * 2U], .reconstructed = surfaces[index * 2U + 1U]});
    }
    RequireVa(api_,
              api_.create_context(display_, config_, static_cast<int>(geometry.width),
                                  static_cast<int>(geometry.height), VA_PROGRESSIVE,
                                  surfaces.data(), static_cast<int>(surfaces.size()), &context_),
              "vaCreateContext VP9");
  }

  void UploadMappedImage(const Nv12Image &nv12, const VAImage &image) const {
    if (image.format.fourcc != VA_FOURCC_NV12 || image.width < nv12.width ||
        image.height < nv12.height) {
      throw std::runtime_error("VA-API input surface is not compatible NV12");
    }
    void *mapped = nullptr;
    RequireVa(api_, api_.map_buffer(display_, image.buf, &mapped), "vaMapBuffer input image");
    try {
      // The VA-API C ABI exposes mapped planes as a base pointer plus byte
      // offsets and pitches. Bounds are established by the validated image
      // geometry and NV12 surface allocation above.
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const auto *source = nv12.bytes.data();
      auto *destination = static_cast<std::uint8_t *>(mapped);
      for (std::uint32_t y = 0; y < nv12.height; ++y) {
        std::memcpy(destination + image.offsets[0] + static_cast<std::size_t>(y) * image.pitches[0],
                    source + static_cast<std::size_t>(y) * nv12.width, nv12.width);
      }
      const std::size_t source_chroma = static_cast<std::size_t>(nv12.width) * nv12.height;
      for (std::uint32_t y = 0; y < nv12.height / 2U; ++y) {
        std::memcpy(destination + image.offsets[1] + static_cast<std::size_t>(y) * image.pitches[1],
                    source + source_chroma + static_cast<std::size_t>(y) * nv12.width, nv12.width);
      }
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    } catch (...) {
      static_cast<void>(api_.unmap_buffer(display_, image.buf));
      throw;
    }
    RequireVa(api_, api_.unmap_buffer(display_, image.buf), "vaUnmapBuffer input image");
  }

  struct FrameParameterOptions {
    VABufferID coded_buffer;
    VASurfaceID reconstructed_surface;
    std::uint32_t quality_index;
  };

  [[nodiscard]] std::array<VABufferID, 3> CreateFrameParameters(
      FrameParameterOptions options) const {
    VAEncSequenceParameterBufferVP9 sequence{
        .max_frame_width = geometry_.width,
        .max_frame_height = geometry_.height,
        .kf_auto = 0,
        .kf_min_dist = 1,
        .kf_max_dist = 1,
        .bits_per_second = 0,
        .intra_period = 1,
        .va_reserved = {},
    };
    VAEncPictureParameterBufferVP9 picture{};
    picture.frame_width_src = geometry_.width;
    picture.frame_height_src = geometry_.height;
    picture.frame_width_dst = geometry_.width;
    picture.frame_height_dst = geometry_.height;
    picture.reconstructed_frame = options.reconstructed_surface;
    std::ranges::fill(picture.reference_frames, VA_INVALID_ID);
    picture.coded_buf = options.coded_buffer;
    // These are C API bitfield unions. Zero initialization above establishes
    // the active storage and each write is to the documented VA-API view.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
    picture.ref_flags.bits.force_kf = 1;
    picture.pic_flags.bits.frame_type = 0;
    picture.pic_flags.bits.show_frame = 1;
    picture.pic_flags.bits.error_resilient_mode = 1;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    picture.refresh_frame_flags = 0xff;
    picture.luma_ac_qindex = static_cast<std::uint8_t>(options.quality_index);
    picture.luma_dc_qindex_delta = 1;
    picture.chroma_ac_qindex_delta = 1;
    picture.chroma_dc_qindex_delta = 1;
    picture.filter_level = 16;
    std::ranges::fill(picture.ref_lf_delta, 1);
    std::ranges::fill(picture.mode_lf_delta, 1);
    VAEncMiscParameterTypeVP9PerSegmantParam segments{};

    std::array<VABufferID, 3> buffers{VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    try {
      CreateParameterBuffer(VAEncSequenceParameterBufferType, sequence, buffers[0]);
      CreateParameterBuffer(VAEncPictureParameterBufferType, picture, buffers[1]);
      CreateParameterBuffer(VAQMatrixBufferType, segments, buffers[2]);
    } catch (...) {
      DestroyBuffers(buffers);
      throw;
    }
    return buffers;
  }

  template <typename Parameter>
  void CreateParameterBuffer(VABufferType type, Parameter &parameter, VABufferID &buffer) const {
    static_assert(sizeof(Parameter) <= std::numeric_limits<unsigned int>::max());
    RequireVa(
        api_,
        api_.create_buffer(display_, context_, type, static_cast<unsigned int>(sizeof(Parameter)),
                           1, &parameter, &buffer),
        "vaCreateBuffer VP9 parameter");
  }

  void Submit(const std::array<VABufferID, 3> &parameters, VASurfaceID input_surface) const {
    RequireVa(api_, api_.begin_picture(display_, context_, input_surface), "vaBeginPicture VP9");
    try {
      std::array<VABufferID, 3> mutable_parameters = parameters;
      RequireVa(api_,
                api_.render_picture(display_, context_, mutable_parameters.data(),
                                    static_cast<int>(mutable_parameters.size())),
                "vaRenderPicture VP9");
      RequireVa(api_, api_.end_picture(display_, context_), "vaEndPicture VP9");
    } catch (...) {
      static_cast<void>(api_.end_picture(display_, context_));
      throw;
    }
  }

  [[nodiscard]] std::vector<std::uint8_t> CopyCodedBuffer(VABufferID coded_buffer) const {
    void *mapped = nullptr;
    RequireVa(api_, api_.map_buffer(display_, coded_buffer, &mapped), "vaMapBuffer coded data");
    std::vector<std::uint8_t> output;
    try {
      auto *segment = static_cast<VACodedBufferSegment *>(mapped);
      while (segment != nullptr) {
        if ((segment->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) != 0U) {
          throw std::runtime_error("VA-API VP9 coded buffer overflowed");
        }
        if (segment->bit_offset != 0U) {
          throw std::runtime_error("VA-API VP9 returned a non-byte-aligned frame");
        }
        if ((segment->status & VA_CODED_BUF_STATUS_BAD_BITSTREAM) != 0U) {
          throw std::runtime_error("VA-API VP9 returned a bad bitstream status");
        }
        if (segment->size > 0U) {
          if (segment->buf == nullptr) {
            throw std::runtime_error("VA-API VP9 returned an empty coded segment pointer");
          }
          const std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t *>(segment->buf),
                                                    segment->size);
          output.insert(output.end(), bytes.begin(), bytes.end());
        }
        segment = static_cast<VACodedBufferSegment *>(segment->next);
      }
    } catch (...) {
      static_cast<void>(api_.unmap_buffer(display_, coded_buffer));
      throw;
    }
    RequireVa(api_, api_.unmap_buffer(display_, coded_buffer), "vaUnmapBuffer coded data");
    if (output.empty()) {
      throw std::runtime_error("VA-API VP9 returned no coded frame bytes");
    }
    return output;
  }

  void DestroyBuffers(std::span<const VABufferID> buffers) const noexcept {
    for (const VABufferID buffer : buffers) {
      if (buffer != VA_INVALID_ID) {
        static_cast<void>(api_.destroy_buffer(display_, buffer));
      }
    }
  }

  [[nodiscard]] const SurfacePair &SurfaceAt(std::size_t index) const {
    if (index >= surfaces_.size()) {
      throw std::out_of_range("VA-API surface index is out of range");
    }
    return surfaces_[index];
  }

  void DestroyPendingBuffer(VABufferID buffer) {
    RequireVa(api_, api_.destroy_buffer(display_, buffer), "vaDestroyBuffer coded data");
    ErasePendingBuffer(buffer);
  }

  void DiscardPendingBuffer(VABufferID buffer) noexcept {
    static_cast<void>(api_.destroy_buffer(display_, buffer));
    ErasePendingBuffer(buffer);
  }

  void ErasePendingBuffer(VABufferID buffer) {
    const auto found = std::ranges::find(pending_coded_buffers_, buffer);
    if (found != pending_coded_buffers_.end()) {
      pending_coded_buffers_.erase(found);
    }
  }

  void Cleanup() noexcept {
    if (display_ == nullptr) {
      return;
    }
    for (const VABufferID buffer : pending_coded_buffers_) {
      static_cast<void>(api_.destroy_buffer(display_, buffer));
    }
    pending_coded_buffers_.clear();
    if (context_ != VA_INVALID_ID) {
      static_cast<void>(api_.destroy_context(display_, context_));
    }
    std::vector<VASurfaceID> surfaces;
    surfaces.reserve(surfaces_.size() * 2U);
    for (const SurfacePair &pair : surfaces_) {
      surfaces.push_back(pair.input);
      surfaces.push_back(pair.reconstructed);
    }
    if (!surfaces.empty()) {
      static_cast<void>(
          api_.destroy_surfaces(display_, surfaces.data(), static_cast<int>(surfaces.size())));
    }
    if (config_ != VA_INVALID_ID) {
      static_cast<void>(api_.destroy_config(display_, config_));
    }
    if (initialized_) {
      static_cast<void>(api_.terminate(display_));
    }
  }

  const VaApi &api_;
  VADisplay display_ = nullptr;
  VAConfigID config_ = VA_INVALID_ID;
  VAContextID context_ = VA_INVALID_ID;
  std::vector<SurfacePair> surfaces_;
  std::vector<VABufferID> pending_coded_buffers_;
  EncodedGeometry geometry_{};
  bool initialized_ = false;
};

std::uint64_t ConfigureWebm(mkvmuxer::Segment &segment, mkvmuxer::MkvWriter &writer,
                            EncodedGeometry geometry, std::uint64_t median_duration_us) {
  if (!segment.Init(&writer)) {
    throw std::runtime_error("could not initialize WebM muxer");
  }
  segment.set_mode(mkvmuxer::Segment::kFile);
  segment.OutputCues(true);
  segment.AccurateClusterDuration(true);
  segment.GetSegmentInfo()->set_timecode_scale(kWebmTimecodeScaleNanoseconds);
  segment.GetSegmentInfo()->set_muxing_app("swing_capture");
  segment.GetSegmentInfo()->set_writing_app("swing_capture/vaapi-vp9");
  const std::uint64_t track_number = segment.AddVideoTrack(
      static_cast<std::int32_t>(geometry.width), static_cast<std::int32_t>(geometry.height), 1);
  auto *track = dynamic_cast<mkvmuxer::VideoTrack *>(segment.GetTrackByNumber(track_number));
  if (track_number == 0U || track == nullptr) {
    throw std::runtime_error("could not add WebM video track");
  }
  track->set_uid(1);
  track->set_codec_id("V_VP9");
  track->set_default_duration(median_duration_us * kWebmTimecodeScaleNanoseconds);
  track->set_frame_rate(static_cast<double>(kMicrosecondsPerSecond) /
                        static_cast<double>(median_duration_us));
  return track_number;
}

void RecordCriticalPreprocessing(ClipViewPipelineProfile &profile, double blocking_ms,
                                 double bayer_work_ms, double conversion_work_ms) {
  const double total_work_ms = bayer_work_ms + conversion_work_ms;
  if (total_work_ms <= 0.0) {
    return;
  }
  profile.bayer_fit_demosaic_ms = blocking_ms * bayer_work_ms / total_work_ms;
  profile.rgb_to_yuv420_ms = blocking_ms * conversion_work_ms / total_work_ms;
}

struct PendingMuxFrame {
  std::size_t frame_index;
  VaEncoderContext::PendingFrame va_frame;
};

void CompleteAndMuxFront(std::deque<PendingMuxFrame> &pending, VaEncoderContext &context,
                         mkvmuxer::Segment &segment, std::uint64_t track_number,
                         std::span<const std::uint64_t> media_times,
                         ClipViewPipelineProfile &profile) {
  if (pending.empty()) {
    throw std::logic_error("cannot complete an empty VA-API frame queue");
  }
  const PendingMuxFrame frame = pending.front();
  const auto encode_started = ProfileClock::now();
  const std::vector<std::uint8_t> encoded = context.Complete(frame.va_frame);
  profile.codec_encode_ms += ElapsedMilliseconds(encode_started);
  const auto mux_started = ProfileClock::now();
  if (frame.frame_index > 0U) {
    segment.ForceNewClusterOnNextFrame();
  }
  if (!segment.AddFrame(encoded.data(), encoded.size(), track_number,
                        media_times[frame.frame_index] * kWebmTimecodeScaleNanoseconds, true)) {
    throw std::runtime_error("could not mux VP9 frame into WebM");
  }
  profile.webm_mux_ms += ElapsedMilliseconds(mux_started);
  pending.pop_front();
}

class VaapiVp9WebmEncoder final : public ClipMediaEncoder {
 public:
  explicit VaapiVp9WebmEncoder(VaapiVp9WebmOptions options) : options_(std::move(options)) {}

  [[nodiscard]] ClipMediaEncoderCapabilities capabilities() const override {
    return {.encoder_id = "vaapi-vp9-all-intra-v1",
            .file_extension = "webm",
            .mime_type = "video/webm",
            .codec = "vp9",
            .hardware_accelerated = true,
            .deterministic = false,
            .all_frames_keyframes = true};
  }

  [[nodiscard]] EncodedClipMedia Encode(const CameraClipInput &input,
                                        const std::filesystem::path &output_path) const override {
    const auto total_started = ProfileClock::now();
    if (input.frames.size() < 2U) {
      throw std::invalid_argument("VP9 clip encoding requires at least two frames");
    }
    ClipViewPipelineProfile profile{.role = std::string(input.role),
                                    .frame_count = input.frames.size()};
    const std::vector<std::uint64_t> media_times = MediaTimesMicroseconds(input);
    const std::uint64_t cadence_us = MedianDurationMicroseconds(media_times);
    PreparedFrame first = PrepareFrame(input.frames.front(), options_);
    const EncodedGeometry geometry{.width = first.image.width, .height = first.image.height};
    double preprocessing_blocked_ms = first.bayer_fit_demosaic_ms + first.rgb_to_nv12_ms;
    BoundedFramePreprocessor preprocessor(input, options_, std::move(first));

    const VaApi api;
    const FileDescriptor render_node(options_.render_node);
    auto context = std::make_unique<VaEncoderContext>(api);
    context->Initialize(render_node.value(), geometry, options_.encoding_queue_depth);
    mkvmuxer::MkvWriter writer;
    const std::string output_string = output_path.string();
    if (!writer.Open(output_string.c_str())) {
      throw std::runtime_error("could not open WebM output: " + output_string);
    }
    mkvmuxer::Segment segment;
    const std::uint64_t track_number = ConfigureWebm(segment, writer, geometry, cadence_us);
    double bayer_work_ms = 0.0;
    double conversion_work_ms = 0.0;
    std::deque<PendingMuxFrame> pending;
    for (std::size_t index = 0; index < input.frames.size(); ++index) {
      if (pending.size() == options_.encoding_queue_depth) {
        CompleteAndMuxFront(pending, *context, segment, track_number, media_times, profile);
      }
      const auto preparation_wait_started = ProfileClock::now();
      const PreparedFrame prepared = preprocessor.Take(index);
      if (index > 0U) {
        preprocessing_blocked_ms += ElapsedMilliseconds(preparation_wait_started);
      }
      bayer_work_ms += prepared.bayer_fit_demosaic_ms;
      conversion_work_ms += prepared.rgb_to_nv12_ms;
      const auto encode_started = ProfileClock::now();
      const std::size_t surface_index = index % options_.encoding_queue_depth;
      context->Upload(prepared.image, surface_index);
      const VaEncoderContext::PendingFrame va_frame = context->SubmitKeyframe(
          {.quality_index = options_.quality_index, .surface_index = surface_index});
      profile.codec_encode_ms += ElapsedMilliseconds(encode_started);
      pending.push_back({.frame_index = index, .va_frame = va_frame});
    }
    while (!pending.empty()) {
      CompleteAndMuxFront(pending, *context, segment, track_number, media_times, profile);
    }
    RecordCriticalPreprocessing(profile, preprocessing_blocked_ms, bayer_work_ms,
                                conversion_work_ms);
    const auto finalize_started = ProfileClock::now();
    segment.set_duration(static_cast<double>(media_times.back() + cadence_us));
    if (!segment.Finalize()) {
      throw std::runtime_error("could not finalize VP9 WebM clip");
    }
    writer.Close();
    profile.finalize_ms = ElapsedMilliseconds(finalize_started);
    const auto verification_started = ProfileClock::now();
    const WebmInspection inspection = InspectWebm(output_path);
    profile.output_verification_ms = ElapsedMilliseconds(verification_started);
    if (inspection.codec != "vp9" || inspection.width != geometry.width ||
        inspection.height != geometry.height || inspection.frame_count != input.frames.size() ||
        inspection.keyframe_count != input.frames.size() ||
        inspection.first_frame_time != std::chrono::nanoseconds::zero() ||
        inspection.last_frame_time !=
            std::chrono::microseconds(static_cast<std::int64_t>(media_times.back()))) {
      throw std::runtime_error("post-write VP9 WebM verification did not match encoded clip");
    }
    profile.total_ms = ElapsedMilliseconds(total_started);
    return {.width = inspection.width,
            .height = inspection.height,
            .frame_count = inspection.frame_count,
            .keyframe_count = inspection.keyframe_count,
            .encoded_bytes = std::filesystem::file_size(output_path),
            .pipeline_profile = std::move(profile)};
  }

 private:
  VaapiVp9WebmOptions options_;
};

void ValidateOptions(const VaapiVp9WebmOptions &options) {
  if (options.render_node.empty()) {
    throw std::invalid_argument("VA-API render node path must not be empty");
  }
  if (options.maximum_width < 2U || options.maximum_height < 2U) {
    throw std::invalid_argument("clip encoder maximum dimensions must both be at least two");
  }
  if (options.quality_index > 255U) {
    throw std::invalid_argument("VA-API VP9 quality index must be in [0, 255]");
  }
  if (options.preprocessing_threads == 0U || options.preprocessing_threads > 32U) {
    throw std::invalid_argument("VA-API preprocessing thread count must be in [1, 32]");
  }
  if (options.encoding_queue_depth == 0U || options.encoding_queue_depth > 16U) {
    throw std::invalid_argument("VA-API encoding queue depth must be in [1, 16]");
  }
}

}  // namespace

std::unique_ptr<ClipMediaEncoder> MakeVaapiVp9WebmEncoder(VaapiVp9WebmOptions options) {
  ValidateOptions(options);
  return std::make_unique<VaapiVp9WebmEncoder>(std::move(options));
}

}  // namespace swing_capture::encoding
