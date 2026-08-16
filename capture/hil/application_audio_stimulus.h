#ifndef SWING_CAPTURE_CAPTURE_HIL_APPLICATION_AUDIO_STIMULUS_H_
#define SWING_CAPTURE_CAPTURE_HIL_APPLICATION_AUDIO_STIMULUS_H_

#include <chrono>
#include <cstdint>

namespace swing_capture::hil {

// Short fixture stimulus used to exercise the production audio-trigger path.
// The HIL-only level leaves margin for the current physical geometry while
// staying well below the firmware's conservative 125-permille ceiling. Ten
// milliseconds remains brief while clearing the 1.5 ms detector gate.
struct ApplicationAudioStimulus {
  std::chrono::microseconds lead = std::chrono::microseconds(100'000);
  std::chrono::microseconds duration = std::chrono::microseconds(10'000);
  std::uint32_t frequency_hz = 2'000;
  std::uint32_t level_permille = 125;
};

inline constexpr ApplicationAudioStimulus kApplicationAudioStimulus;

}  // namespace swing_capture::hil

#endif  // SWING_CAPTURE_CAPTURE_HIL_APPLICATION_AUDIO_STIMULUS_H_
