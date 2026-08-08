#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "capture/core/camera_source.h"
#include "capture/core/device_clock_mapper.h"
#include "capture/core/pooled_raw_frame_ring.h"
#include "capture/daheng/daheng_camera.h"
#include "capture/hil/hil_metrics.h"
#include "capture/image/bayer_rg8.h"
#include "capture/image/image_quality.h"

namespace {

using SteadyClock = std::chrono::steady_clock;
using swing_capture::DeviceClockMapper;
using swing_capture::FrameView;
using swing_capture::PooledRawFramePushResult;
using swing_capture::PooledRawFrameRing;
using swing_capture::PooledRawFrameRingConfig;
using swing_capture::PooledRawFrameSnapshot;
using swing_capture::daheng::DahengCamera;
using swing_capture::daheng::DahengConfiguration;
using swing_capture::daheng::DahengDiagnostics;
using swing_capture::daheng::DiscoveredCamera;
using swing_capture::daheng::GalaxySdk;
using swing_capture::hil::CameraMetrics;
using swing_capture::hil::Check;
using swing_capture::hil::EvaluateCamera;
using swing_capture::hil::Evaluation;
using swing_capture::hil::Thresholds;
using swing_capture::image::ImageQualityMetrics;

struct Options {
  bool show_help = false;
  int duration_seconds = 10;
  double target_fps = 227.0;
  double exposure_microseconds = 4000.0;
  double gain_decibels = 0.0;
  double minimum_fps_ratio = 0.95;
  double maximum_host_frame_interval_multiple = 10.0;
  double maximum_device_frame_interval_multiple = 4.0;
  double ring_seconds = 0.0;
  double ring_reserve_seconds = 0.0;
  bool exercise_frozen_ring = false;
  std::size_t required_camera_count = 0;
  bool require_distinct_root_controllers = true;
  std::vector<std::string> serial_numbers;
  std::optional<std::filesystem::path> json_output;
};

struct UsbDevice {
  std::string product_id;
  std::string product_name;
  std::string serial;
  std::filesystem::path device_node;
  std::string root_controller;
  bool can_read_write = false;
};

struct TopologyResult {
  bool all_usb_devices_found = false;
  bool all_device_nodes_read_write = false;
  bool distinct_root_controllers = false;
  bool passed = false;
  std::string message;
};

struct RunningMoments {
  void Add(double value) {
    ++count;
    const double delta = value - mean;
    mean += delta / static_cast<double>(count);
    const double delta_after = value - mean;
    sum_squared_delta += delta * delta_after;
    maximum = std::max(maximum, value);
  }

  [[nodiscard]] double StandardDeviation() const {
    return count > 1 ? std::sqrt(sum_squared_delta / static_cast<double>(count - 1)) : 0.0;
  }

  std::uint64_t count = 0;
  double mean = 0.0;
  double sum_squared_delta = 0.0;
  double maximum = 0.0;
};

struct CaptureStats {
  std::string serial;
  std::uint64_t successful_frames = 0;
  std::uint64_t incomplete_frames = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t frame_id_gaps = 0;
  std::uint64_t timestamp_non_monotonic = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t first_frame_id = 0;
  std::uint64_t last_frame_id = 0;
  std::uint64_t first_device_timestamp = 0;
  std::uint64_t last_device_timestamp = 0;
  SteadyClock::time_point first_host_timestamp;
  SteadyClock::time_point last_host_timestamp;
  RunningMoments host_intervals_ms;
  std::uint64_t maximum_device_interval_ticks = 0;
  std::size_t ring_capacity_frames = 0;
  std::size_t ring_reserve_frames = 0;
  std::size_t ring_allocated_bytes = 0;
  std::size_t frozen_ring_frames = 0;
  std::uint64_t diagnostic_frame_id = 0;
  std::string diagnostic_frame_path;
  std::optional<ImageQualityMetrics> diagnostic_image_quality;
  std::unique_ptr<DeviceClockMapper> clock_mapper;
  std::exception_ptr error;

  [[nodiscard]] double HostFramesPerSecond() const {
    if (successful_frames < 2) {
      return 0.0;
    }
    const double seconds =
        std::chrono::duration<double>(last_host_timestamp - first_host_timestamp).count();
    return seconds > 0.0 ? static_cast<double>(successful_frames - 1) / seconds : 0.0;
  }

  [[nodiscard]] double DeviceFramesPerSecond(std::uint64_t timestamp_frequency) const {
    if (successful_frames < 2 || last_device_timestamp <= first_device_timestamp ||
        timestamp_frequency == 0) {
      return 0.0;
    }
    const double seconds = static_cast<double>(last_device_timestamp - first_device_timestamp) /
                           static_cast<double>(timestamp_frequency);
    return static_cast<double>(successful_frames - 1) / seconds;
  }

  [[nodiscard]] double MeasuredDurationSeconds() const {
    if (successful_frames < 2) {
      return 0.0;
    }
    return std::chrono::duration<double>(last_host_timestamp - first_host_timestamp).count();
  }

  [[nodiscard]] double MaximumDeviceIntervalSeconds(std::uint64_t timestamp_frequency) const {
    return timestamp_frequency > 0 ? static_cast<double>(maximum_device_interval_ticks) /
                                         static_cast<double>(timestamp_frequency)
                                   : 0.0;
  }
};

std::string ReadTextFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::string value;
  std::getline(input, value);
  return value;
}

std::optional<int> ReadIntegerFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  int value = 0;
  if (!(input >> value)) {
    return std::nullopt;
  }
  return value;
}

std::uint64_t ReadResidentSetBytes() {
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key == "VmRSS:") {
      std::uint64_t kibibytes = 0;
      status >> kibibytes;
      return kibibytes * 1024;
    }
    std::string remainder;
    std::getline(status, remainder);
  }
  return 0;
}

