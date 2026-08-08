#include "capture/audio/arecord_pcm_source.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace swing_capture {
namespace {

constexpr std::size_t kBytesPerSample = sizeof(std::int16_t);

void SetError(std::string *destination, std::string message) {
  if (destination != nullptr) {
    *destination = std::move(message);
  }
}

std::string ErrnoMessage(std::string_view operation, int error_number) {
  return std::string(operation) + ": " +
         std::error_code(error_number, std::generic_category()).message();
}

std::string ChildStatusMessage(int status) {
  // Clang's include-cleaner cannot associate these POSIX status macros with
  // the directly included <sys/wait.h> public header.
  // NOLINTBEGIN(misc-include-cleaner)
  if (WIFEXITED(status)) {
    return "capture process exited with status " + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "capture process was terminated by signal " + std::to_string(WTERMSIG(status));
  }
  // NOLINTEND(misc-include-cleaner)
  return "capture process ended with an unknown status";
}

std::string WaitUntilReadable(int file_descriptor, std::chrono::steady_clock::time_point deadline) {
  // Clang's include-cleaner does not identify <poll.h> as the public provider
  // for these POSIX declarations, despite the direct include above.
  // NOLINTBEGIN(misc-include-cleaner)
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return "timed out waiting for a complete PCM block";
    }
    const auto remaining_milliseconds =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    const auto poll_timeout = static_cast<int>(
        std::min<std::int64_t>(remaining_milliseconds, std::numeric_limits<int>::max()));
    pollfd descriptor = {
        .fd = file_descriptor,
        .events = POLLIN,
        .revents = 0,
    };
    const int poll_result = poll(&descriptor, 1, poll_timeout);
    if (poll_result == 0) {
      return "timed out waiting for a complete PCM block";
    }
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ErrnoMessage("poll", errno);
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
      return "PCM pipe became invalid";
    }
    if ((descriptor.revents & POLLERR) != 0) {
      return "PCM pipe reported an I/O error";
    }
    return {};
  }
  // NOLINTEND(misc-include-cleaner)
}

struct PipeReadResult {
  std::size_t bytes_read = 0;
  bool reached_eof = false;
  std::string error;
};

PipeReadResult ReadFromPipe(int file_descriptor, std::span<std::uint8_t> destination,
                            std::chrono::steady_clock::time_point deadline) {
  PipeReadResult result;
  while (result.bytes_read < destination.size()) {
    result.error = WaitUntilReadable(file_descriptor, deadline);
    if (!result.error.empty()) {
      return result;
    }

    ssize_t count = -1;
    do {
      const std::span<std::uint8_t> remaining = destination.subspan(result.bytes_read);
      count = read(file_descriptor, remaining.data(), remaining.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
      result.error = ErrnoMessage("read", errno);
      return result;
    }
    if (count == 0) {
      result.reached_eof = true;
      return result;
    }
    result.bytes_read += static_cast<std::size_t>(count);
  }
  return result;
}

std::chrono::steady_clock::duration FramesToDuration(std::uint64_t frames,
                                                     std::uint32_t sample_rate_hz) {
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(static_cast<double>(frames) /
                                    static_cast<double>(sample_rate_hz)));
}

void ValidateConfig(const ArecordPcmConfig &config) {
  if (config.capture_executable.empty()) {
    throw std::invalid_argument("capture_executable cannot be empty");
  }
  if (config.device.empty()) {
    throw std::invalid_argument("device cannot be empty");
  }
  if (config.sample_rate_hz == 0) {
    throw std::invalid_argument("sample_rate_hz must be positive");
  }
  if (config.channel_count == 0) {
    throw std::invalid_argument("channel_count must be positive");
  }
  if (config.selected_channel >= config.channel_count) {
    throw std::invalid_argument("selected_channel must be less than channel_count");
  }
  if (config.frames_per_block == 0) {
    throw std::invalid_argument("frames_per_block must be positive");
  }
  if (config.read_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("read_timeout must be positive");
  }
  if (config.frames_per_block >
      std::numeric_limits<std::size_t>::max() /
          (static_cast<std::size_t>(config.channel_count) * kBytesPerSample)) {
    throw std::invalid_argument("configured PCM block is too large");
  }
}

