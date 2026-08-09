#include "capture/preview/camera_settings.h"

#include <algorithm>
#include <cmath>

namespace swing_capture::preview {

bool SettingValidationResult::valid() const noexcept {
  return error == SettingValidationError::kNone;
}

bool CameraSettingsValidation::valid() const noexcept {
  return exposure_microseconds.valid() && gain_db.valid();
}

SettingValidationResult ValidateSetting(double requested_value,
                                        const NumericSettingRange &range) noexcept {
  if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum) ||
      !std::isfinite(range.increment) || range.minimum > range.maximum || range.increment <= 0.0) {
    return {.error = SettingValidationError::kInvalidRange};
  }
  if (!std::isfinite(requested_value)) {
    return {.error = SettingValidationError::kNotFinite};
  }
  if (requested_value < range.minimum) {
    return {.error = SettingValidationError::kBelowMinimum};
  }
  if (requested_value > range.maximum) {
    return {.error = SettingValidationError::kAboveMaximum};
  }

  const double step_count = (requested_value - range.minimum) / range.increment;
  const double nearest_step = std::round(step_count);
  const double tolerance = 1e-9 * std::max(1.0, std::abs(step_count));
  if (std::abs(step_count - nearest_step) > tolerance) {
    return {.error = SettingValidationError::kNotAlignedToIncrement};
  }
  return {};
}

CameraSettingsValidation ValidateCameraSettings(const CameraSettingsRequest &request,
                                                const CameraSettingRanges &ranges) noexcept {
  return {
      .exposure_microseconds =
          ValidateSetting(request.exposure_microseconds, ranges.exposure_microseconds),
      .gain_db = ValidateSetting(request.gain_db, ranges.gain_db),
  };
}

}  // namespace swing_capture::preview