std::optional<std::string> EnvironmentValue(std::string_view name) {
  const std::string owned_name(name);
  // Reading the Bazel process environment is confined to startup, before the
  // capture threads are created.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(owned_name.c_str());
  return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

std::filesystem::path ResolveOutputPath(std::filesystem::path output_path) {
  if (output_path.is_relative()) {
    if (const auto workspace = EnvironmentValue("BUILD_WORKSPACE_DIRECTORY");
        workspace.has_value() && !workspace->empty()) {
      output_path = std::filesystem::path(*workspace) / output_path;
    }
  }
  return output_path;
}

std::optional<std::filesystem::path> FindJsonOutputPath(std::span<char *> arguments) {
  for (std::size_t index = 1; index + 1 < arguments.size(); ++index) {
    if (std::string_view(arguments[index]) == "--json") {
      return ResolveOutputPath(arguments[index + 1]);
    }
  }
  if (const auto test_outputs = EnvironmentValue("TEST_UNDECLARED_OUTPUTS_DIR");
      test_outputs.has_value() && !test_outputs->empty()) {
    return std::filesystem::path(*test_outputs) / "report.json";
  }
  return std::nullopt;
}

void WriteTextFileAtomically(const std::filesystem::path &output_path, std::string_view contents) {
  if (!output_path.parent_path().empty()) {
    std::error_code directory_error;
    std::filesystem::create_directories(output_path.parent_path(), directory_error);
    if (directory_error) {
      throw std::runtime_error("cannot create output directory: " + directory_error.message());
    }
  }

  std::filesystem::path temporary_path = output_path;
  temporary_path += ".tmp." + std::to_string(getpid());
  {
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open temporary output: " + temporary_path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
      std::error_code remove_error;
      std::filesystem::remove(temporary_path, remove_error);
      throw std::runtime_error("cannot write temporary output: " + temporary_path.string());
    }
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary_path, output_path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
    throw std::runtime_error("cannot publish output: " + rename_error.message());
  }
}

std::string RootController(const std::filesystem::path &sysfs_path) {
  std::error_code error;
  const std::string canonical = std::filesystem::canonical(sysfs_path, error).string();
  if (error) {
    return {};
  }
  const std::size_t usb_component = canonical.find("/usb");
  return usb_component == std::string::npos ? canonical : canonical.substr(0, usb_component);
}

std::vector<UsbDevice> FindDahengUsbDevices() {
  std::vector<UsbDevice> devices;
  const std::filesystem::path sysfs_root = "/sys/bus/usb/devices";
  std::error_code error;

  for (const auto &entry : std::filesystem::directory_iterator(sysfs_root, error)) {
    const auto &path = entry.path();
    if (ReadTextFile(path / "idVendor") != "2ba2") {
      continue;
    }

    const std::optional<int> bus_number = ReadIntegerFile(path / "busnum");
    const std::optional<int> device_number = ReadIntegerFile(path / "devnum");
    if (!bus_number || !device_number) {
      continue;
    }

    std::ostringstream device_node;
    device_node << "/dev/bus/usb/" << std::setfill('0') << std::setw(3) << *bus_number << '/'
                << std::setw(3) << *device_number;

    UsbDevice device = {
        .product_id = ReadTextFile(path / "idProduct"),
        .product_name = ReadTextFile(path / "product"),
        .serial = ReadTextFile(path / "serial"),
        .device_node = device_node.str(),
        .root_controller = RootController(path),
    };
    device.can_read_write = access(device.device_node.c_str(), R_OK | W_OK) == 0;
    devices.push_back(std::move(device));
  }

  std::ranges::sort(devices, {}, &UsbDevice::serial);
  return devices;
}

void PrintUsage(const char *program) {
  std::cout << "Usage: " << program << " [duration-seconds] [options]\n"
            << "  --duration-seconds N\n"
            << "  --fps N\n"
            << "  --exposure-us N\n"
            << "  --gain-db N          Fixed analog gain; default 0\n"
            << "  --serial SERIAL       May be repeated; default is all cameras\n"
            << "  --minimum-fps-ratio N Pass threshold; default 0.95\n"
            << "  --maximum-host-frame-interval-multiple N\n"
            << "                        Host scheduling threshold; default 10\n"
            << "  --maximum-device-frame-interval-multiple N\n"
            << "                        Camera timing threshold; default 4\n"
            << "  --ring-seconds N     Active pre-impact retention window\n"
            << "  --ring-reserve-seconds N\n"
            << "                        Blocks available while a snapshot is frozen\n"
            << "  --exercise-frozen-ring\n"
            << "  --require-camera-count N\n"
            << "  --allow-shared-root-controller\n"
            << "  --json PATH           Write machine-readable results\n";
}

std::string RequireValue(std::span<char *> arguments, std::size_t &index, std::string_view option) {
  if (++index >= arguments.size()) {
    throw std::invalid_argument(std::string(option) + " requires a value");
  }
  return arguments[index];
}

bool ApplyNamedOption(std::string_view argument, std::span<char *> arguments, std::size_t &index,
                      Options *options) {
  if (argument == "--duration-seconds") {
    options->duration_seconds = std::stoi(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--fps") {
    options->target_fps = std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--exposure-us") {
    options->exposure_microseconds = std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--gain-db") {
    options->gain_decibels = std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--serial") {
    options->serial_numbers.push_back(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--minimum-fps-ratio") {
    options->minimum_fps_ratio = std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--maximum-host-frame-interval-multiple") {
    options->maximum_host_frame_interval_multiple =
        std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--maximum-device-frame-interval-multiple") {
    options->maximum_device_frame_interval_multiple =
        std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--maximum-frame-interval-multiple") {
    const double legacy_multiple = std::stod(RequireValue(arguments, index, argument));
    options->maximum_host_frame_interval_multiple = legacy_multiple;
    options->maximum_device_frame_interval_multiple = legacy_multiple;
    return true;
  }
  if (argument == "--ring-seconds") {
    options->ring_seconds = std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--ring-reserve-seconds") {
    options->ring_reserve_seconds = std::stod(RequireValue(arguments, index, argument));
    return true;
  }
  if (argument == "--exercise-frozen-ring") {
    options->exercise_frozen_ring = true;
    return true;
  }
  if (argument == "--require-camera-count") {
    options->required_camera_count =
        static_cast<std::size_t>(std::stoul(RequireValue(arguments, index, argument)));
    return true;
  }
  if (argument == "--allow-shared-root-controller") {
    options->require_distinct_root_controllers = false;
    return true;
  }
  if (argument == "--json") {
    options->json_output = RequireValue(arguments, index, argument);
    return true;
  }
  return false;
}

void ValidateOptions(const Options &options) {
  if (options.duration_seconds <= 0 || options.target_fps <= 0.0 ||
      options.exposure_microseconds <= 0.0 || !std::isfinite(options.gain_decibels) ||
      options.gain_decibels < 0.0 || options.minimum_fps_ratio <= 0.0 ||
      options.minimum_fps_ratio > 1.0 || options.ring_seconds < 0.0 ||
      options.ring_seconds > 10.0 || options.ring_reserve_seconds < 0.0 ||
      options.ring_reserve_seconds > 10.0 || options.maximum_host_frame_interval_multiple <= 0.0 ||
      options.maximum_device_frame_interval_multiple <= 0.0) {
    throw std::invalid_argument("capture options are outside valid ranges");
  }
  const std::set<std::string> unique_serials(options.serial_numbers.begin(),
                                             options.serial_numbers.end());
  if (unique_serials.size() != options.serial_numbers.size()) {
    throw std::invalid_argument("duplicate --serial selection");
  }
  if ((options.ring_reserve_seconds > 0.0 || options.exercise_frozen_ring) &&
      options.ring_seconds == 0.0) {
    throw std::invalid_argument("ring reserve/freeze requires a nonzero --ring-seconds");
  }
}

Options ParseOptions(std::span<char *> arguments) {
  Options options;
  bool consumed_positional_duration = false;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
      return options;
    }
    if (ApplyNamedOption(argument, arguments, index, &options)) {
      continue;
    }
    if (!argument.empty() && argument.front() != '-' && !consumed_positional_duration) {
      options.duration_seconds = std::stoi(std::string(argument));
      consumed_positional_duration = true;
      continue;
    }
    throw std::invalid_argument("unknown argument: " + std::string(argument));
  }

  ValidateOptions(options);
  return options;
}

