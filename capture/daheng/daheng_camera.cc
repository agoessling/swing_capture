#include "capture/daheng/daheng_camera.h"

#include <GXDef.h>
#include <GXErrorList.h>
#include <GxIAPI.h>
#include <GxIAPILegacy.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/daheng/daheng_sdk_runtime.h"

namespace swing_capture::daheng {
namespace {

std::string StringFromNullTerminatedSpan(std::span<const char> characters) {
  const auto terminator = std::ranges::find(characters, '\0');
  return {characters.begin(), terminator};
}

std::string LastErrorText() {
  GX_STATUS error_code = GX_STATUS_SUCCESS;
  std::size_t size = 0;
  if (GXGetLastError(&error_code, nullptr, &size) != GX_STATUS_SUCCESS || size == 0) {
    return {};
  }

  std::string text(size, '\0');
  if (GXGetLastError(&error_code, text.data(), &size) != GX_STATUS_SUCCESS) {
    return {};
  }
  if (!text.empty() && text.back() == '\0') {
    text.pop_back();
  }
  return text;
}

void Check(GX_STATUS status, const std::string &operation) {
  if (status == GX_STATUS_SUCCESS) {
    return;
  }

  std::string message =
      operation + " failed with GX_STATUS " + std::to_string(static_cast<int>(status));
  const std::string detail = LastErrorText();
  if (!detail.empty()) {
    message += ": " + detail;
  }
  throw std::runtime_error(message);
}

std::optional<std::string> EnvironmentValue(std::string_view name) {
  const std::string owned_name(name);
  // The Galaxy SDK discovers its GenTL producer only through process startup
  // environment. These calls run while the SDK registry lock is held, before
  // GXInitLib can start any vendor threads.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = getenv(owned_name.c_str());
  return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

struct EnvironmentUpdate {
  std::string_view name;
  std::string_view value;
  bool overwrite;
};

void SetEnvironment(const EnvironmentUpdate &update) {
  const std::string name(update.name);
  const std::string value(update.value);
  // See EnvironmentValue: this is the required pre-GXInitLib configuration
  // boundary and is serialized by the SDK registry lock.
  // Clang also cannot associate this POSIX extension with <cstdlib>.
  // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner)
  if (setenv(name.c_str(), value.c_str(), update.overwrite ? 1 : 0) != 0) {
    throw std::runtime_error("failed to set " + name);
  }
}

std::string_view WithoutTrailingSlashes(std::string_view path) {
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  return path;
}

struct EnvironmentPathRequirement {
  std::string_view name;
  std::string_view required_path;
};

void PrependEnvironmentPath(const EnvironmentPathRequirement &requirement) {
  const std::string name(requirement.name);
  const std::string required(requirement.required_path);
  if (required.empty()) {
    throw std::runtime_error("cannot prepend an empty path to " + name);
  }

  std::vector<std::string> remaining_paths;
  const std::optional<std::string> current_value = EnvironmentValue(name);
  if (current_value.has_value() && !current_value->empty()) {
    const std::string &current = *current_value;
    std::size_t begin = 0;
    while (begin <= current.size()) {
      const std::size_t end = current.find(':', begin);
      const std::string entry =
          current.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      if (WithoutTrailingSlashes(entry) != WithoutTrailingSlashes(required)) {
        remaining_paths.push_back(entry);
      }
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    }
  }

  std::string updated = required;
  for (const std::string &path : remaining_paths) {
    updated += ':';
    updated += path;
  }
  if (current_value.has_value() && updated == *current_value) {
    return;
  }
  SetEnvironment({.name = name, .value = updated, .overwrite = true});
}

GX_NODE_ACCESS_MODE NodeAccess(GX_PORT_HANDLE port, const char *name) {
  GX_NODE_ACCESS_MODE access = GX_NODE_ACCESS_MODE_UNDEF;
  Check(GXGetNodeAccessMode(port, name, &access), std::string("query access for ") + name);
  return access;
}

void RequireReadWrite(GX_PORT_HANDLE port, const char *name) {
  const GX_NODE_ACCESS_MODE access = NodeAccess(port, name);
  if (access != GX_NODE_ACCESS_MODE_RW) {
    throw std::runtime_error(std::string("required camera node ") + name +
                             " is not readable and writable (access mode " +
                             std::to_string(static_cast<int>(access)) + ")");
  }
}

std::string ReadEnum(GX_PORT_HANDLE port, const char *name) {
  GX_ENUM_VALUE value = {};
  Check(GXGetEnumValue(port, name, &value), std::string("read ") + name);
  return StringFromNullTerminatedSpan(std::span(value.stCurValue.strCurSymbolic));
}

std::string TryReadEnum(GX_PORT_HANDLE port, const char *name) {
  GX_ENUM_VALUE value = {};
  if (GXGetEnumValue(port, name, &value) != GX_STATUS_SUCCESS) {
    return {};
  }
  return StringFromNullTerminatedSpan(std::span(value.stCurValue.strCurSymbolic));
}

void VerifyRequiredEnum(GX_PORT_HANDLE port, const char *name, const char *expected) {
  const std::string actual = ReadEnum(port, name);
  if (actual != expected) {
    throw std::runtime_error(std::string("camera did not retain required ") + name + "=" +
                             expected + " (read back " + actual + ")");
  }
}

void SetRequiredEnum(GX_PORT_HANDLE port, const char *name, const char *requested) {
  RequireReadWrite(port, name);
  Check(GXSetEnumValueByString(port, name, requested),
        std::string("set ") + name + "=" + requested);
  VerifyRequiredEnum(port, name, requested);
}

GX_FLOAT_VALUE ReadFloatValue(GX_PORT_HANDLE port, const char *name) {
  GX_FLOAT_VALUE value = {};
  Check(GXGetFloatValue(port, name, &value), std::string("read ") + name);
  return value;
}

std::optional<GX_FLOAT_VALUE> TryReadFloatValue(GX_PORT_HANDLE port, const char *name) {
  GX_FLOAT_VALUE value = {};
  if (GXGetFloatValue(port, name, &value) != GX_STATUS_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

double QuantizeFloat(double requested, const GX_FLOAT_VALUE &node) {
  double configured = std::clamp(requested, node.dMin, node.dMax);
  if (node.bIncIsValid && std::isfinite(node.dInc) && node.dInc > 0.0) {
    const double step_count = std::round((configured - node.dMin) / node.dInc);
    configured = node.dMin + step_count * node.dInc;
    configured = std::clamp(configured, node.dMin, node.dMax);
  }
  return configured;
}

double VerifyRequiredFloat(GX_PORT_HANDLE port, const char *name, double expected) {
  const GX_FLOAT_VALUE readback = ReadFloatValue(port, name);
  const double increment_tolerance =
      readback.bIncIsValid && std::isfinite(readback.dInc) && readback.dInc > 0.0
          ? readback.dInc * 0.51
          : 0.0;
  const double tolerance = std::max({increment_tolerance, 1e-6, std::abs(expected) * 1e-6});
  if (!std::isfinite(readback.dCurValue) || std::abs(readback.dCurValue - expected) > tolerance) {
    throw std::runtime_error(std::string("camera did not retain required ") + name +
                             " value (expected " + std::to_string(expected) + ", read back " +
                             std::to_string(readback.dCurValue) + ", increment " +
                             std::to_string(readback.bIncIsValid ? readback.dInc : 0.0) + ")");
  }
  return readback.dCurValue;
}

double SetRequiredFloat(GX_PORT_HANDLE port, const char *name, double requested) {
  if (!std::isfinite(requested)) {
    throw std::invalid_argument(std::string(name) + " must be a finite number");
  }
  RequireReadWrite(port, name);
  const GX_FLOAT_VALUE before = ReadFloatValue(port, name);
  if (!std::isfinite(before.dMin) || !std::isfinite(before.dMax) || before.dMin > before.dMax) {
    throw std::runtime_error(std::string("camera reported an invalid range for ") + name);
  }
  const double configured = QuantizeFloat(requested, before);
  Check(GXSetFloatValue(port, name, configured), std::string("set ") + name);
  return VerifyRequiredFloat(port, name, configured);
}

std::int64_t SetRequiredInt(GX_PORT_HANDLE port, const char *name, std::int64_t requested) {
  RequireReadWrite(port, name);
  GX_INT_VALUE value = {};
  Check(GXGetIntValue(port, name, &value), std::string("read ") + name);
  std::int64_t configured = std::clamp(requested, value.nMin, value.nMax);
  if (value.nInc > 0) {
    const std::int64_t steps = (configured - value.nMin + value.nInc / 2) / value.nInc;
    configured = value.nMin + steps * value.nInc;
    configured = std::clamp(configured, value.nMin, value.nMax);
  }
  Check(GXSetIntValue(port, name, configured), std::string("set ") + name);
  GX_INT_VALUE readback = {};
  Check(GXGetIntValue(port, name, &readback), std::string("read ") + name);
  if (readback.nCurValue != configured) {
    throw std::runtime_error(std::string("camera did not retain required ") + name +
                             " value (requested " + std::to_string(requested) + ", configured " +
                             std::to_string(configured) + ", read back " +
                             std::to_string(readback.nCurValue) + ")");
  }
  return configured;
}

std::string ReadString(GX_PORT_HANDLE port, const char *name) {
  GX_STRING_VALUE value = {};
  Check(GXGetStringValue(port, name, &value), std::string("read ") + name);
  return StringFromNullTerminatedSpan(std::span(value.strCurValue));
}

double ReadFloat(GX_PORT_HANDLE port, const char *name) {
  return ReadFloatValue(port, name).dCurValue;
}

std::int64_t ReadInt(GX_PORT_HANDLE port, const char *name) {
  GX_INT_VALUE value = {};
  Check(GXGetIntValue(port, name, &value), std::string("read ") + name);
  return value.nCurValue;
}

std::string TransportName(GX_DEVICE_CLASS device_class) {
  switch (device_class) {
    case GX_DEVICE_CLASS_USB2:
      return "USB2";
    case GX_DEVICE_CLASS_GEV:
      return "GigE Vision";
    case GX_DEVICE_CLASS_U3V:
      return "USB3 Vision";
    case GX_DEVICE_CLASS_CXP:
      return "CoaXPress";
    case GX_DEVICE_CLASS_SMART:
      return "Smart";
    default:
      return "Unknown";
  }
}

class DequeuedFrameLease final {
 public:
  DequeuedFrameLease(GX_DEV_HANDLE device, PGX_FRAME_DATA_EX frame)
      : device_(device), frame_(frame) {}

  ~DequeuedFrameLease() {
    if (frame_ != nullptr) {
      // Destructors must not replace an exception from the frame handler.
      GXQBufEx(device_, frame_);
    }
  }

  DequeuedFrameLease(const DequeuedFrameLease &) = delete;
  DequeuedFrameLease &operator=(const DequeuedFrameLease &) = delete;
  DequeuedFrameLease(DequeuedFrameLease &&) = delete;
  DequeuedFrameLease &operator=(DequeuedFrameLease &&) = delete;

  GX_STATUS Requeue() noexcept {
    PGX_FRAME_DATA_EX frame = std::exchange(frame_, nullptr);
    return frame == nullptr ? GX_STATUS_SUCCESS : GXQBufEx(device_, frame);
  }

 private:
  GX_DEV_HANDLE device_;
  PGX_FRAME_DATA_EX frame_;
};

}  // namespace

struct GalaxySdk::Impl {
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  Impl() {
    std::string runfiles_error;
    if (const auto gentl_directory = FindBundledGentlDirectory(&runfiles_error)) {
      PrependEnvironmentPath({
          .name = "GENICAM_GENTL64_PATH",
          .required_path = *gentl_directory,
      });
    } else {
      const std::optional<std::string> configured = EnvironmentValue("GENICAM_GENTL64_PATH");
      if (!configured.has_value() || configured->empty()) {
        throw std::runtime_error(
            "cannot locate the Bazel-bundled Daheng GenTL producer and "
            "GENICAM_GENTL64_PATH is not configured: " +
            runfiles_error);
      }
    }
    SetEnvironment({
        .name = "LOG4CPLUS_LOGLOG_QUIETMODE",
        .value = "true",
        .overwrite = false,
    });
    Check(GXInitLib(), "GXInitLib");
  }

  ~Impl() noexcept { GXCloseLib(); }
};

GalaxySdk::GalaxySdk() {
  // The registry itself intentionally outlives C++ exit destruction, but it
  // holds only a weak reference. The final GalaxySdk/camera owner therefore
  // calls GXCloseLib before main returns, while Daheng's own C++ globals are
  // still alive. Letting a strong function-static reference close the SDK
  // from an exit handler races the vendor library's static destructors and
  // causes a use-after-free inside GXCloseLib.
  struct Registry {
    std::mutex mutex;
    std::weak_ptr<Impl> state;
  };
  static auto *const registry = new Registry();

  const std::scoped_lock lock(registry->mutex);
  if (std::shared_ptr<Impl> existing = registry->state.lock()) {
    impl_ = std::move(existing);
    return;
  }

  // The deleter takes the same registry lock so GXCloseLib cannot overlap a
  // concurrent constructor's GXInitLib.
  std::shared_ptr<Impl> created(new Impl(), [](Impl *state) noexcept {
    const std::scoped_lock close_lock(registry->mutex);
    delete state;
  });
  registry->state = created;
  impl_ = std::move(created);
}

GalaxySdk::~GalaxySdk() = default;

std::vector<DiscoveredCamera> GalaxySdk::Discover(std::chrono::milliseconds timeout) const {
  std::uint32_t device_count = 0;
  Check(GXUpdateDeviceList(&device_count,
                           static_cast<std::uint32_t>(std::max<std::int64_t>(timeout.count(), 0))),
        "GXUpdateDeviceList");

  if (device_count == 0) {
    return {};
  }

  std::vector<GX_DEVICE_BASE_INFO> device_info(device_count);
  std::size_t buffer_size = device_info.size() * sizeof(device_info.front());
  Check(GXGetAllDeviceBaseInfo(device_info.data(), &buffer_size), "GXGetAllDeviceBaseInfo");

  const std::size_t returned_count =
      std::min(device_info.size(), buffer_size / sizeof(device_info.front()));
  std::vector<DiscoveredCamera> cameras;
  cameras.reserve(returned_count);
  for (std::size_t index = 0; index < returned_count; ++index) {
    const GX_DEVICE_BASE_INFO &info = device_info[index];
    cameras.push_back({
        .identity =
            {
                .model = StringFromNullTerminatedSpan(std::span(info.szModelName)),
                .serial_number = StringFromNullTerminatedSpan(std::span(info.szSN)),
                .vendor = StringFromNullTerminatedSpan(std::span(info.szVendorName)),
            },
        .transport = TransportName(info.deviceClass),
    });
  }
  return cameras;
}

struct DahengCamera::Impl {
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  Impl(std::shared_ptr<void> sdk_state, std::string requested_serial)
      : sdk_lifetime(std::move(sdk_state)), serial_number(std::move(requested_serial)) {
    GX_OPEN_PARAM open_parameter = {};
    open_parameter.accessMode = GX_ACCESS_EXCLUSIVE;
    open_parameter.openMode = GX_OPEN_SN;
    open_parameter.pszContent = serial_number.data();
    Check(GXOpenDevice(&open_parameter, &device), "GXOpenDevice(" + serial_number + ")");

    try {
      camera_identity = {
          .model = ReadString(device, "DeviceModelName"),
          .serial_number = ReadString(device, "DeviceSerialNumber"),
          .vendor = ReadString(device, "DeviceVendorName"),
      };

      std::uint32_t stream_count = 0;
      Check(GXGetDataStreamNumFromDev(device, &stream_count), "GXGetDataStreamNumFromDev");
      if (stream_count == 0) {
        throw std::runtime_error("camera has no data streams");
      }
      Check(GXGetDataStreamHandleFromDev(device, 1, &stream), "GXGetDataStreamHandleFromDev");
    } catch (...) {
      GXCloseDevice(device);
      device = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (started) {
      GXStreamOff(device);
    }
    if (device != nullptr) {
      GXCloseDevice(device);
    }
  }

  void RefreshDiagnostics() {
    capture_profile.width = static_cast<std::uint32_t>(ReadInt(device, "Width"));
    capture_profile.height = static_cast<std::uint32_t>(ReadInt(device, "Height"));
    capture_profile.pixel_format = ReadEnum(device, "PixelFormat");
    capture_profile.frames_per_second = ReadFloat(device, "CurrentAcquisitionFrameRate");
    camera_diagnostics.width = capture_profile.width;
    camera_diagnostics.height = capture_profile.height;
    camera_diagnostics.offset_x = ReadInt(device, "OffsetX");
    camera_diagnostics.offset_y = ReadInt(device, "OffsetY");

    const GX_FLOAT_VALUE exposure = ReadFloatValue(device, "ExposureTime");
    camera_diagnostics.exposure_microseconds = exposure.dCurValue;
    camera_diagnostics.minimum_exposure_microseconds = exposure.dMin;
    camera_diagnostics.maximum_exposure_microseconds = exposure.dMax;
    camera_diagnostics.exposure_increment_microseconds = exposure.dInc;
    camera_diagnostics.exposure_increment_valid = exposure.bIncIsValid;
    if (const auto gain = TryReadFloatValue(device, "Gain")) {
      camera_diagnostics.gain_decibels = gain->dCurValue;
      camera_diagnostics.minimum_gain_decibels = gain->dMin;
      camera_diagnostics.maximum_gain_decibels = gain->dMax;
      camera_diagnostics.gain_increment_decibels = gain->dInc;
      camera_diagnostics.gain_increment_valid = gain->bIncIsValid;
    }
    const GX_FLOAT_VALUE frame_rate = ReadFloatValue(device, "AcquisitionFrameRate");
    camera_diagnostics.target_frames_per_second = frame_rate.dCurValue;
    camera_diagnostics.minimum_target_frames_per_second = frame_rate.dMin;
    camera_diagnostics.maximum_target_frames_per_second = frame_rate.dMax;
    camera_diagnostics.frame_rate_increment = frame_rate.dInc;
    camera_diagnostics.frame_rate_increment_valid = frame_rate.bIncIsValid;
    camera_diagnostics.resulting_frames_per_second = capture_profile.frames_per_second;
    camera_diagnostics.acquisition_mode = TryReadEnum(device, "AcquisitionMode");
    camera_diagnostics.trigger_selector = TryReadEnum(device, "TriggerSelector");
    camera_diagnostics.trigger_mode = TryReadEnum(device, "TriggerMode");
    camera_diagnostics.pixel_format = TryReadEnum(device, "PixelFormat");
    camera_diagnostics.exposure_mode = TryReadEnum(device, "ExposureMode");
    camera_diagnostics.exposure_auto = TryReadEnum(device, "ExposureAuto");
    camera_diagnostics.gain_selector = TryReadEnum(device, "GainSelector");
    camera_diagnostics.gain_auto = TryReadEnum(device, "GainAuto");
    camera_diagnostics.exposure_time_mode = TryReadEnum(device, "ExposureTimeMode");
    camera_diagnostics.throughput_limit_mode = TryReadEnum(device, "DeviceLinkThroughputLimitMode");
    camera_diagnostics.frame_rate_mode = TryReadEnum(device, "AcquisitionFrameRateMode");
    std::int64_t timestamp_frequency = 0;
    if (GXGetInt(device, GX_INT_TIMESTAMP_TICK_FREQUENCY, &timestamp_frequency) ==
            GX_STATUS_SUCCESS &&
        timestamp_frequency > 0) {
      camera_diagnostics.timestamp_ticks_per_second =
          static_cast<std::uint64_t>(timestamp_frequency);
      camera_diagnostics.timestamp_frequency_source = "camera_feature";
    } else if (camera_identity.model.starts_with("MER2-")) {
      // MER2 USB3 frame metadata uses a nanosecond timestamp even though this
      // model does not implement TimestampTickFrequency.
      camera_diagnostics.timestamp_ticks_per_second = 1000000000ULL;
      camera_diagnostics.timestamp_frequency_source = "MER2_USB3_nanoseconds";
    } else {
      camera_diagnostics.timestamp_ticks_per_second = 0;
      camera_diagnostics.timestamp_frequency_source = "unavailable";
    }

    std::uint32_t payload_size = 0;
    Check(GXGetPayLoadSize(stream, &payload_size), "GXGetPayLoadSize");
    camera_diagnostics.payload_bytes = payload_size;

    GX_INT_VALUE value = {};
    if (GXGetIntValue(device, "EstimatedBandwidth", &value) == GX_STATUS_SUCCESS) {
      camera_diagnostics.estimated_bandwidth_bytes_per_second =
          static_cast<std::uint64_t>(value.nCurValue);
    }
    if (GXGetIntValue(stream, "StreamTransferSize", &value) == GX_STATUS_SUCCESS) {
      camera_diagnostics.stream_transfer_bytes = value.nCurValue;
    }
    if (GXGetIntValue(stream, "StreamTransferNumberUrb", &value) == GX_STATUS_SUCCESS) {
      camera_diagnostics.stream_urb_count = value.nCurValue;
    }
  }

  std::shared_ptr<void> sdk_lifetime;
  std::string serial_number;
  GX_DEV_HANDLE device = nullptr;
  GX_DS_HANDLE stream = nullptr;
  bool started = false;
  CameraIdentity camera_identity;
  CaptureProfile capture_profile;
  DahengDiagnostics camera_diagnostics;
};

DahengCamera::DahengCamera(GalaxySdk &sdk, std::string serial_number)
    : impl_(std::make_unique<Impl>(sdk.impl_, std::move(serial_number))) {
  impl_->RefreshDiagnostics();
}

DahengCamera::~DahengCamera() = default;

CameraIdentity DahengCamera::identity() const { return impl_->camera_identity; }

CaptureProfile DahengCamera::profile() const { return impl_->capture_profile; }

DahengDiagnostics DahengCamera::diagnostics() const { return impl_->camera_diagnostics; }

void DahengCamera::Configure(const DahengConfiguration &configuration) {
  if (impl_->started) {
    throw std::logic_error("cannot configure a streaming camera");
  }
  if (!std::isfinite(configuration.target_frames_per_second) ||
      configuration.target_frames_per_second <= 0.0) {
    throw std::invalid_argument("target_frames_per_second must be finite and positive");
  }
  if (!std::isfinite(configuration.exposure_microseconds) ||
      configuration.exposure_microseconds <= 0.0) {
    throw std::invalid_argument("exposure_microseconds must be finite and positive");
  }
  if (!std::isfinite(configuration.gain_decibels) || configuration.gain_decibels < 0.0) {
    throw std::invalid_argument("gain_decibels must be finite and nonnegative");
  }
  if (configuration.acquisition_buffer_count == 0) {
    throw std::invalid_argument("acquisition_buffer_count must be positive");
  }
  if (configuration.width == 0 || configuration.height == 0) {
    throw std::invalid_argument("capture width and height must be positive");
  }

  impl_->camera_diagnostics.deterministic_free_run_verified = false;
  impl_->camera_diagnostics.requested_exposure_microseconds = configuration.exposure_microseconds;
  impl_->camera_diagnostics.requested_gain_decibels = configuration.gain_decibels;
  impl_->camera_diagnostics.requested_frames_per_second = configuration.target_frames_per_second;

  SetRequiredEnum(impl_->device, "AcquisitionMode", "Continuous");
  SetRequiredEnum(impl_->device, "TriggerSelector", "FrameStart");
  SetRequiredEnum(impl_->device, "TriggerMode", "Off");
  SetRequiredEnum(impl_->device, "PixelFormat", "BayerRG8");
  SetRequiredInt(impl_->device, "OffsetX", 0);
  SetRequiredInt(impl_->device, "OffsetY", 0);
  const std::int64_t configured_width = SetRequiredInt(impl_->device, "Width", configuration.width);
  const std::int64_t configured_height =
      SetRequiredInt(impl_->device, "Height", configuration.height);
  if (std::cmp_not_equal(configured_width, configuration.width) ||
      std::cmp_not_equal(configured_height, configuration.height)) {
    throw std::runtime_error("camera cannot provide the required capture dimensions");
  }
  SetRequiredEnum(impl_->device, "ExposureAuto", "Off");
  SetRequiredEnum(impl_->device, "GainSelector", "AnalogAll");
  SetRequiredEnum(impl_->device, "GainAuto", "Off");
  SetRequiredEnum(impl_->device, "ExposureMode", "Timed");
  SetRequiredEnum(impl_->device, "ExposureTimeMode", "Standard");
  SetRequiredEnum(impl_->device, "DeviceLinkThroughputLimitMode", "Off");
  SetRequiredEnum(impl_->device, "AcquisitionFrameRateMode", "On");

  const double configured_exposure =
      SetRequiredFloat(impl_->device, "ExposureTime", configuration.exposure_microseconds);
  const double configured_gain =
      SetRequiredFloat(impl_->device, "Gain", configuration.gain_decibels);
  const double configured_frame_rate = SetRequiredFloat(impl_->device, "AcquisitionFrameRate",
                                                        configuration.target_frames_per_second);

  // Re-read every mode after all dependent features are configured. Some
  // GenICam nodes can change access or values when another selector changes.
  VerifyRequiredEnum(impl_->device, "AcquisitionMode", "Continuous");
  VerifyRequiredEnum(impl_->device, "TriggerSelector", "FrameStart");
  VerifyRequiredEnum(impl_->device, "TriggerMode", "Off");
  VerifyRequiredEnum(impl_->device, "PixelFormat", "BayerRG8");
  VerifyRequiredEnum(impl_->device, "ExposureAuto", "Off");
  VerifyRequiredEnum(impl_->device, "GainSelector", "AnalogAll");
  VerifyRequiredEnum(impl_->device, "GainAuto", "Off");
  VerifyRequiredEnum(impl_->device, "ExposureMode", "Timed");
  VerifyRequiredEnum(impl_->device, "ExposureTimeMode", "Standard");
  VerifyRequiredEnum(impl_->device, "DeviceLinkThroughputLimitMode", "Off");
  VerifyRequiredEnum(impl_->device, "AcquisitionFrameRateMode", "On");
  VerifyRequiredFloat(impl_->device, "ExposureTime", configured_exposure);
  VerifyRequiredFloat(impl_->device, "Gain", configured_gain);
  VerifyRequiredFloat(impl_->device, "AcquisitionFrameRate", configured_frame_rate);

  Check(GXSetAcqusitionBufferNumber(impl_->device, configuration.acquisition_buffer_count),
        "GXSetAcqusitionBufferNumber");
  impl_->camera_diagnostics.acquisition_buffer_count = configuration.acquisition_buffer_count;
  if (configuration.stream_transfer_bytes > 0) {
    SetRequiredInt(impl_->stream, "StreamTransferSize", configuration.stream_transfer_bytes);
  }
  if (configuration.stream_urb_count > 0) {
    SetRequiredInt(impl_->stream, "StreamTransferNumberUrb", configuration.stream_urb_count);
  }

  impl_->RefreshDiagnostics();
  impl_->camera_diagnostics.deterministic_free_run_verified = true;
}

void DahengCamera::Start() {
  if (impl_->started) {
    throw std::logic_error("camera is already streaming");
  }
  Check(GXStreamOn(impl_->device), "GXStreamOn(" + impl_->camera_identity.serial_number + ")");
  impl_->started = true;
}

void DahengCamera::Stop() {
  if (!impl_->started) {
    return;
  }
  Check(GXStreamOff(impl_->device), "GXStreamOff(" + impl_->camera_identity.serial_number + ")");
  impl_->started = false;
}

bool DahengCamera::CaptureOne(std::chrono::milliseconds timeout, const FrameHandler &handler) {
  if (!impl_->started) {
    throw std::logic_error("camera is not streaming");
  }

  PGX_FRAME_DATA_EX frame = nullptr;
  const GX_STATUS dequeue_status =
      GXDQBufEx(impl_->device, &frame,
                static_cast<std::uint32_t>(std::max<std::int64_t>(timeout.count(), 0)));
  if (dequeue_status == GX_STATUS_TIMEOUT) {
    return false;
  }
  Check(dequeue_status, "GXDQBufEx");
  if (frame == nullptr) {
    throw std::runtime_error("GXDQBufEx returned a null frame");
  }
  DequeuedFrameLease frame_lease(impl_->device, frame);

  const auto received_at = std::chrono::steady_clock::now();
  const bool complete = frame->nStatus == GX_FRAME_STATUS_SUCCESS && frame->pImgBuf != 0 &&
                        frame->nImgSize > 0 && frame->nWidth > 0 && frame->nHeight > 0;
  const std::size_t payload_size =
      complete && frame->nImgSize > 0 ? static_cast<std::size_t>(frame->nImgSize) : 0;
  // The vendor ABI represents an image address as uint64_t (`pvoid64`) rather
  // than a pointer. Keep that unavoidable conversion at this single boundary.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  const auto *image_buffer = reinterpret_cast<const std::byte *>(frame->pImgBuf);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  const FrameView view = {
      .metadata =
          {
              .frame_id = frame->nFrameID,
              .device_timestamp = frame->nTimestamp,
              .host_received_at = received_at,
              .width = static_cast<std::uint32_t>(frame->nWidth),
              .height = static_cast<std::uint32_t>(frame->nHeight),
              .complete = complete,
          },
      .payload = std::span<const std::byte>(image_buffer, payload_size),
  };

  handler(view);
  Check(frame_lease.Requeue(), "GXQBufEx");
  return true;
}

}  // namespace swing_capture::daheng
