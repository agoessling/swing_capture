#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>  // NOLINT(misc-include-cleaner)
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "capture/offline/experiments/envelope/envelope_experiment.h"
#include "capture/offline/experiments/envelope/streaming_envelope_detector.h"

namespace {

using swing_capture::offline::envelope::Candidate;
using swing_capture::offline::envelope::DetectorConfig;
using swing_capture::offline::envelope::RobustHp120X12Config;
using swing_capture::offline::envelope::StreamingEnvelopeDetector;

constexpr std::uint32_t kRequiredSampleRateHz = 48'000;
constexpr std::size_t kReplayBlockFrames = 4'096;

struct Attempt {
  std::string id;
  std::int64_t arm_ms = 0;
  std::int64_t end_ms = 0;
};

template <typename Integer>
Integer ReadLittleEndian(std::span<const std::byte> input, std::size_t offset) {
  static_assert(std::is_unsigned_v<Integer>);
  Integer result = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    result |= std::to_integer<Integer>(input[offset + index]) << (index * 8U);
  }
  return result;
}

bool Matches(std::span<const std::byte> input, std::size_t offset, std::string_view expected) {
  if (offset + expected.size() > input.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (input[offset + index] != static_cast<std::byte>(expected[index])) {
      return false;
    }
  }
  return true;
}

class MonoPcmS16Wav {
 public:
  explicit MonoPcmS16Wav(const std::string &path) : input_(path, std::ios::binary) {
    if (!input_) {
      throw std::runtime_error("failed to open WAV input");
    }
    Parse();
  }

  [[nodiscard]] std::uint32_t sample_rate_hz() const { return sample_rate_hz_; }
  [[nodiscard]] std::uint64_t frame_count() const { return data_bytes_ / 2U; }

  void Seek(std::uint64_t frame) {
    if (frame > frame_count()) {
      throw std::out_of_range("WAV frame seek exceeds data length");
    }
    const std::uint64_t byte_position = data_offset_ + frame * 2U;
    if (byte_position > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      throw std::overflow_error("WAV seek exceeds stream offset range");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(byte_position));
    if (!input_) {
      throw std::runtime_error("failed to seek WAV input");
    }
  }

  std::size_t Read(std::span<std::int16_t> output) {
    if (output.size() > byte_buffer_.size() / 2U) {
      throw std::length_error("WAV replay block exceeds the fixed read buffer");
    }
    const std::span<std::byte> bytes = std::span(byte_buffer_).first(output.size() * 2U);
    // std::istream's byte-oriented API requires a char pointer at this boundary.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto *byte_data = reinterpret_cast<char *>(bytes.data());
    input_.read(byte_data, static_cast<std::streamsize>(bytes.size()));
    const std::streamsize count = input_.gcount();
    if (count < 0 || count % 2 != 0) {
      throw std::runtime_error("WAV PCM read ended on a partial sample");
    }
    const auto frames = static_cast<std::size_t>(count) / 2U;
    for (std::size_t index = 0; index < frames; ++index) {
      output[index] =
          std::bit_cast<std::int16_t>(ReadLittleEndian<std::uint16_t>(bytes, index * 2U));
    }
    return frames;
  }