int ConfigureSpawnFileActions(posix_spawn_file_actions_t *actions,
                              const std::array<int, 2> &pipe_fds) {
  int error = posix_spawn_file_actions_adddup2(actions, pipe_fds[1], STDOUT_FILENO);
  if (error == 0) {
    error = posix_spawn_file_actions_addclose(actions, pipe_fds[0]);
  }
  if (error == 0) {
    error = posix_spawn_file_actions_addclose(actions, pipe_fds[1]);
  }
  return error;
}

}  // namespace

ArecordPcmSource::ArecordPcmSource(ArecordPcmConfig config) : config_(std::move(config)) {
  ValidateConfig(config_);
  interleaved_bytes_.resize(config_.frames_per_block *
                            static_cast<std::size_t>(config_.channel_count) * kBytesPerSample);
}

ArecordPcmSource::~ArecordPcmSource() { Stop(); }

bool ArecordPcmSource::Start(std::string *error) {
  if (child_pid_ > 0 || read_fd_ >= 0) {
    SetError(error, "audio capture is already running");
    return false;
  }

  std::array<int, 2> pipe_fds = {-1, -1};
  if (pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
    SetError(error, ErrnoMessage("pipe2", errno));
    return false;
  }

  posix_spawn_file_actions_t actions;
  int spawn_error = posix_spawn_file_actions_init(&actions);
  if (spawn_error != 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    SetError(error, ErrnoMessage("posix_spawn_file_actions_init", spawn_error));
    return false;
  }

  const auto destroy_actions = [&actions]() { posix_spawn_file_actions_destroy(&actions); };
  spawn_error = ConfigureSpawnFileActions(&actions, pipe_fds);
  if (spawn_error != 0) {
    destroy_actions();
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    SetError(error, ErrnoMessage("posix_spawn file action setup", spawn_error));
    return false;
  }

  std::vector<std::string> arguments = {
      config_.capture_executable,
      "--quiet",
      "--file-type",
      "raw",
      "--format",
      "S16_LE",
      "--rate",
      std::to_string(config_.sample_rate_hz),
      "--channels",
      std::to_string(config_.channel_count),
      "--device",
      config_.device,
      "-",
  };
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (std::string &argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  // Clang's include-cleaner does not identify <sys/types.h> as the public
  // provider for this POSIX type.
  pid_t pid = -1;  // NOLINT(misc-include-cleaner)
  spawn_error = posix_spawnp(&pid, config_.capture_executable.c_str(), &actions, nullptr,
                             argv.data(), environ);
  destroy_actions();
  close(pipe_fds[1]);
  if (spawn_error != 0) {
    close(pipe_fds[0]);
    SetError(error, ErrnoMessage("posix_spawnp", spawn_error));
    return false;
  }

  read_fd_ = pipe_fds[0];
  child_pid_ = static_cast<int>(pid);
  frames_read_ = 0;
  has_stream_time_ = false;
  next_block_start_ = {};
  SetError(error, {});
  return true;
}

PcmReadResult ArecordPcmSource::ReadBlock() {
  if (child_pid_ <= 0 || read_fd_ < 0) {
    return {
        .status = PcmReadStatus::kError,
        .block = {},
        .message = "audio capture is not running",
    };
  }

  const auto read_deadline = std::chrono::steady_clock::now() + config_.read_timeout;
  const PipeReadResult pipe_read = ReadFromPipe(read_fd_, interleaved_bytes_, read_deadline);
  if (!pipe_read.error.empty()) {
    return ReadFailure(pipe_read.error);
  }
  const std::size_t bytes_read = pipe_read.bytes_read;
  const bool reached_eof = pipe_read.reached_eof;
  const auto read_complete_time = std::chrono::steady_clock::now();

  const std::size_t bytes_per_frame =
      static_cast<std::size_t>(config_.channel_count) * kBytesPerSample;
  if (bytes_read % bytes_per_frame != 0) {
    return ReadFailure("capture process ended in the middle of an interleaved PCM frame");
  }

  if (bytes_read == 0 && reached_eof) {
    int child_status = 0;
    if (!WaitForChildExit(std::chrono::milliseconds(100), &child_status)) {
      return ReadFailure("capture process closed its PCM stream without exiting");
    }
    CloseReadPipe();
    if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0) {
      return {
          .status = PcmReadStatus::kEndOfStream,
          .block = {},
          .message = {},
      };
    }
    return {
        .status = PcmReadStatus::kError,
        .block = {},
        .message = ChildStatusMessage(child_status),
    };
  }

  const std::size_t frame_count = bytes_read / bytes_per_frame;
  MonoPcmBlock block;
  block.samples.resize(frame_count);
  block.first_frame_index = frames_read_;
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    const std::size_t offset =
        (frame * static_cast<std::size_t>(config_.channel_count) + config_.selected_channel) *
        kBytesPerSample;
    const std::uint16_t bits = static_cast<std::uint16_t>(interleaved_bytes_[offset]) |
                               (static_cast<std::uint16_t>(interleaved_bytes_[offset + 1]) << 8U);
    block.samples[frame] = std::bit_cast<std::int16_t>(bits);
  }

  if (!has_stream_time_) {
    next_block_start_ = read_complete_time - FramesToDuration(frame_count, config_.sample_rate_hz);
    has_stream_time_ = true;
  }
  block.estimated_start_time = next_block_start_;
  next_block_start_ += FramesToDuration(frame_count, config_.sample_rate_hz);
  frames_read_ += frame_count;

  return {
      .status = PcmReadStatus::kData,
      .block = std::move(block),
      .message = reached_eof ? "final partial PCM block" : std::string(),
  };
}