std::string JsonEscape(const std::string &value) {
  std::ostringstream escaped;
  for (const char character : value) {
    switch (character) {
      case '"':
        escaped << "\\\"";
        break;
      case '\\':
        escaped << "\\\\";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        escaped << character;
    }
  }
  return escaped.str();
}

std::string BuildFatalErrorJson(const std::string &message) {
  std::ostringstream json;
  json << "{\n"
       << "  \"schema_version\": 3,\n"
       << "  \"test_scope\": \"transport_and_retention\",\n"
       << "  \"passed\": false,\n"
       << "  \"diagnostic_images_nominal\": false,\n"
       << "  \"fatal_error\": \"" << JsonEscape(message) << "\",\n"
       << "  \"cameras\": []\n"
       << "}\n";
  return json.str();
}

std::string SafeFileComponent(const std::string &value) {
  std::string safe = value;
  for (char &character : safe) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) == 0 && character != '-' && character != '_') {
      character = '_';
    }
  }
  return safe.empty() ? "unknown" : safe;
}

bool LikelyUnderexposed(const ImageQualityMetrics &quality) {
  return quality.p99 <= 16 || quality.near_black_fraction >= 0.98 ||
         (quality.p50 <= 8 && quality.mean <= 16.0);
}

bool LikelyOverexposed(const ImageQualityMetrics &quality) {
  return quality.p01 >= 239 || quality.near_white_fraction >= 0.50;
}

bool LikelyLowDetail(const ImageQualityMetrics &quality) { return quality.gradient_energy < 4.0; }

std::string ImageQualityAssessment(const ImageQualityMetrics &quality) {
  if (LikelyUnderexposed(quality)) {
    return "underexposed_or_obscured";
  }
  if (LikelyOverexposed(quality)) {
    return "overexposed";
  }
  if (LikelyLowDetail(quality)) {
    return "low_detail";
  }
  return "nominal";
}

const UsbDevice *FindUsbDevice(const std::vector<UsbDevice> &devices, const std::string &serial) {
  const auto found = std::ranges::find(devices, serial, &UsbDevice::serial);
  return found == devices.end() ? nullptr : &*found;
}

TopologyResult EvaluateTopology(const Options &options, const std::vector<UsbDevice> &usb_devices,
                                const std::vector<std::string> &selected_serials) {
  TopologyResult result = {
      .all_usb_devices_found = true,
      .all_device_nodes_read_write = true,
      .distinct_root_controllers = true,
  };
  std::set<std::string> root_controllers;
  for (const std::string &serial : selected_serials) {
    const UsbDevice *usb = FindUsbDevice(usb_devices, serial);
    if (usb == nullptr) {
      result.all_usb_devices_found = false;
      result.all_device_nodes_read_write = false;
      result.distinct_root_controllers = false;
      continue;
    }
    result.all_device_nodes_read_write = result.all_device_nodes_read_write && usb->can_read_write;
    if (usb->root_controller.empty()) {
      result.distinct_root_controllers = false;
    } else {
      root_controllers.insert(usb->root_controller);
    }
  }
  if (selected_serials.size() > 1) {
    result.distinct_root_controllers =
        result.distinct_root_controllers && root_controllers.size() == selected_serials.size();
  }

  const bool controller_policy_passed = !options.require_distinct_root_controllers ||
                                        selected_serials.size() < 2 ||
                                        result.distinct_root_controllers;
  result.passed = result.all_usb_devices_found && result.all_device_nodes_read_write &&
                  controller_policy_passed;

  std::ostringstream message;
  message << "usb_devices_found=" << (result.all_usb_devices_found ? "yes" : "no")
          << " device_access="
          << (result.all_device_nodes_read_write ? "read/write" : "insufficient")
          << " distinct_root_controllers=" << (result.distinct_root_controllers ? "yes" : "no")
          << " distinct_required=" << (options.require_distinct_root_controllers ? "yes" : "no");
  result.message = message.str();
  return result;
}

void RecordFrame(CaptureStats &stats, const FrameView &frame) {
  if (!frame.metadata.complete) {
    ++stats.incomplete_frames;
    return;
  }

  if (stats.successful_frames == 0) {
    stats.first_frame_id = frame.metadata.frame_id;
    stats.first_device_timestamp = frame.metadata.device_timestamp;
    stats.first_host_timestamp = frame.metadata.host_received_at;
    if (stats.clock_mapper != nullptr) {
      stats.clock_mapper->AddSample(frame.metadata.device_timestamp,
                                    frame.metadata.host_received_at);
    }
  } else {
    if (frame.metadata.frame_id != stats.last_frame_id + 1U) {
      stats.frame_id_gaps += frame.metadata.frame_id > stats.last_frame_id
                                 ? frame.metadata.frame_id - stats.last_frame_id - 1U
                                 : 1U;
    }
    if (frame.metadata.device_timestamp <= stats.last_device_timestamp) {
      ++stats.timestamp_non_monotonic;
    } else {
      stats.maximum_device_interval_ticks =
          std::max(stats.maximum_device_interval_ticks,
                   frame.metadata.device_timestamp - stats.last_device_timestamp);
      if (stats.clock_mapper != nullptr) {
        stats.clock_mapper->AddSample(frame.metadata.device_timestamp,
                                      frame.metadata.host_received_at);
      }
    }
    const double interval_ms = std::chrono::duration<double, std::milli>(
                                   frame.metadata.host_received_at - stats.last_host_timestamp)
                                   .count();
    stats.host_intervals_ms.Add(interval_ms);
  }

  stats.last_frame_id = frame.metadata.frame_id;
  stats.last_device_timestamp = frame.metadata.device_timestamp;
  stats.last_host_timestamp = frame.metadata.host_received_at;
  stats.payload_bytes += frame.payload.size();
  ++stats.successful_frames;
}

