#include "capture/preview/camera_settings.h"

#include <cassert>
#include <limits>

namespace {

using swing_capture::preview::CameraSettingRanges;
using swing_capture::preview::CameraSettingsRequest;
using swing_capture::preview::NumericSettingRange;
using swing_capture::preview::SettingValidationError;
using swing_capture::preview::ValidateCameraSettings;
using swing_capture::preview::ValidateSetting;

void AcceptsBoundsAndIncrementAlignedValues() {
  const NumericSettingRange range = {.minimum = 10.0, .maximum = 20.0, .increment = 0.5};
  assert(ValidateSetting(10.0, range).valid());
  assert(ValidateSetting(12.5, range).valid());
  assert(ValidateSetting(20.0, range).valid());

  const NumericSettingRange fractional = {
      .minimum = 0.1,
      .maximum = 1.0,
      .increment = 0.1,
  };
  assert(ValidateSetting(0.1 + 0.2, fractional).valid());
}

void ReportsSpecificValidationFailures() {
  const NumericSettingRange range = {.minimum = 10.0, .maximum = 20.0, .increment = 0.5};
  assert(ValidateSetting(9.5, range).error == SettingValidationError::kBelowMinimum);
  assert(ValidateSetting(20.5, range).error == SettingValidationError::kAboveMaximum);
  assert(ValidateSetting(12.25, range).error == SettingValidationError::kNotAlignedToIncrement);
  assert(ValidateSetting(std::numeric_limits<double>::infinity(), range).error ==
         SettingValidationError::kNotFinite);
}

void RejectsMalformedRanges() {
  assert(ValidateSetting(5.0, {.minimum = 10.0, .maximum = 1.0, .increment = 1.0}).error ==
         SettingValidationError::kInvalidRange);
  assert(ValidateSetting(5.0, {.minimum = 1.0, .maximum = 10.0, .increment = 0.0}).error ==
         SettingValidationError::kInvalidRange);
  assert(ValidateSetting(5.0,
                         {
                             .minimum = 1.0,
                             .maximum = std::numeric_limits<double>::quiet_NaN(),
                             .increment = 1.0,
                         })
             .error == SettingValidationError::kInvalidRange);
}

void ValidatesExposureAndGainTogether() {
  const CameraSettingRanges ranges = {
      .exposure_microseconds = {.minimum = 10.0, .maximum = 1000.0, .increment = 5.0},
      .gain_db = {.minimum = 0.0, .maximum = 24.0, .increment = 0.1},
  };
  const auto valid = ValidateCameraSettings(
      CameraSettingsRequest{.exposure_microseconds = 500.0, .gain_db = 6.5}, ranges);
  assert(valid.valid());

  const auto invalid = ValidateCameraSettings(
      CameraSettingsRequest{.exposure_microseconds = 501.0, .gain_db = 30.0}, ranges);
  assert(!invalid.valid());
  assert(invalid.exposure_microseconds.error == SettingValidationError::kNotAlignedToIncrement);
  assert(invalid.gain_db.error == SettingValidationError::kAboveMaximum);
}

}  // namespace

int main() {
  AcceptsBoundsAndIncrementAlignedValues();
  ReportsSpecificValidationFailures();
  RejectsMalformedRanges();
  ValidatesExposureAndGainTogether();
  return 0;
}
