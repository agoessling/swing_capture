#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Options {
  std::uint32_t sample_rate_hz = 32000;
  std::uint16_t channel_count = 2;
  std::string device = "fake:frames=150";
};

bool ParseUnsigned(std::string_view text, std::uint64_t *value) {
  if (text.empty()) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const std::string owned(text);
  const auto parsed = std::strtoull(owned.c_str(), &end, 10);
  if (errno != 0 || end == owned.c_str() || *end != '\0') {
    return false;
  }
  *value = parsed;
  return true;
}

bool ParseOptions(std::span<char *> arguments, Options *options) {
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    const auto take_value = [&](std::string_view name, std::string *output) {
      if (argument != name || index + 1 >= arguments.size()) {
        return false;
      }
      *output = arguments[++index];
      return true;
    };

    std::string value;
    if (take_value("--device", &value)) {
      options->device = std::move(value);
      continue;
    }
    if (take_value("--rate", &value)) {
      std::uint64_t parsed = 0;
      if (!ParseUnsigned(value, &parsed) || parsed == 0 || parsed > UINT32_MAX) {
        return false;
      }
      options->sample_rate_hz = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (take_value("--channels", &value)) {
      std::uint64_t parsed = 0;
      if (!ParseUnsigned(value, &parsed) || parsed == 0 || parsed > UINT16_MAX) {
        return false;
      }
      options->channel_count = static_cast<std::uint16_t>(parsed);
      continue;
    }
  }
  return true;
}

bool WriteAll(const std::vector<std::uint8_t> &bytes) {
  std::span<const std::uint8_t> remaining(bytes);
  while (!remaining.empty()) {
    const ssize_t count = write(STDOUT_FILENO, remaining.data(), remaining.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    remaining = remaining.subspan(static_cast<std::size_t>(count));
  }
  return true;
}

struct PcmRequest {
  std::size_t first_frame;
  std::size_t frame_count;
  std::uint16_t channel_count;
};

std::vector<std::uint8_t> MakePcm(PcmRequest request) {
  std::vector<std::uint8_t> bytes(request.frame_count *
                                  static_cast<std::size_t>(request.channel_count) * 2);
  for (std::size_t frame = 0; frame < request.frame_count; ++frame) {
    const std::size_t stream_frame = request.first_frame + frame;
    for (std::size_t channel = 0; channel < request.channel_count; ++channel) {
      auto sample = channel == 0 ? static_cast<std::int16_t>(100) : static_cast<std::int16_t>(-200);
      if (channel == 1 && stream_frame == 50) {
        sample = static_cast<std::int16_t>(-30000);
      }
      const auto bits = static_cast<std::uint16_t>(sample);
      const std::size_t offset =
          (frame * static_cast<std::size_t>(request.channel_count) + channel) * 2;
      bytes[offset] = static_cast<std::uint8_t>(bits & 0xffU);
      bytes[offset + 1] = static_cast<std::uint8_t>(bits >> 8U);
    }
  }
  return bytes;
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!ParseOptions(std::span(argv, static_cast<std::size_t>(argc)), &options)) {
    return 64;
  }
  if (options.device == "fake:fail") {
    return 23;
  }
  if (options.device == "fake:stall") {
    while (true) {
      pause();
    }
  }

  if (options.device == "fake:continuous") {
    std::size_t first_frame = 0;
    while (true) {
      constexpr std::size_t kChunkFrames = 512;
      if (!WriteAll(MakePcm({
              .first_frame = first_frame,
              .frame_count = kChunkFrames,
              .channel_count = options.channel_count,
          }))) {
        return 0;
      }
      first_frame += kChunkFrames;
    }
  }

  if (options.device == "fake:dribble") {
    // Each write arrives comfortably inside the consumer's test timeout, but
    // completing the whole block takes much longer. This distinguishes one
    // absolute block deadline from an incorrectly reset per-read timeout.
    constexpr std::size_t kFrameCount = 30;
    for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
      if (!WriteAll(MakePcm({
              .first_frame = frame,
              .frame_count = 1,
              .channel_count = options.channel_count,
          }))) {
        return 0;
      }
      if (frame + 1 < kFrameCount) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    return 0;
  }

  constexpr std::string_view kFramesPrefix = "fake:frames=";
  std::size_t frame_count = 150;
  if (options.device.starts_with(kFramesPrefix)) {
    std::uint64_t parsed = 0;
    if (!ParseUnsigned(std::string_view(options.device).substr(kFramesPrefix.size()), &parsed)) {
      return 64;
    }
    frame_count = static_cast<std::size_t>(parsed);
  }
  return WriteAll(MakePcm({
             .first_frame = 0,
             .frame_count = frame_count,
             .channel_count = options.channel_count,
         }))
             ? 0
             : 74;
}