std::string ErrorMessage(const std::exception_ptr &error) {
  if (error == nullptr) {
    return {};
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &exception) {
    return exception.what();
  } catch (...) {
    return "unknown exception";
  }
}

CameraMetrics BuildMetrics(const Options &options, const DahengDiagnostics &diagnostics,
                           const CaptureStats &stats) {
  return {
      .requested_frames_per_second = options.target_fps,
      .resulting_frames_per_second = diagnostics.resulting_frames_per_second,
      .host_frames_per_second = stats.HostFramesPerSecond(),
      .device_frames_per_second =
          stats.DeviceFramesPerSecond(diagnostics.timestamp_ticks_per_second),
      .maximum_host_frame_interval_seconds = stats.host_intervals_ms.maximum / 1000.0,
      .maximum_device_frame_interval_seconds =
          stats.MaximumDeviceIntervalSeconds(diagnostics.timestamp_ticks_per_second),
      .requested_duration_seconds = static_cast<double>(options.duration_seconds),
      .measured_duration_seconds = stats.MeasuredDurationSeconds(),
      .complete_frames = stats.successful_frames,
      .incomplete_frames = stats.incomplete_frames,
      .timeouts = stats.timeouts,
      .missing_frame_ids = stats.frame_id_gaps,
      .timestamp_non_monotonic = stats.timestamp_non_monotonic,
      .payload_bytes = stats.payload_bytes,
      .expected_payload_bytes_per_frame = diagnostics.payload_bytes,
      .internal_error = stats.error != nullptr,
  };
}

std::string_view JsonBoolean(bool value) { return value ? "true" : "false"; }

bool DiagnosticImagesNominal(std::span<const CaptureStats> stats) {
  return !stats.empty() && std::ranges::all_of(stats, [](const CaptureStats &camera_stats) {
    return camera_stats.diagnostic_image_quality.has_value() &&
           ImageQualityAssessment(camera_stats.diagnostic_image_quality.value()) == "nominal";
  });
}

void AppendDiagnosticImageQuality(std::ostringstream &json,
                                  const std::optional<ImageQualityMetrics> &maybe_quality) {
  if (!maybe_quality.has_value()) {
    json << "null,\n";
    return;
  }
  const ImageQualityMetrics &quality = maybe_quality.value();
  json << "{\n"
       << "        \"assessment\": \"" << ImageQualityAssessment(quality) << "\",\n"
       << "        \"likely_underexposed\": " << JsonBoolean(LikelyUnderexposed(quality)) << ",\n"
       << "        \"likely_overexposed\": " << JsonBoolean(LikelyOverexposed(quality)) << ",\n"
       << "        \"likely_low_detail\": " << JsonBoolean(LikelyLowDetail(quality)) << ",\n"
       << "        \"sample_count\": " << quality.sample_count << ",\n"
       << "        \"minimum\": " << static_cast<unsigned int>(quality.min_value) << ",\n"
       << "        \"maximum\": " << static_cast<unsigned int>(quality.max_value) << ",\n"
       << "        \"mean\": " << quality.mean << ",\n"
       << "        \"p01\": " << static_cast<unsigned int>(quality.p01) << ",\n"
       << "        \"p50\": " << static_cast<unsigned int>(quality.p50) << ",\n"
       << "        \"p99\": " << static_cast<unsigned int>(quality.p99) << ",\n"
       << "        \"near_black_fraction\": " << quality.near_black_fraction << ",\n"
       << "        \"near_white_fraction\": " << quality.near_white_fraction << ",\n"
       << "        \"gradient_energy\": " << quality.gradient_energy << "\n"
       << "      },\n";
}

void AppendChecks(std::ostringstream &json, std::span<const Check> checks) {
  for (std::size_t index = 0; index < checks.size(); ++index) {
    const Check &check = checks[index];
    json << "        {\"id\": \"" << JsonEscape(check.id)
         << "\", \"passed\": " << JsonBoolean(check.passed) << ", \"message\": \""
         << JsonEscape(check.message) << "\"}";
    json << (index + 1 == checks.size() ? "\n" : ",\n");
  }
}

struct UsbJsonDetails {
  std::string device_node;
  std::string root_controller;
};

UsbJsonDetails UsbDetails(const UsbDevice *usb) {
  if (usb == nullptr) {
    return {};
  }
  return {
      .device_node = usb->device_node.string(),
      .root_controller = usb->root_controller,
  };
}

struct ClockMappingJson {
  bool ready = false;
  std::size_t sample_count = 0;
  double clock_rate_ratio = 0.0;
  double drift_parts_per_million = 0.0;
  double residual_standard_deviation_microseconds = 0.0;
};

ClockMappingJson ClockMappingDetails(const CaptureStats &stats) {
  ClockMappingJson details;
  if (stats.clock_mapper == nullptr) {
    return details;
  }
  details.sample_count = stats.clock_mapper->sample_count();
  details.ready = stats.clock_mapper->ready();
  if (details.ready) {
    details.clock_rate_ratio = stats.clock_mapper->clock_rate_ratio();
    details.drift_parts_per_million = stats.clock_mapper->clock_drift_parts_per_million();
    details.residual_standard_deviation_microseconds =
        stats.clock_mapper->residual_standard_deviation_seconds() * 1000000.0;
  }
  return details;
}

std::string_view CameraObjectTerminator(bool last_camera) { return last_camera ? "\n" : ",\n"; }

SteadyClock::duration AbsoluteDuration(SteadyClock::duration duration) {
  return duration >= SteadyClock::duration::zero() ? duration : -duration;
}

