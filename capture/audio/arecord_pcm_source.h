#ifndef SWING_CAPTURE_CAPTURE_AUDIO_ARECORD_PCM_SOURCE_H_
#define SWING_CAPTURE_CAPTURE_AUDIO_ARECORD_PCM_SOURCE_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swing_capture {

struct ArecordPcmConfig {
  std::string capture_executable = "/usr/bin/arecord";
  std::string device = "default";
  std::uint32_t sample_rate_hz = 32000;
  std::uint16_t channel_count = 2;
  std::uint16_t selected_channel = 0;
  std::size_t frames_per_block = 1024;
  std::chrono::milliseconds read_timeout = std::chrono::seconds(2);
};

struct MonoPcmBlock {
  std::vector<std::int16_t> samples;

  // This is a host steady-clock estimate, not an ALSA hardware timestamp.
  // The first block is placed at read-completion minus its duration. Later
  // blocks advance continuously by sample count. Device buffering, arecord
  // buffering, pipe scheduling, and oscillator drift therefore remain
  // unmeasured. A production trigger path must calibrate that fixed latency or
  // replace this provisional source with an ALSA timestamped backend.
  std::chrono::steady_clock::time_point estimated_start_time;
  std::uint64_t first_frame_index = 0;
};

enum class PcmReadStatus {
  kData,
  kEndOfStream,
  kError,
};

struct PcmReadResult {
  PcmReadStatus status = PcmReadStatus::kError;
  MonoPcmBlock block;
  std::string message;
};

// Provisional Linux PCM source that executes arecord directly, without a
// command shell. The child writes raw interleaved S16_LE PCM to a private pipe;
// ReadBlock converts one configured channel to host int16 mono samples.
//
// This class is single-threaded. Start, ReadBlock, and Stop must all be called
// from the same controlling thread. The destructor closes the pipe, terminates
// a still-running child, and reaps it.
class ArecordPcmSource final {
 public:
  explicit ArecordPcmSource(ArecordPcmConfig config = {});
  ~ArecordPcmSource();

  ArecordPcmSource(const ArecordPcmSource &) = delete;
  ArecordPcmSource &operator=(const ArecordPcmSource &) = delete;
  ArecordPcmSource(ArecordPcmSource &&) = delete;
  ArecordPcmSource &operator=(ArecordPcmSource &&) = delete;

  [[nodiscard]] bool Start(std::string *error);
  [[nodiscard]] PcmReadResult ReadBlock();
  void Stop() noexcept;

  [[nodiscard]] bool running() const;
  [[nodiscard]] const ArecordPcmConfig &config() const;
  [[nodiscard]] std::uint64_t frames_read() const;

 private:
  [[nodiscard]] PcmReadResult ReadFailure(std::string message);
  [[nodiscard]] bool WaitForChildExit(std::chrono::milliseconds timeout, int *status) noexcept;
  void CloseReadPipe() noexcept;

  ArecordPcmConfig config_;
  int read_fd_ = -1;
  int child_pid_ = -1;
  std::vector<std::uint8_t> interleaved_bytes_;
  std::uint64_t frames_read_ = 0;
  bool has_stream_time_ = false;
  std::chrono::steady_clock::time_point next_block_start_;
};

}  // namespace swing_capture

#endif  // SWING_CAPTURE_CAPTURE_AUDIO_ARECORD_PCM_SOURCE_H_
