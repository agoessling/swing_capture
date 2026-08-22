#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "web/memory_usage_parser.h"
#include "web/process_lifecycle.h"

namespace {

constexpr std::uint64_t kMaximumMemoryBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr auto kGracefulShutdownTimeout = std::chrono::milliseconds(500);
volatile sig_atomic_t termination_signal = 0;

struct ProcessGroupUsage {
  std::uint64_t memory_bytes = 0;
  std::size_t process_count = 0;
};

void RecordTerminationSignal(int signal) { termination_signal = signal; }

bool InstallSignalHandler(int signal) {
  struct sigaction action{};
  action.sa_handler = RecordTerminationSignal;
  sigemptyset(&action.sa_mask);
  return sigaction(signal, &action, nullptr) == 0;
}

std::optional<pid_t> ReadProcessGroup(const std::filesystem::path &process_directory) {
  std::ifstream input(process_directory / "stat");
  std::string record;
  if (!std::getline(input, record)) {
    return std::nullopt;
  }
  const std::size_t command_end = record.rfind(')');
  if (command_end == std::string::npos || command_end + 2 >= record.size()) {
    return std::nullopt;
  }
  std::istringstream fields(record.substr(command_end + 2));
  char state = '\0';
  pid_t parent = 0;
  pid_t process_group = 0;
  if (!(fields >> state >> parent >> process_group)) {
    return std::nullopt;
  }
  return process_group;
}

std::optional<std::uint64_t> ReadMemoryBytes(const std::filesystem::path &process_directory) {
  std::ifstream input(process_directory / "status");
  if (!input) {
    return std::nullopt;
  }
  return swing_capture::web::ParseProcStatusMemoryBytes(input);
}

ProcessGroupUsage ReadProcessGroupUsage(pid_t process_group) {
  ProcessGroupUsage usage;
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator("/proc", error)) {
    if (error) {
      usage.memory_bytes = std::numeric_limits<std::uint64_t>::max();
      return usage;
    }
    const std::string name = entry.path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos ||
        ReadProcessGroup(entry.path()) != process_group) {
      continue;
    }
    ++usage.process_count;
    const std::optional<std::uint64_t> memory = ReadMemoryBytes(entry.path());
    if (!memory.has_value()) {
      continue;
    }
    if (*memory > std::numeric_limits<std::uint64_t>::max() - usage.memory_bytes) {
      usage.memory_bytes = std::numeric_limits<std::uint64_t>::max();
      return usage;
    }
    usage.memory_bytes += *memory;
  }
  return usage;
}

void SignalProcessGroup(pid_t process_group, int signal) {
  if (kill(-process_group, signal) != 0 && errno != ESRCH) {
    std::cerr << "Unable to signal component-test process group\n";
  }
}

void StopRemainingProcessGroup(pid_t process_group) {
  if (ReadProcessGroupUsage(process_group).process_count == 0) {
    return;
  }
  SignalProcessGroup(process_group, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + kGracefulShutdownTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReadProcessGroupUsage(process_group).process_count == 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  SignalProcessGroup(process_group, SIGKILL);
}

void StopAndReapChild(pid_t child, int signal) {
  SignalProcessGroup(child, signal);
  const auto deadline = std::chrono::steady_clock::now() + kGracefulShutdownTimeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t wait_result = waitpid(child, &status, WNOHANG);
    if (wait_result == child || (wait_result < 0 && errno == ECHILD)) {
      StopRemainingProcessGroup(child);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  SignalProcessGroup(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  StopRemainingProcessGroup(child);
}

int ChildExitCode(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

}  // namespace

int main(int argument_count, char **arguments) {
  if (argument_count < 2) {
    std::cerr << "Expected a Bazel test-executable runfile argument\n";
    return 2;
  }
  const std::filesystem::path executable = std::filesystem::absolute(arguments[1]);
  if (!std::filesystem::is_regular_file(executable) || access(executable.c_str(), X_OK) != 0) {
    std::cerr << "Test runfile is not executable: " << executable << '\n';
    return 2;
  }
  if (!InstallSignalHandler(SIGINT) || !InstallSignalHandler(SIGTERM) ||
      !InstallSignalHandler(SIGHUP)) {
    std::cerr << "Unable to install component-test termination handlers\n";
    return 2;
  }

  const pid_t wrapper = getpid();
  const pid_t child = fork();
  if (child < 0) {
    std::cerr << "Unable to fork the component-test process\n";
    return 2;
  }
  if (child == 0) {
    if (!swing_capture::web::ArmParentDeathSignal(wrapper)) {
      std::cerr << "Unable to bind component-test lifetime to its wrapper\n";
      _exit(2);
    }
    if (setpgid(0, 0) != 0) {
      std::cerr << "Unable to isolate the component-test process group\n";
      _exit(2);
    }
    std::vector<char *> child_arguments(arguments + 1, arguments + argument_count);
    child_arguments.push_back(nullptr);
    execv(executable.c_str(), child_arguments.data());
    std::cerr << "Unable to execute component test: " << executable << '\n';
    _exit(1);
  }
  if (setpgid(child, child) != 0 && getpgid(child) != child) {
    std::cerr << "Unable to monitor the component-test process group\n";
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    return 2;
  }

  while (true) {
    if (termination_signal != 0) {
      const int signal = termination_signal;
      StopAndReapChild(child, signal);
      return 128 + signal;
    }
    int status = 0;
    const pid_t wait_result = waitpid(child, &status, WNOHANG);
    if (wait_result == child) {
      StopRemainingProcessGroup(child);
      return ChildExitCode(status);
    }
    if (wait_result < 0) {
      std::cerr << "Unable to observe the component-test process\n";
      return 2;
    }
    const ProcessGroupUsage usage = ReadProcessGroupUsage(child);
    if (usage.memory_bytes > kMaximumMemoryBytes) {
      std::cerr << "Component test exceeded the 1 GiB RSS-plus-swap safety limit (observed "
                << usage.memory_bytes << " bytes across " << usage.process_count << " processes)\n";
      StopAndReapChild(child, SIGKILL);
      return 137;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