std::string BuildJson(const Options &options, const std::vector<UsbDevice> &usb_devices,
                      const TopologyResult &topology,
                      const std::vector<DahengDiagnostics> &diagnostics,
                      const std::vector<CaptureStats> &stats,
                      std::uint64_t resident_bytes_before_capture,
                      std::uint64_t resident_bytes_after_capture, bool passed) {
  const bool diagnostic_images_nominal = DiagnosticImagesNominal(stats);

  std::ostringstream json;
  json << std::fixed << std::setprecision(6);
  json << "{\n"
       << "  \"schema_version\": 3,\n"
       << "  \"test_scope\": \"transport_and_retention\",\n"
       << "  \"passed\": " << JsonBoolean(passed) << ",\n"
       << "  \"diagnostic_images_nominal\": " << JsonBoolean(diagnostic_images_nominal) << ",\n"
       << "  \"requested_duration_seconds\": " << options.duration_seconds << ",\n"
       << "  \"requested_frames_per_second\": " << options.target_fps << ",\n"
       << "  \"requested_exposure_microseconds\": " << options.exposure_microseconds << ",\n"
       << "  \"requested_gain_decibels\": " << options.gain_decibels << ",\n"
       << "  \"maximum_host_frame_interval_multiple\": "
       << options.maximum_host_frame_interval_multiple << ",\n"
       << "  \"maximum_device_frame_interval_multiple\": "
       << options.maximum_device_frame_interval_multiple << ",\n"
       << "  \"raw_ring_seconds\": " << options.ring_seconds << ",\n"
       << "  \"raw_ring_reserve_seconds\": " << options.ring_reserve_seconds << ",\n"
       << "  \"exercise_frozen_ring\": " << JsonBoolean(options.exercise_frozen_ring) << ",\n"
       << "  \"required_camera_count\": " << options.required_camera_count << ",\n"
       << "  \"topology\": {\n"
       << "    \"passed\": " << JsonBoolean(topology.passed) << ",\n"
       << "    \"distinct_root_controllers_required\": "
       << JsonBoolean(options.require_distinct_root_controllers) << ",\n"
       << "    \"all_usb_devices_found\": " << JsonBoolean(topology.all_usb_devices_found) << ",\n"
       << "    \"all_device_nodes_read_write\": "
       << JsonBoolean(topology.all_device_nodes_read_write) << ",\n"
       << "    \"distinct_root_controllers\": " << JsonBoolean(topology.distinct_root_controllers)
       << ",\n"
       << "    \"message\": \"" << JsonEscape(topology.message) << "\"\n"
       << "  },\n"
       << "  \"resident_bytes_before_capture\": " << resident_bytes_before_capture << ",\n"
       << "  \"resident_bytes_after_capture\": " << resident_bytes_after_capture << ",\n"
       << "  \"cameras\": [\n";

  for (std::size_t index = 0; index < stats.size(); ++index) {
    const CaptureStats &camera_stats = stats[index];
    const DahengDiagnostics &camera_diagnostics = diagnostics[index];
    const UsbDevice *usb = FindUsbDevice(usb_devices, camera_stats.serial);
    const double host_fps = camera_stats.HostFramesPerSecond();
    const double device_fps =
        camera_stats.DeviceFramesPerSecond(camera_diagnostics.timestamp_ticks_per_second);
    const ClockMappingJson clock_mapping = ClockMappingDetails(camera_stats);
    const UsbJsonDetails usb_details = UsbDetails(usb);
    const Evaluation evaluation = EvaluateCamera(
        BuildMetrics(options, camera_diagnostics, camera_stats),
        Thresholds{
            .minimum_fps_ratio = options.minimum_fps_ratio,
            .maximum_host_frame_interval_multiple = options.maximum_host_frame_interval_multiple,
            .maximum_device_frame_interval_multiple =
                options.maximum_device_frame_interval_multiple,
        });

    json << "    {\n"
         << "      \"serial\": \"" << JsonEscape(camera_stats.serial) << "\",\n"
         << "      \"usb_node\": \"" << JsonEscape(usb_details.device_node) << "\",\n"
         << "      \"usb_root_controller\": \"" << JsonEscape(usb_details.root_controller)
         << "\",\n"
         << "      \"diagnostic_frame_id\": " << camera_stats.diagnostic_frame_id << ",\n"
         << "      \"diagnostic_frame_path\": \"" << JsonEscape(camera_stats.diagnostic_frame_path)
         << "\",\n"
         << "      \"diagnostic_image_quality\": ";
    AppendDiagnosticImageQuality(json, camera_stats.diagnostic_image_quality);
    json << "      \"configuration\": {\n"
         << "        \"verified\": "
         << JsonBoolean(camera_diagnostics.deterministic_free_run_verified) << ",\n"
         << "        \"width\": " << camera_diagnostics.width << ",\n"
         << "        \"height\": " << camera_diagnostics.height << ",\n"
         << "        \"offset_x\": " << camera_diagnostics.offset_x << ",\n"
         << "        \"offset_y\": " << camera_diagnostics.offset_y << ",\n"
         << "        \"pixel_format\": \"" << JsonEscape(camera_diagnostics.pixel_format) << "\",\n"
         << "        \"acquisition_mode\": \"" << JsonEscape(camera_diagnostics.acquisition_mode)
         << "\",\n"
         << "        \"trigger_selector\": \"" << JsonEscape(camera_diagnostics.trigger_selector)
         << "\",\n"
         << "        \"trigger_mode\": \"" << JsonEscape(camera_diagnostics.trigger_mode) << "\",\n"
         << "        \"exposure_mode\": \"" << JsonEscape(camera_diagnostics.exposure_mode)
         << "\",\n"
         << "        \"exposure_auto\": \"" << JsonEscape(camera_diagnostics.exposure_auto)
         << "\",\n"
         << "        \"gain_selector\": \"" << JsonEscape(camera_diagnostics.gain_selector)
         << "\",\n"
         << "        \"gain_auto\": \"" << JsonEscape(camera_diagnostics.gain_auto) << "\",\n"
         << "        \"exposure_time_mode\": \""
         << JsonEscape(camera_diagnostics.exposure_time_mode) << "\",\n"
         << "        \"throughput_limit_mode\": \""
         << JsonEscape(camera_diagnostics.throughput_limit_mode) << "\",\n"
         << "        \"frame_rate_mode\": \"" << JsonEscape(camera_diagnostics.frame_rate_mode)
         << "\",\n"
         << "        \"requested_exposure_microseconds\": "
         << camera_diagnostics.requested_exposure_microseconds << ",\n"
         << "        \"exposure_microseconds\": " << camera_diagnostics.exposure_microseconds
         << ",\n"
         << "        \"requested_gain_decibels\": " << camera_diagnostics.requested_gain_decibels
         << ",\n"
         << "        \"gain_decibels\": " << camera_diagnostics.gain_decibels << ",\n"
         << "        \"requested_frames_per_second\": "
         << camera_diagnostics.requested_frames_per_second << ",\n"
         << "        \"target_frames_per_second\": " << camera_diagnostics.target_frames_per_second
         << ",\n"
         << "        \"payload_bytes_per_frame\": " << camera_diagnostics.payload_bytes << ",\n"
         << "        \"acquisition_buffer_count\": " << camera_diagnostics.acquisition_buffer_count
         << ",\n"
         << "        \"stream_transfer_bytes\": " << camera_diagnostics.stream_transfer_bytes
         << ",\n"
         << "        \"stream_urb_count\": " << camera_diagnostics.stream_urb_count << "\n"
         << "      },\n"
         << "      \"resulting_frames_per_second\": "
         << camera_diagnostics.resulting_frames_per_second << ",\n"
         << "      \"host_measured_frames_per_second\": " << host_fps << ",\n"
         << "      \"device_measured_frames_per_second\": " << device_fps << ",\n"
         << "      \"timestamp_frequency_hz\": " << camera_diagnostics.timestamp_ticks_per_second
         << ",\n"
         << "      \"timestamp_frequency_source\": \""
         << JsonEscape(camera_diagnostics.timestamp_frequency_source) << "\",\n"
         << "      \"clock_mapping\": {\n"
         << "        \"ready\": " << JsonBoolean(clock_mapping.ready) << ",\n"
         << "        \"sample_count\": " << clock_mapping.sample_count << ",\n"
         << "        \"host_seconds_per_device_second\": " << clock_mapping.clock_rate_ratio
         << ",\n"
         << "        \"drift_parts_per_million\": " << clock_mapping.drift_parts_per_million
         << ",\n"
         << "        \"residual_standard_deviation_microseconds\": "
         << clock_mapping.residual_standard_deviation_microseconds << "\n"
         << "      },\n"
         << "      \"successful_frames\": " << camera_stats.successful_frames << ",\n"
         << "      \"incomplete_frames\": " << camera_stats.incomplete_frames << ",\n"
         << "      \"timeouts\": " << camera_stats.timeouts << ",\n"
         << "      \"frame_id_gaps\": " << camera_stats.frame_id_gaps << ",\n"
         << "      \"timestamp_non_monotonic\": " << camera_stats.timestamp_non_monotonic << ",\n"
         << "      \"payload_bytes\": " << camera_stats.payload_bytes << ",\n"
         << "      \"retention\": {\n"
         << "        \"active_capacity_frames\": " << camera_stats.ring_capacity_frames << ",\n"
         << "        \"reserve_frames\": " << camera_stats.ring_reserve_frames << ",\n"
         << "        \"allocated_bytes\": " << camera_stats.ring_allocated_bytes << ",\n"
         << "        \"frozen_frames\": " << camera_stats.frozen_ring_frames << "\n"
         << "      },\n"
         << "      \"host_interval_mean_ms\": " << camera_stats.host_intervals_ms.mean << ",\n"
         << "      \"host_interval_stddev_ms\": "
         << camera_stats.host_intervals_ms.StandardDeviation() << ",\n"
         << "      \"host_interval_max_ms\": " << camera_stats.host_intervals_ms.maximum << ",\n"
         << "      \"device_interval_max_ms\": "
         << camera_stats.MaximumDeviceIntervalSeconds(
                camera_diagnostics.timestamp_ticks_per_second) *
                1000.0
         << ",\n"
         << "      \"error\": \"" << JsonEscape(ErrorMessage(camera_stats.error)) << "\",\n"
         << "      \"checks\": [\n";
    AppendChecks(json, evaluation.checks);
    json << "      ]\n"
         << "    }" << CameraObjectTerminator(index + 1 == stats.size());
  }

  json << "  ]";
  if (stats.size() == 2 && stats[0].successful_frames > 0 && stats[1].successful_frames > 0) {
    const auto first_frame_skew = stats[0].first_host_timestamp - stats[1].first_host_timestamp;
    json << ",\n  \"first_frame_host_skew_microseconds\": "
         << std::chrono::duration<double, std::micro>(AbsoluteDuration(first_frame_skew)).count();
  }
  json << "\n}\n";
  return json.str();
}