void ArecordPcmSource::Stop() noexcept {
  CloseReadPipe();
  if (child_pid_ <= 0) {
    return;
  }

  const auto pid = static_cast<pid_t>(child_pid_);
  // Clang's include-cleaner cannot associate these POSIX process declarations
  // and signal constants with the direct headers above.
  // NOLINTBEGIN(misc-include-cleaner)
  if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
    // Continue to wait/reap. Stop is noexcept and there is no useful recovery
    // for callers during destruction.
  }

  int status = 0;
  if (!WaitForChildExit(std::chrono::milliseconds(500), &status)) {
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
      // A subsequent waitpid still determines whether the child is ours.
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    child_pid_ = -1;
  }
  // NOLINTEND(misc-include-cleaner)
}

bool ArecordPcmSource::running() const { return child_pid_ > 0 && read_fd_ >= 0; }

const ArecordPcmConfig &ArecordPcmSource::config() const { return config_; }

std::uint64_t ArecordPcmSource::frames_read() const { return frames_read_; }

PcmReadResult ArecordPcmSource::ReadFailure(std::string message) {
  Stop();
  return {
      .status = PcmReadStatus::kError,
      .block = {},
      .message = std::move(message),
  };
}

bool ArecordPcmSource::WaitForChildExit(std::chrono::milliseconds timeout, int *status) noexcept {
  if (child_pid_ <= 0) {
    return true;
  }
  const auto pid = static_cast<pid_t>(child_pid_);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const pid_t wait_result = waitpid(pid, status, WNOHANG);  // NOLINT(misc-include-cleaner)
    if (wait_result == pid) {
      child_pid_ = -1;
      return true;
    }
    if (wait_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        child_pid_ = -1;
        return true;
      }
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void ArecordPcmSource::CloseReadPipe() noexcept {
  if (read_fd_ >= 0) {
    close(read_fd_);
    read_fd_ = -1;
  }
}

}  // namespace swing_capture
