#include "android/dual_hil/hil_command.h"

#include <fcntl.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>  // NOLINT(misc-include-cleaner) - POSIX kill uses SIGKILL.
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace swing_capture::android::dual_hil {

namespace {

std::string SystemErrorMessage(const int error) {
  return std::error_code(error, std::generic_category()).message();
}

}  // namespace

// The lifecycle is intentionally linear so every post-fork descriptor and child cleanup path is
// visible together.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
HilCommandResult RunHilCommand(const std::filesystem::path &executable,
                               const std::vector<std::string> &arguments,
                               std::chrono::steady_clock::time_point deadline,
                               std::string_view input) {
  std::array<int, 2> output_pipe = {-1, -1};
  std::array<int, 2> input_pipe = {-1, -1};
  if (pipe2(output_pipe.data(), O_CLOEXEC) != 0) {
    throw std::runtime_error(std::string("cannot create command output pipe: ") +
                             SystemErrorMessage(errno));
  }
  if (pipe2(input_pipe.data(), O_CLOEXEC) != 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error(std::string("cannot create command input pipe: ") +
                             SystemErrorMessage(saved_errno));
  }
  const auto child = fork();
  if (child < 0) {
    const int saved_errno = errno;
    close(output_pipe[0]);
    close(output_pipe[1]);
    close(input_pipe[0]);
    close(input_pipe[1]);
    throw std::runtime_error(std::string("cannot fork command: ") +
                             SystemErrorMessage(saved_errno));
  }
  if (child == 0) {
    close(output_pipe[0]);
    close(input_pipe[1]);
    if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(output_pipe[1], STDERR_FILENO) < 0 ||
        dup2(input_pipe[0], STDIN_FILENO) < 0) {
      _exit(126);
    }
    close(output_pipe[1]);
    close(input_pipe[0]);
    std::vector<std::string> command_storage;
    command_storage.reserve(arguments.size() + 1U);
    command_storage.push_back(executable.string());
    command_storage.insert(command_storage.end(), arguments.begin(), arguments.end());
    std::vector<char *> command;
    command.reserve(arguments.size() + 2U);
    for (std::string &argument : command_storage) {
      command.push_back(argument.data());
    }
    command.push_back(nullptr);
    execv(executable.c_str(), command.data());
    _exit(127);
  }

  close(output_pipe[1]);
  close(input_pipe[0]);
  std::size_t input_offset = 0;
  while (input_offset < input.size()) {
    const std::string_view remaining = input.substr(input_offset);
    const auto count = write(input_pipe[1], remaining.data(), remaining.size());
    if (count > 0) {
      input_offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    const int saved_errno = errno;
    close(input_pipe[1]);
    close(output_pipe[0]);
    static_cast<void>(kill(child, SIGKILL));  // NOLINT(misc-include-cleaner)
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    throw std::runtime_error(std::string("cannot write command input: ") +
                             SystemErrorMessage(saved_errno));
  }
  close(input_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (flags >= 0) {
    // POSIX fcntl exposes its command-dependent third argument through varargs.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    static_cast<void>(fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK));
  }
  HilCommandResult result;
  int status = 0;
  bool exited = false;
  bool end_of_output = false;
  while (!exited || !end_of_output) {
    std::array<char, 4096> buffer = {};
    while (true) {
      const auto count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        end_of_output = true;
      }
      break;
    }
    if (!exited) {
      const auto waited = waitpid(child, &status, WNOHANG);  // NOLINT(misc-include-cleaner)
      if (waited == child) {
        exited = true;
      } else if (waited < 0 && errno != EINTR) {
        const int saved_errno = errno;
        close(output_pipe[0]);
        throw std::runtime_error(std::string("cannot wait for command: ") +
                                 SystemErrorMessage(saved_errno));
      }
    }
    if (exited && end_of_output) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (!exited) {
        static_cast<void>(kill(child, SIGKILL));  // NOLINT(misc-include-cleaner)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
      }
      result.timed_out = true;
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptor = {.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    const int polled =
        poll(&descriptor, 1,
             static_cast<int>(std::min(remaining, std::chrono::milliseconds(50)).count()));
    if (polled < 0 && errno != EINTR) {
      const int saved_errno = errno;
      if (!exited) {
        static_cast<void>(kill(child, SIGKILL));  // NOLINT(misc-include-cleaner)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
      }
      close(output_pipe[0]);
      throw std::runtime_error(std::string("cannot poll command output: ") +
                               SystemErrorMessage(saved_errno));
    }
  }
  close(output_pipe[0]);
  if (!result.timed_out) {
    // The wait-status accessors are POSIX macros that include-cleaner cannot attribute.
    result.exit_code = WIFEXITED(status)              // NOLINT(misc-include-cleaner)
                           ? WEXITSTATUS(status)      // NOLINT(misc-include-cleaner)
                           : 128 + WTERMSIG(status);  // NOLINT(misc-include-cleaner)
  }
  return result;
}

std::string RunRequiredHilCommand(const std::filesystem::path &executable,
                                  const std::vector<std::string> &arguments,
                                  std::chrono::steady_clock::time_point deadline) {
  const HilCommandResult result = RunHilCommand(executable, arguments, deadline);
  if (result.timed_out) {
    throw std::runtime_error(executable.filename().string() + " exceeded its stage deadline");
  }
  if (result.exit_code != 0) {
    throw std::runtime_error(executable.filename().string() + " failed with exit " +
                             std::to_string(result.exit_code) + ": " + result.output);
  }
  return result.output;
}

}  // namespace swing_capture::android::dual_hil