void PrintUsbDevices(std::span<const UsbDevice> devices) {
  for (const UsbDevice &usb : devices) {
    std::cout << "USB: " << usb.product_name << " serial=" << usb.serial
              << " node=" << usb.device_node.string()
              << " access=" << (usb.can_read_write ? "read/write" : "insufficient")
              << " root=" << usb.root_controller << '\n';
  }
  std::cout.flush();
}

std::vector<std::string> SelectSerials(const Options &options,
                                       std::span<const DiscoveredCamera> discovered) {
  if (discovered.empty()) {
    throw std::runtime_error("Galaxy found no cameras");
  }

  std::vector<std::string> selected_serials = options.serial_numbers;
  if (selected_serials.empty()) {
    for (const DiscoveredCamera &camera : discovered) {
      selected_serials.push_back(camera.identity.serial_number);
    }
    std::ranges::sort(selected_serials);
  }
  if (options.required_camera_count > 0 &&
      selected_serials.size() != options.required_camera_count) {
    throw std::runtime_error("camera-count gate failed: required " +
                             std::to_string(options.required_camera_count) + ", found " +
                             std::to_string(selected_serials.size()));
  }

  for (const std::string &serial : selected_serials) {
    if (std::ranges::find(discovered, serial, [](const auto &camera) {
          return camera.identity.serial_number;
        }) == discovered.end()) {
      throw std::invalid_argument("requested camera not found: " + serial);
    }
  }
  return selected_serials;
}

struct ConfiguredCapture {
  std::vector<std::unique_ptr<DahengCamera>> cameras;
  std::vector<std::unique_ptr<PooledRawFrameRing>> rings;
  std::vector<DahengDiagnostics> diagnostics;
};

ConfiguredCapture ConfigureCameras(GalaxySdk &sdk, const Options &options,
                                   std::span<const std::string> selected_serials) {
  const DahengConfiguration configuration = {
      .target_frames_per_second = options.target_fps,
      .exposure_microseconds = options.exposure_microseconds,
      .gain_decibels = options.gain_decibels,
  };
  ConfiguredCapture configured;
  configured.cameras.reserve(selected_serials.size());
  configured.rings.reserve(selected_serials.size());
  configured.diagnostics.reserve(selected_serials.size());
  for (const std::string &serial : selected_serials) {
    auto camera = std::make_unique<DahengCamera>(sdk, serial);
    camera->Configure(configuration);
    const auto identity = camera->identity();
    const auto profile = camera->profile();
    const auto diagnostics = camera->diagnostics();
    std::cout << "Configured: " << identity.model << " serial=" << identity.serial_number << ' '
              << profile.width << 'x' << profile.height << ' ' << profile.pixel_format
              << " exposure_us=" << diagnostics.exposure_microseconds
              << " gain_db=" << diagnostics.gain_decibels
              << " target_fps=" << diagnostics.target_frames_per_second
              << " resulting_fps=" << diagnostics.resulting_frames_per_second
              << " transfer_bytes=" << diagnostics.stream_transfer_bytes
              << " urbs=" << diagnostics.stream_urb_count << '\n';
    configured.diagnostics.push_back(diagnostics);
    if (options.ring_seconds > 0.0) {
      const auto frame_capacity = static_cast<std::size_t>(
          std::ceil(diagnostics.resulting_frames_per_second * options.ring_seconds));
      const auto reserve_frames = static_cast<std::size_t>(
          std::ceil(diagnostics.resulting_frames_per_second * options.ring_reserve_seconds));
      auto ring = std::make_unique<PooledRawFrameRing>(PooledRawFrameRingConfig{
          .active_frame_capacity = frame_capacity,
          .reserve_frame_blocks = reserve_frames,
          .maximum_payload_bytes = diagnostics.payload_bytes,
      });
      std::cout << "Raw ring: serial=" << identity.serial_number
                << " active_frames=" << frame_capacity << " reserve_frames=" << reserve_frames
                << " allocated_bytes=" << ring->allocated_bytes() << '\n';
      configured.rings.push_back(std::move(ring));
    } else {
      configured.rings.push_back(nullptr);
    }
    configured.cameras.push_back(std::move(camera));
  }
  std::cout.flush();
  return configured;
}

