#include "capture/hil/feather_hil_serial.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "capture/hil/feather_hil_protocol.h"

namespace swing_capture::hil {
namespace {

constexpr std::size_t kMaximumBufferedBytes = 4096;

std::runtime_error SystemFailure(std::string_view operation) {
  return std::runtime_error(std::string(operation) + ": " +
                            std::error_code(errno, std::generic_category()).message());
}

int RemainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
}

// Clang's include-cleaner cannot associate these POSIX declarations with the
// directly included <poll.h> public header.
// NOLINTBEGIN(misc-include-cleaner)
void WaitFor(int file_descriptor, int events, std::chrono::steady_clock::time_point deadline,
             std::string_view operation) {
  while (true) {
    const int remaining = RemainingMilliseconds(deadline);
    if (remaining == 0) {
      throw std::runtime_error(std::string(operation) + " timed out");
    }
    pollfd descriptor = {
        .fd = file_descriptor,
        .events = static_cast<std::int16_t>(events),
        .revents = 0,
    };
    const int result = poll(&descriptor, 1, remaining);
    if (result > 0) {
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error(std::string(operation) + " failed because the serial link closed");
      }
      if ((descriptor.revents & events) != 0) {
        return;
      }
      continue;
    }
    if (result == 0) {
      throw std::runtime_error(std::string(operation) + " timed out");
    }
    if (errno != EINTR) {
      throw SystemFailure(operation);
    }
  }
}
// NOLINTEND(misc-include-cleaner)

int OpenDevice(const std::filesystem::path &device_path) {
  // The two-argument form does not consume a variadic mode argument.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int file_descriptor = open(device_path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
  if (file_descriptor < 0) {
    throw SystemFailure("open Feather serial " + device_path.string());
  }
  return file_descriptor;
}

void ConfigureRawUsbCdc(int file_descriptor) {
  termios attributes = {};
  if (tcgetattr(file_descriptor, &attributes) != 0) {
    throw SystemFailure("tcgetattr Feather serial");
  }
  cfmakeraw(&attributes);
  attributes.c_cflag |= CLOCAL | CREAD;
  if (cfsetispeed(&attributes, B115200) != 0 || cfsetospeed(&attributes, B115200) != 0 ||
      tcsetattr(file_descriptor, TCSANOW, &attributes) != 0) {
    throw SystemFailure("configure Feather serial");
  }
}

}  // namespace

FeatherHilSerial::FeatherHilSerial(const std::filesystem::path &device_path)
    : file_descriptor_(OpenDevice(device_path)) {
  try {
    ConfigureRawUsbCdc(file_descriptor_);
  } catch (...) {
    close(file_descriptor_);
    file_descriptor_ = -1;
    throw;
  }
}

FeatherHilSerial::~FeatherHilSerial() {
  if (file_descriptor_ >= 0) {
    close(file_descriptor_);
  }
}

void FeatherHilSerial::Write(std::string_view command, std::chrono::milliseconds timeout) const {
  if (command.empty() || command.back() != '\n' ||
      command.substr(0, command.size() - 1).contains('\n')) {
    throw std::invalid_argument("Feather HIL command must contain exactly one trailing newline");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t written = 0;
  while (written < command.size()) {
    WaitFor(file_descriptor_, POLLOUT, deadline,  // NOLINT(misc-include-cleaner)
            "write Feather serial");
    const ssize_t count =
        write(file_descriptor_, command.data() + written, command.size() - written);  // NOLINT
    if (count > 0) {
      written += static_cast<std::size_t>(count);
    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw SystemFailure("write Feather serial");
    }
  }
}

FeatherResponse FeatherHilSerial::Read(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (const std::size_t newline = receive_buffer_.find('\n'); newline != std::string::npos) {
      const std::string line = receive_buffer_.substr(0, newline + 1);
      receive_buffer_.erase(0, newline + 1);
      return ParseFeatherResponse(line);
    }
    WaitFor(file_descriptor_, POLLIN, deadline,  // NOLINT(misc-include-cleaner)
            "read Feather serial");
    std::array<char, 256> incoming = {};
    const ssize_t count = read(file_descriptor_, incoming.data(), incoming.size());
    if (count > 0) {
      receive_buffer_.append(incoming.data(), static_cast<std::size_t>(count));
      if (receive_buffer_.size() > kMaximumBufferedBytes) {
        throw std::runtime_error("Feather serial response exceeded buffer limit");
      }
    } else if (count == 0) {
      throw std::runtime_error("Feather serial link reached end of stream");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw SystemFailure("read Feather serial");
    }
  }
}

}  // namespace swing_capture::hil
