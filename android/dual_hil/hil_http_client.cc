#include "android/dual_hil/hil_http_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>        // NOLINT(misc-include-cleaner)
#include <sys/socket.h>  // NOLINT(misc-include-cleaner)
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace swing_capture::android::dual_hil {
namespace {

// clang-tidy's include database cannot attribute the POSIX poll/socket declarations to their
// platform headers even though they are included directly above.
// NOLINTBEGIN(misc-include-cleaner)
void WaitForSocket(int descriptor, std::int16_t events,
                   std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd polled = {.fd = descriptor, .events = events, .revents = 0};
    const int result = poll(
        &polled, 1, static_cast<int>(std::min(remaining, std::chrono::milliseconds(100)).count()));
    if (result > 0 && (polled.revents & events) != 0) {
      return;
    }
    if (result > 0 && (polled.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      throw std::runtime_error("Android node HTTP socket failed");
    }
    if (result < 0 && errno != EINTR) {
      throw std::runtime_error("cannot poll Android node HTTP socket: " +
                               std::error_code(errno, std::generic_category()).message());
    }
  }
  throw std::runtime_error("Android node HTTP request exceeded its HIL deadline");
}

int Connect(const HilHttpRequest &request) {
  const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (descriptor < 0) {
    throw std::runtime_error("cannot create Android node HTTP socket");
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(request.port);
  const std::string host(request.ipv4_host);
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    close(descriptor);
    throw std::invalid_argument("Android node HTTP host must be a numeric IPv4 address");
  }
  // POSIX exposes protocol-specific socket addresses through sockaddr pointers.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *socket_address = reinterpret_cast<const sockaddr *>(&address);
  if (connect(descriptor, socket_address, sizeof(address)) != 0) {
    if (errno != EINPROGRESS) {
      close(descriptor);
      throw std::runtime_error("cannot connect to Android node HTTP endpoint");
    }
    try {
      WaitForSocket(descriptor, POLLOUT, request.deadline);
      int socket_error = 0;
      socklen_t size = sizeof(socket_error);
      if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &size) != 0 ||
          socket_error != 0) {
        throw std::runtime_error("cannot establish Android node HTTP endpoint");
      }
    } catch (...) {
      close(descriptor);
      throw;
    }
  }
  return descriptor;
}

void SendAll(int descriptor, std::string_view wire,
             std::chrono::steady_clock::time_point deadline) {
  std::size_t sent = 0;
  while (sent < wire.size()) {
    const std::string_view remaining = wire.substr(sent);
    const ssize_t count = send(descriptor, remaining.data(), remaining.size(), MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw std::runtime_error("cannot send Android node HTTP request");
    } else {
      WaitForSocket(descriptor, POLLOUT, deadline);
    }
  }
}

std::string ReceiveAll(int descriptor, std::chrono::steady_clock::time_point deadline) {
  constexpr std::size_t kMaximumResponseBytes = 2U * std::size_t{1024} * std::size_t{1024};
  std::string wire;
  while (true) {
    std::array<char, 8192> buffer = {};
    const ssize_t count = recv(descriptor, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      wire.append(buffer.data(), static_cast<std::size_t>(count));
      if (wire.size() > kMaximumResponseBytes) {
        throw std::runtime_error("Android node HTTP response is unexpectedly large");
      }
    } else if (count == 0) {
      return wire;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      throw std::runtime_error("cannot receive Android node HTTP response");
    } else {
      WaitForSocket(descriptor, POLLIN, deadline);
    }
  }
}
// NOLINTEND(misc-include-cleaner)

std::string Lowercase(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return lowered;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1U);
  }
  return value;
}

void RequireHeaderSafe(std::string_view name, std::string_view value) {
  if (name.empty() || name.find_first_of("\r\n:") != std::string_view::npos ||
      value.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("Android node HTTP header is unsafe");
  }
}

}  // namespace