struct AcquisitionStartGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool released = false;
  bool cancelled = false;
};

void CaptureCamera(const Options &options, DahengCamera &camera, PooledRawFrameRing *ring,
                   CaptureStats &stats, std::optional<PooledRawFrameSnapshot> &frozen_snapshot,
                   AcquisitionStartGate &start_gate, std::atomic<bool> &capture_cancelled) {
  try {
    {
      std::unique_lock lock(start_gate.mutex);
      start_gate.condition.wait(lock, [&] { return start_gate.released; });
      if (start_gate.cancelled) {
        return;
      }
    }
    camera.Start();
    const auto capture_started = SteadyClock::now();
    const auto deadline = capture_started + std::chrono::seconds(options.duration_seconds);
    const auto freeze_at =
        capture_started +
        std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(static_cast<double>(options.duration_seconds) / 2.0));
    while (!capture_cancelled.load(std::memory_order_relaxed) && SteadyClock::now() < deadline) {
      if (!camera.CaptureOne(std::chrono::milliseconds(250), [&](const FrameView &frame) {
            RecordFrame(stats, frame);
            if (ring != nullptr && frame.metadata.complete) {
              const PooledRawFramePushResult result = ring->TryPush(frame);
              if (result != PooledRawFramePushResult::kStored) {
                throw std::runtime_error("pooled raw ring could not retain a complete frame");
              }
            }
          })) {
        ++stats.timeouts;
        capture_cancelled.store(true, std::memory_order_relaxed);
      }
      if (options.exercise_frozen_ring && !frozen_snapshot.has_value() &&
          SteadyClock::now() >= freeze_at) {
        const PooledRawFrameSnapshot &snapshot = frozen_snapshot.emplace(ring->Freeze());
        stats.frozen_ring_frames = snapshot.size();
      }
    }
    if (options.exercise_frozen_ring && stats.frozen_ring_frames != stats.ring_capacity_frames) {
      throw std::runtime_error("frozen raw ring did not contain a full retention window");
    }
    camera.Stop();
  } catch (...) {
    stats.error = std::current_exception();
    capture_cancelled.store(true, std::memory_order_relaxed);
    try {
      camera.Stop();
    } catch (const std::exception &stop_error) {
      std::cerr << "camera_probe: cannot stop " << stats.serial
                << " after capture failure: " << stop_error.what() << '\n';
    }
  }
}

struct CaptureRun {
  std::vector<CaptureStats> stats;
  std::vector<std::optional<PooledRawFrameSnapshot>> frozen_snapshots;
  std::uint64_t resident_bytes_before_capture = 0;
  std::uint64_t resident_bytes_after_capture = 0;
};

CaptureRun CaptureAllCameras(const Options &options, ConfiguredCapture &configured) {
  CaptureRun run;
  run.resident_bytes_before_capture = ReadResidentSetBytes();
  run.stats.resize(configured.cameras.size());
  run.frozen_snapshots.resize(configured.cameras.size());
  for (std::size_t index = 0; index < configured.cameras.size(); ++index) {
    run.stats[index].serial = configured.cameras[index]->identity().serial_number;
    if (configured.diagnostics[index].timestamp_ticks_per_second > 0) {
      run.stats[index].clock_mapper = std::make_unique<DeviceClockMapper>(
          configured.diagnostics[index].timestamp_ticks_per_second);
    }
    if (configured.rings[index] != nullptr) {
      run.stats[index].ring_capacity_frames = configured.rings[index]->capacity();
      run.stats[index].ring_reserve_frames = configured.rings[index]->reserve_block_count();
      run.stats[index].ring_allocated_bytes = configured.rings[index]->allocated_bytes();
    }
  }

  AcquisitionStartGate start_gate;
  std::atomic<bool> capture_cancelled = false;
  std::vector<std::jthread> acquisition_threads;
  acquisition_threads.reserve(configured.cameras.size());
  try {
    for (std::size_t index = 0; index < configured.cameras.size(); ++index) {
      acquisition_threads.emplace_back([&, index] {
        CaptureCamera(options, *configured.cameras[index], configured.rings[index].get(),
                      run.stats[index], run.frozen_snapshots[index], start_gate, capture_cancelled);
      });
    }
  } catch (...) {
    {
      const std::scoped_lock lock(start_gate.mutex);
      start_gate.cancelled = true;
      start_gate.released = true;
    }
    start_gate.condition.notify_all();
    throw;
  }
  {
    const std::scoped_lock lock(start_gate.mutex);
    start_gate.released = true;
  }
  start_gate.condition.notify_all();
  for (std::jthread &thread : acquisition_threads) {
    thread.join();
  }
  run.resident_bytes_after_capture = ReadResidentSetBytes();
  return run;
}