 private:
  void Parse() {
    std::array<std::byte, 12> header{};
    ReadExactly(header);
    if (!Matches(header, 0, "RIFF") || !Matches(header, 8, "WAVE")) {
      throw std::invalid_argument("input is not a RIFF/WAVE file");
    }
    bool format_seen = false;
    bool data_seen = false;
    while (!data_seen) {
      std::array<std::byte, 8> chunk{};
      ReadExactly(chunk);
      const auto length = ReadLittleEndian<std::uint32_t>(chunk, 4);
      if (Matches(chunk, 0, "fmt ")) {
        if (length < 16U) {
          throw std::invalid_argument("WAV fmt chunk is truncated");
        }
        std::array<std::byte, 16> format{};
        ReadExactly(format);
        Skip(length - format.size());
        const auto encoding = ReadLittleEndian<std::uint16_t>(format, 0);
        const auto channels = ReadLittleEndian<std::uint16_t>(format, 2);
        sample_rate_hz_ = ReadLittleEndian<std::uint32_t>(format, 4);
        const auto block_align = ReadLittleEndian<std::uint16_t>(format, 12);
        const auto bits_per_sample = ReadLittleEndian<std::uint16_t>(format, 14);
        if (encoding != 1U || channels != 1U || sample_rate_hz_ != kRequiredSampleRateHz ||
            block_align != 2U || bits_per_sample != 16U) {
          throw std::invalid_argument("WAV must be mono 48 kHz signed PCM16");
        }
        format_seen = true;
      } else if (Matches(chunk, 0, "data")) {
        if (!format_seen || length % 2U != 0U) {
          throw std::invalid_argument("WAV data precedes a valid format or has a partial sample");
        }
        const auto position = input_.tellg();
        if (position < 0) {
          throw std::runtime_error("failed to locate WAV PCM data");
        }
        data_offset_ = static_cast<std::uint64_t>(position);
        data_bytes_ = length;
        data_seen = true;
      } else {
        Skip(length);
      }
      if (!data_seen && (length & 1U) != 0U) {
        Skip(1U);
      }
    }
  }

  void ReadExactly(std::span<std::byte> output) {
    // std::istream's byte-oriented API requires a char pointer at this boundary.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto *byte_data = reinterpret_cast<char *>(output.data());
    input_.read(byte_data, static_cast<std::streamsize>(output.size()));
    if (input_.gcount() != static_cast<std::streamsize>(output.size())) {
      throw std::invalid_argument("WAV chunk is truncated");
    }
  }

  void Skip(std::uint64_t bytes) {
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      throw std::invalid_argument("WAV chunk is too large");
    }
    input_.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    if (!input_) {
      throw std::invalid_argument("WAV chunk extends past end of file");
    }
  }

  std::ifstream input_;
  std::uint32_t sample_rate_hz_ = 0;
  std::uint64_t data_offset_ = 0;
  std::uint64_t data_bytes_ = 0;
  std::array<std::byte, kReplayBlockFrames * 2U> byte_buffer_{};
};

// nlohmann's umbrella header is the stable public provider even though include-cleaner cannot
// attribute the generated aliases correctly.
// NOLINTNEXTLINE(misc-include-cleaner)
nlohmann::json ConfigJson(const DetectorConfig &config) {
  return {{"name", config.name},
          {"high_pass_hz", config.high_pass_hz},
          {"hop_us", config.hop_us},
          {"fast_window_us", config.fast_window_us},
          {"background_window_us", config.background_window_us},
          {"background_guard_us", config.background_guard_us},
          {"background_block_us", config.background_block_us},
          {"background_multiplier", config.background_multiplier},
          {"rise_ratio", config.rise_ratio},
          {"minimum_peak", config.minimum_peak},
          {"minimum_high_frequency_ratio", config.minimum_high_frequency_ratio},
          {"minimum_crest_factor", config.minimum_crest_factor},
          {"cluster_gap_us", config.cluster_gap_us},
          {"refractory_us", config.refractory_us}};
}

std::vector<Attempt> ReadAttempts() {
  std::vector<Attempt> attempts;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream fields(line);
    Attempt attempt;
    std::string arm;
    std::string end;
    if (!std::getline(fields, attempt.id, '\t') || !std::getline(fields, arm, '\t') ||
        !std::getline(fields, end) || attempt.id.empty() || arm.empty() || end.empty() ||
        end.contains('\t')) {
      throw std::invalid_argument("attempt input must be ID<TAB>ARM_MS<TAB>END_MS");
    }
    attempt.arm_ms = std::stoll(arm);
    attempt.end_ms = std::stoll(end);
    if (attempt.arm_ms < 0 || attempt.end_ms < attempt.arm_ms) {
      throw std::invalid_argument("attempt interval is invalid");
    }
    attempts.push_back(std::move(attempt));
  }
  return attempts;
}

