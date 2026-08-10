#ifndef SWING_CAPTURE_CAPTURE_OPTICAL_WHITE_IMPACT_ACCEPTANCE_H_
#define SWING_CAPTURE_CAPTURE_OPTICAL_WHITE_IMPACT_ACCEPTANCE_H_

#include <cstddef>

namespace swing_capture::optical {

// Camera-SDK-independent qualification policy shared by the optical analyzer,
// session publisher, and serialized-artifact validator. Keep this deliberately
// limited to the metrics that determine white-impact acceptance; color distance
// remains a diagnostic behind matching_frame_count/matching_fraction.
struct WhiteImpactAcceptancePolicy {
  std::size_t minimum_stable_frames;
  double minimum_matching_fraction;
  double minimum_signal_delta;
  double maximum_saturated_fraction;
  double maximum_bloom_fraction;

  [[nodiscard]] constexpr bool Accepts(std::size_t stable_frame_count, double matching_fraction,
                                       double mean_signal_delta, double maximum_saturated,
                                       double maximum_bloom) const {
    return stable_frame_count >= minimum_stable_frames &&
           matching_fraction >= minimum_matching_fraction &&
           mean_signal_delta >= minimum_signal_delta &&
           maximum_saturated <= maximum_saturated_fraction &&
           maximum_bloom <= maximum_bloom_fraction;
  }
};

inline constexpr WhiteImpactAcceptancePolicy kDefaultWhiteImpactAcceptancePolicy{
    .minimum_stable_frames = 2,
    .minimum_matching_fraction = 0.75,
    .minimum_signal_delta = 12.0,
    .maximum_saturated_fraction = 0.08,
    .maximum_bloom_fraction = 0.08,
};

static_assert(kDefaultWhiteImpactAcceptancePolicy.Accepts(2, 0.75, 12.0, 0.08, 0.08));
static_assert(!kDefaultWhiteImpactAcceptancePolicy.Accepts(1, 0.75, 12.0, 0.08, 0.08));
static_assert(!kDefaultWhiteImpactAcceptancePolicy.Accepts(2, 0.74, 12.0, 0.08, 0.08));
static_assert(!kDefaultWhiteImpactAcceptancePolicy.Accepts(2, 0.75, 11.99, 0.08, 0.08));
static_assert(!kDefaultWhiteImpactAcceptancePolicy.Accepts(2, 0.75, 12.0, 0.081, 0.08));
static_assert(!kDefaultWhiteImpactAcceptancePolicy.Accepts(2, 0.75, 12.0, 0.08, 0.081));

}  // namespace swing_capture::optical

#endif  // SWING_CAPTURE_CAPTURE_OPTICAL_WHITE_IMPACT_ACCEPTANCE_H_