std::optional<std::filesystem::path> WriteDiagnosticFrames(const Options &options,
                                                           const ConfiguredCapture &configured,
                                                           CaptureRun &run) {
  if (!options.json_output.has_value()) {
    return std::nullopt;
  }
  std::filesystem::path json_output = ResolveOutputPath(options.json_output.value());
  for (std::size_t index = 0; index < configured.rings.size(); ++index) {
    if (configured.rings[index] == nullptr) {
      continue;
    }
    std::optional<PooledRawFrameSnapshot> &frozen_snapshot = run.frozen_snapshots[index];
    if (!frozen_snapshot.has_value()) {
      frozen_snapshot.emplace(configured.rings[index]->Freeze());
    }
    if (!frozen_snapshot.has_value()) {
      throw std::logic_error("raw ring snapshot was not created");
    }
    const PooledRawFrameSnapshot &snapshot = frozen_snapshot.value();
    run.stats[index].frozen_ring_frames = snapshot.size();
    if (snapshot.empty()) {
      continue;
    }

    const auto &frame = snapshot.at(snapshot.size() / 2U);
    const FrameView view = frame.view();
    const ImageQualityMetrics image_quality = swing_capture::image::MeasureRaw8ImageQuality({
        .pixels = view.payload,
        .width = view.metadata.width,
        .height = view.metadata.height,
    });
    run.stats[index].diagnostic_image_quality = image_quality;
    const auto rgb = swing_capture::image::DemosaicBayerRg8(view.payload, view.metadata.width,
                                                            view.metadata.height);
    const std::filesystem::path image_path =
        json_output.parent_path() /
        ("frame-" + SafeFileComponent(run.stats[index].serial) + ".png");
    WriteTextFileAtomically(image_path, swing_capture::image::EncodePng(rgb));
    run.stats[index].diagnostic_frame_id = view.metadata.frame_id;
    run.stats[index].diagnostic_frame_path = image_path.filename().string();
    std::cout << "Diagnostic frame: serial=" << run.stats[index].serial
              << " frame_id=" << view.metadata.frame_id
              << " quality=" << ImageQualityAssessment(image_quality)
              << " mean=" << image_quality.mean
              << " p99=" << static_cast<unsigned int>(image_quality.p99)
              << " gradient=" << image_quality.gradient_energy << " path=" << image_path.string()
              << '\n';
  }
  return json_output;
}

bool EvaluateAndPrint(const Options &options, const TopologyResult &topology,
                      const ConfiguredCapture &configured, std::span<const CaptureStats> stats) {
  bool passed = topology.passed;
  for (std::size_t index = 0; index < stats.size(); ++index) {
    const CaptureStats &camera_stats = stats[index];
    const DahengDiagnostics &diagnostics = configured.diagnostics[index];
    const double host_fps = camera_stats.HostFramesPerSecond();
    const double device_fps =
        camera_stats.DeviceFramesPerSecond(diagnostics.timestamp_ticks_per_second);
    const Evaluation evaluation = EvaluateCamera(
        BuildMetrics(options, diagnostics, camera_stats),
        Thresholds{
            .minimum_fps_ratio = options.minimum_fps_ratio,
            .maximum_host_frame_interval_multiple = options.maximum_host_frame_interval_multiple,
            .maximum_device_frame_interval_multiple =
                options.maximum_device_frame_interval_multiple,
        });
    passed = passed && evaluation.passed;

    std::cout << std::fixed << std::setprecision(3) << "Result: serial=" << camera_stats.serial
              << " pass=" << (evaluation.passed ? "yes" : "no")
              << " frames=" << camera_stats.successful_frames
              << " incomplete=" << camera_stats.incomplete_frames
              << " timeouts=" << camera_stats.timeouts << " gaps=" << camera_stats.frame_id_gaps
              << " host_fps=" << host_fps << " device_fps=" << device_fps
              << " interval_mean_ms=" << camera_stats.host_intervals_ms.mean
              << " interval_stddev_ms=" << camera_stats.host_intervals_ms.StandardDeviation()
              << " interval_max_ms=" << camera_stats.host_intervals_ms.maximum
              << " device_interval_max_ms="
              << camera_stats.MaximumDeviceIntervalSeconds(diagnostics.timestamp_ticks_per_second) *
                     1000.0
              << " ring_frames=" << camera_stats.ring_capacity_frames
              << " ring_reserve=" << camera_stats.ring_reserve_frames
              << " frozen_frames=" << camera_stats.frozen_ring_frames;
    const std::string error = ErrorMessage(camera_stats.error);
    if (!error.empty()) {
      std::cout << " error=\"" << error << '"';
    }
    std::cout << '\n';
  }

  if (stats.size() == 2 && stats[0].successful_frames > 0 && stats[1].successful_frames > 0) {
    const auto first_frame_skew = stats[0].first_host_timestamp - stats[1].first_host_timestamp;
    std::cout
        << "First-frame host skew: " << std::setprecision(3)
        << std::chrono::duration<double, std::micro>(AbsoluteDuration(first_frame_skew)).count()
        << " us (diagnostic only; cameras are free-running)\n";
  }
  return passed;
}

int RunProbe(const Options &options) {
  const std::vector<UsbDevice> usb_devices = FindDahengUsbDevices();
  PrintUsbDevices(usb_devices);

  GalaxySdk sdk;
  const std::vector<DiscoveredCamera> discovered = sdk.Discover(std::chrono::seconds(1));
  const std::vector<std::string> selected_serials = SelectSerials(options, discovered);
  const TopologyResult topology = EvaluateTopology(options, usb_devices, selected_serials);
  std::cout << "Topology: " << topology.message << " pass=" << (topology.passed ? "yes" : "no")
            << '\n';

  ConfiguredCapture configured = ConfigureCameras(sdk, options, selected_serials);
  CaptureRun run = CaptureAllCameras(options, configured);
  const std::optional<std::filesystem::path> json_output =
      WriteDiagnosticFrames(options, configured, run);
  const bool passed = EvaluateAndPrint(options, topology, configured, run.stats);
  const std::string json =
      BuildJson(options, usb_devices, topology, configured.diagnostics, run.stats,
                run.resident_bytes_before_capture, run.resident_bytes_after_capture, passed);
  if (json_output.has_value()) {
    WriteTextFileAtomically(json_output.value(), json);
    std::cout << "JSON: " << json_output->string() << '\n';
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char **argv) {
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  const std::optional<std::filesystem::path> failure_json_output = FindJsonOutputPath(arguments);
  try {
    Options options = ParseOptions(arguments);
    if (options.show_help) {
      PrintUsage(arguments.front());
      return EXIT_SUCCESS;
    }
    if (!options.json_output.has_value()) {
      if (const auto test_outputs = EnvironmentValue("TEST_UNDECLARED_OUTPUTS_DIR");
          test_outputs.has_value() && !test_outputs->empty()) {
        options.json_output = std::filesystem::path(*test_outputs) / "report.json";
      }
    }
    return RunProbe(options);
  } catch (const std::exception &error) {
    std::cerr << "camera_probe: " << error.what() << '\n';
    if (failure_json_output.has_value()) {
      try {
        WriteTextFileAtomically(*failure_json_output, BuildFatalErrorJson(error.what()));
        std::cerr << "JSON: " << failure_json_output->string() << '\n';
      } catch (const std::exception &report_error) {
        std::cerr << "camera_probe: cannot write failure report: " << report_error.what() << '\n';
      }
    }
    return EXIT_FAILURE;
  }
}