// Both values are deliberately integral but have different units and signedness.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::uint64_t MillisecondsToFrame(std::int64_t milliseconds, std::uint64_t maximum) {
  if (milliseconds < 0 || static_cast<std::uint64_t>(milliseconds) >
                              std::numeric_limits<std::uint64_t>::max() / kRequiredSampleRateHz) {
    throw std::overflow_error("attempt timestamp exceeds audio frame range");
  }
  const std::uint64_t frame =
      static_cast<std::uint64_t>(milliseconds) * kRequiredSampleRateHz / 1'000U;
  return std::min(frame, maximum);
}

void AppendCandidate(std::ostream &output, const Candidate &candidate, bool &first) {
  if (!first) {
    output << ',';
  }
  first = false;
  output << "{\"strike_ms\":" << candidate.strike_us / 1'000
         << ",\"decision_ms\":" << candidate.decision_us / 1'000 << '}';
}

void Replay(MonoPcmS16Wav &wav, std::uint64_t first_frame, std::uint64_t end_frame,
            std::ostream &output) {
  if (end_frame < first_frame || end_frame > wav.frame_count()) {
    throw std::out_of_range("replay interval is outside WAV data");
  }
  wav.Seek(first_frame);
  StreamingEnvelopeDetector detector(wav.sample_rate_hz(), RobustHp120X12Config(), first_frame);
  std::array<std::int16_t, kReplayBlockFrames> buffer{};
  std::uint64_t remaining = end_frame - first_frame;
  bool first = true;
  output << '[';
  const auto emit = [&](const Candidate &candidate) { AppendCandidate(output, candidate, first); };
  while (remaining > 0U) {
    const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
    const std::size_t read = wav.Read(std::span(buffer).first(requested));
    if (read != requested) {
      throw std::invalid_argument("WAV PCM data is shorter than its declared length");
    }
    detector.Process(std::span(buffer).first(read), emit);
    remaining -= read;
  }
  detector.Finish(emit);
  output << ']';
}

// NOLINTNEXTLINE(misc-include-cleaner)
std::string EscapeJson(const std::string &value) { return nlohmann::json(value).dump(); }

void Predict(const std::string &wav_path) {
  MonoPcmS16Wav wav(wav_path);
  const std::vector<Attempt> attempts = ReadAttempts();
  std::cout << "{\"config\":" << ConfigJson(RobustHp120X12Config()).dump()
            << ",\"continuous_candidates\":";
  Replay(wav, 0, wav.frame_count(), std::cout);
  std::cout << ",\"armed_replays\":[";
  for (std::size_t index = 0; index < attempts.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    const Attempt &attempt = attempts[index];
    const std::uint64_t first_frame = MillisecondsToFrame(attempt.arm_ms, wav.frame_count());
    const std::uint64_t end_frame = MillisecondsToFrame(attempt.end_ms, wav.frame_count());
    std::cout << "{\"attempt_id\":" << EscapeJson(attempt.id) << ",\"arm_ms\":" << attempt.arm_ms
              << ",\"evaluation_end_ms\":" << attempt.end_ms << ",\"candidates\":";
    Replay(wav, first_frame, end_frame, std::cout);
    std::cout << '}';
  }
  std::cout << "]}\n";
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
    if (arguments.size() == 2U && std::string_view(arguments[1]) == "describe") {
      std::cout << ConfigJson(RobustHp120X12Config()).dump() << '\n';
      return 0;
    }
    if (arguments.size() == 3U && std::string_view(arguments[1]) == "predict") {
      Predict(arguments[2]);
      return 0;
    }
    std::cerr << "usage: streaming_envelope_replay describe | predict AUDIO.wav\n";
    return 2;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
