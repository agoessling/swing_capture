#include <httplib.h>
// POSIX signal masks and sigtimedwait are not exposed by <csignal>.
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "capture/service/preview_api.h"
#include "capture/service/preview_station.h"
#include "station/hardware_lock.h"
#include "station/station_config.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace {

using bazel::tools::cpp::runfiles::Runfiles;
using swing_capture::service::PreviewStation;
using swing_capture::station::HardwareLock;

constexpr std::string_view kHardwareLockEnvironment = "SWING_CAPTURE_HARDWARE_LOCK";

struct Options {
  std::string bind_address = "0.0.0.0";
  int port = 8080;
  std::optional<std::filesystem::path> station_config;
  std::optional<std::filesystem::path> static_root;
  std::optional<std::filesystem::path> hardware_lock;
  std::optional<std::filesystem::path> sessions_root;
  bool enable_hil_controls = false;
};

class TerminationSignalMask final {
 public:
  TerminationSignalMask() {
    if (sigemptyset(&signals_) != 0 || sigaddset(&signals_, SIGINT) != 0 ||
        sigaddset(&signals_, SIGTERM) != 0) {
      throw std::runtime_error("cannot initialize termination signal set");
    }
    const int error = pthread_sigmask(SIG_BLOCK, &signals_, &previous_);
    if (error != 0) {
      throw std::runtime_error("cannot block termination signals: " + ErrorMessage(error));
    }
    active_ = true;
  }

  ~TerminationSignalMask() {
    if (active_) {
      static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
    }
  }

  TerminationSignalMask(const TerminationSignalMask &) = delete;
  TerminationSignalMask &operator=(const TerminationSignalMask &) = delete;
  TerminationSignalMask(TerminationSignalMask &&) = delete;
  TerminationSignalMask &operator=(TerminationSignalMask &&) = delete;

  // sigset_t is a POSIX type exposed by <signal.h>; include-cleaner's standard
  // provider map does not attribute the typedef to that header.
  // NOLINTNEXTLINE(misc-include-cleaner)
  [[nodiscard]] const sigset_t &signals() const noexcept { return signals_; }

 private:
  static std::string ErrorMessage(int error_number) {
    return std::error_code(error_number, std::generic_category()).message();
  }

  // NOLINTBEGIN(misc-include-cleaner)
  sigset_t signals_{};
  sigset_t previous_{};
  // NOLINTEND(misc-include-cleaner)
  bool active_ = false;
};

std::string RequireValue(std::span<char *> arguments, std::size_t *index, std::string_view option) {
  if (*index + 1 >= arguments.size()) {
    throw std::invalid_argument(std::string(option) + " requires a value");
  }
  ++*index;
  return arguments[*index];
}

Options ParseOptions(std::span<char *> arguments) {
  Options options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "--bind") {
      options.bind_address = RequireValue(arguments, &index, argument);
    } else if (argument == "--port") {
      options.port = std::stoi(RequireValue(arguments, &index, argument));
    } else if (argument == "--station-config") {
      options.station_config = RequireValue(arguments, &index, argument);
    } else if (argument == "--static-root") {
      options.static_root = RequireValue(arguments, &index, argument);
    } else if (argument == "--hardware-lock") {
      options.hardware_lock = RequireValue(arguments, &index, argument);
    } else if (argument == "--sessions-root") {
      options.sessions_root = RequireValue(arguments, &index, argument);
    } else if (argument == "--enable-hil-controls") {
      options.enable_hil_controls = true;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }
  if (options.bind_address.empty()) {
    throw std::invalid_argument("--bind must not be empty");
  }
  if (options.port < 1 || options.port > 65535) {
    throw std::invalid_argument("--port must be between 1 and 65535");
  }
  return options;
}

std::filesystem::path ResolveStationConfig(const Options &options) {
  if (options.station_config.has_value()) {
    return std::filesystem::absolute(*options.station_config);
  }
  const std::optional<std::filesystem::path> environment =
      swing_capture::station::StationConfigPathFromEnvironment();
  if (environment.has_value()) {
    return std::filesystem::absolute(*environment);
  }
  // Bazel sets this for `bazel run`; read it before any camera worker exists.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  if (workspace != nullptr && *workspace != '\0') {
    return std::filesystem::path(workspace) / ".station.local.conf";
  }
  return std::filesystem::current_path() / ".station.local.conf";
}