HilHttpResponse ParseHilHttpResponse(std::string_view wire, bool head) {
  const std::size_t status_end = wire.find("\r\n");
  const std::size_t header_end = wire.find("\r\n\r\n");
  const std::size_t first_space = wire.find(' ');
  if (!wire.starts_with("HTTP/1.") || status_end == std::string_view::npos ||
      header_end == std::string_view::npos || status_end >= header_end ||
      first_space == std::string_view::npos || first_space + 4U > status_end) {
    throw std::runtime_error("Android node returned a malformed HTTP response");
  }
  const std::size_t status_start = first_space + 1U;
  HilHttpResponse response;
  const std::string_view status_text = wire.substr(status_start, 3U);
  const auto [status_pointer, status_error] =
      std::from_chars(status_text.begin(), status_text.end(), response.status);
  if (status_error != std::errc() || status_pointer != status_text.end()) {
    throw std::runtime_error("Android node HTTP status is invalid");
  }
  std::size_t line_start = status_end + 2U;
  while (line_start < header_end) {
    const std::size_t line_end = wire.find("\r\n", line_start);
    const std::size_t separator = wire.find(':', line_start);
    if (line_end == std::string_view::npos || separator == std::string_view::npos ||
        separator >= line_end) {
      throw std::runtime_error("Android node HTTP header is malformed");
    }
    response.headers.emplace(Lowercase(wire.substr(line_start, separator - line_start)),
                             Trim(wire.substr(separator + 1U, line_end - separator - 1U)));
    line_start = line_end + 2U;
  }
  response.body = wire.substr(header_end + 4U);
  const auto length = response.headers.find("content-length");
  std::size_t advertised = 0;
  if (length == response.headers.end()) {
    throw std::runtime_error("Android node HTTP response lacks Content-Length");
  }
  const std::string_view length_text = length->second;
  const auto [length_pointer, length_error] =
      std::from_chars(length_text.begin(), length_text.end(), advertised);
  if (length_error != std::errc() || length_pointer != length_text.end() ||
      (!head && advertised != response.body.size()) || (head && !response.body.empty())) {
    throw std::runtime_error("Android node HTTP Content-Length is inconsistent");
  }
  return response;
}

HilHttpResponse RequestHilHttp(const HilHttpRequest &request) {
  if (request.ipv4_host.empty() || request.port == 0 || request.method.empty() ||
      !request.path.starts_with('/') ||
      request.method.find_first_of(" \r\n") != std::string_view::npos ||
      request.path.find_first_of(" \r\n") != std::string_view::npos ||
      request.bearer_token.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("Android node HTTP request is incomplete or unsafe");
  }
  std::string wire = std::string(request.method) + " " + std::string(request.path) +
                     " HTTP/1.1\r\nHost: " + std::string(request.ipv4_host) + ":" +
                     std::to_string(request.port) + "\r\nAccept: application/json\r\n";
  if (!request.bearer_token.empty()) {
    wire += "Authorization: Bearer " + std::string(request.bearer_token) + "\r\n";
  }
  for (const auto &[name, value] : request.headers) {
    RequireHeaderSafe(name, value);
    wire.append(name);
    wire.append(": ");
    wire.append(value);
    wire.append("\r\n");
  }
  if (!request.body.empty()) {
    wire +=
        "Content-Type: application/json\r\nContent-Length: " + std::to_string(request.body.size()) +
        "\r\n";
  }
  wire += "Connection: close\r\n\r\n";
  wire.append(request.body);
  const int descriptor = Connect(request);
  try {
    SendAll(descriptor, wire, request.deadline);
    const std::string response = ReceiveAll(descriptor, request.deadline);
    close(descriptor);
    return ParseHilHttpResponse(response, request.method == "HEAD");
  } catch (...) {
    close(descriptor);
    throw;
  }
}

HilHttpResponse RequireHilHttp(const HilHttpRequest &request, int expected_status) {
  HilHttpResponse response = RequestHilHttp(request);
  if (response.status != expected_status) {
    throw std::runtime_error("Android node " + std::string(request.method) + " " +
                             std::string(request.path) + " returned HTTP " +
                             std::to_string(response.status) + ": " + response.body);
  }
  return response;
}

}  // namespace swing_capture::android::dual_hil