std::filesystem::path ResolveStaticRoot(const Options &options, const char *program) {
  if (options.static_root.has_value()) {
    return std::filesystem::absolute(*options.static_root);
  }
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::Create(program, &error));
  if (runfiles == nullptr) {
    throw std::runtime_error("cannot initialize Bazel runfiles: " + error);
  }
  const std::filesystem::path root = runfiles->Rlocation("swing_capture/web/static_app");
  if (!std::filesystem::is_regular_file(root / "index.html")) {
    throw std::runtime_error("cannot locate web/static_app/index.html in Bazel runfiles");
  }
  return root;
}

std::filesystem::path ResolveWorkspaceRoot() {
  // Bazel sets this for `bazel run`; read it before any worker threads exist.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  if (workspace != nullptr && *workspace != '\0') {
    return std::filesystem::absolute(workspace);
  }
  for (std::filesystem::path candidate = std::filesystem::current_path(); !candidate.empty();
       candidate = candidate.parent_path()) {
    if (std::filesystem::is_regular_file(candidate / "MODULE.bazel")) {
      return candidate;
    }
    if (candidate == candidate.root_path()) {
      break;
    }
  }
  throw std::runtime_error(
      "cannot locate the repository; pass --hardware-lock for an installed service");
}

std::filesystem::path ResolveHardwareLock(const Options &options) {
  if (options.hardware_lock.has_value()) {
    return std::filesystem::absolute(*options.hardware_lock);
  }
  // Read the deployment override before any worker threads exist.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *environment = std::getenv(std::string(kHardwareLockEnvironment).c_str());
  if (environment != nullptr && *environment != '\0') {
    return std::filesystem::absolute(environment);
  }
  return ResolveWorkspaceRoot() / "artifacts" / "hil" / "hardware.lock";
}

std::filesystem::path ResolveSessionsRoot(const Options &options) {
  return options.sessions_root.has_value() ? std::filesystem::absolute(*options.sessions_root)
                                           : ResolveWorkspaceRoot() / "artifacts" / "sessions";
}

int ListenUntilTerminated(httplib::Server &server, const TerminationSignalMask &signal_mask,
                          std::string_view bind_address, int port) {
  std::atomic<int> received_signal = 0;
  std::jthread signal_waiter(
      [&server, &signal_mask, &received_signal](const std::stop_token &stop_token) {
        constexpr timespec kPollInterval = {.tv_sec = 0, .tv_nsec = 100'000'000};
        while (!stop_token.stop_requested()) {
          const int signal = sigtimedwait(&signal_mask.signals(), nullptr, &kPollInterval);
          if (signal == SIGINT || signal == SIGTERM) {
            received_signal.store(signal);
            server.stop();
            return;
          }
          if (signal < 0 && errno != EAGAIN && errno != EINTR) {
            return;
          }
        }
      });

  const bool listened = server.listen(std::string(bind_address), port);
  signal_waiter.request_stop();
  signal_waiter.join();
  if (received_signal.load() != 0) {
    std::cout << "Termination signal received; cameras stopped cleanly.\n";
    return 0;
  }
  if (!listened) {
    throw std::runtime_error("preview HTTP listener failed");
  }
  return 0;
}

int RunServer(std::span<char *> arguments) {
  const Options options = ParseOptions(arguments);
  const std::filesystem::path config_path = ResolveStationConfig(options);
  const std::filesystem::path static_root = ResolveStaticRoot(options, arguments.front());
  const std::filesystem::path lock_path = ResolveHardwareLock(options);
  const std::filesystem::path sessions_root = ResolveSessionsRoot(options);

  const TerminationSignalMask signal_mask;
  const swing_capture::station::StationConfig config =
      swing_capture::station::LoadStationConfig(config_path);
  const HardwareLock hardware_lock(lock_path);
  PreviewStation station(config, sessions_root, options.enable_hil_controls);
  httplib::Server server;
  swing_capture::service::RegisterPreviewRoutes(server, station, static_root);

  std::cout << "Swing Capture preview: http://" << options.bind_address << ':' << options.port
            << "/\n"
            << "Station config: " << config_path << '\n'
            << "Static assets: " << static_root << '\n'
            << "Sessions: " << sessions_root << '\n'
            << "HIL controls: " << (options.enable_hil_controls ? "enabled" : "disabled") << '\n'
            << "Hardware lock: " << lock_path << '\n';
  return ListenUntilTerminated(server, signal_mask, options.bind_address, options.port);
}

}  // namespace

int main(int argc, char **argv) {
  try {
    return RunServer(std::span<char *>(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception &error) {
    std::cerr << "preview_server: " << error.what() << '\n';
    return 1;
  }
}
