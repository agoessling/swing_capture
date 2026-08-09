#ifndef SWING_CAPTURE_CAPTURE_PREVIEW_CAMERA_SETTINGS_H_
#define SWING_CAPTURE_CAPTURE_PREVIEW_CAMERA_SETTINGS_H_

namespace swing_capture::preview {

struct NumericSettingRange {
  double minimum = 0.0;
  double maximum = 0.0;
  double increment = 0.0;
};

struct CameraSettingsRequest {
  double exposure_microseconds = 0.0;
  double gain_db = 0.0;
};

struct CameraSettingRanges {
  NumericSettingRange exposure_microseconds;
  NumericSettingRange gain_db;
};

enum class SettingValidationError {
  kNone,
  kInvalidRange,
  kNotFinite,
  kBelowMinimum,
  kAboveMaximum,
  kNotAlignedToIncrement,
};

struct SettingValidationResult {
  SettingValidationError error = SettingValidationError::kNone;

  [[nodiscard]] bool valid() const noexcept;
};

struct CameraSettingsValidation {
  SettingValidationResult exposure_microseconds;
  SettingValidationResult gain_db;

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] SettingValidationResult ValidateSetting(double requested_value,
                                                      const NumericSettingRange &range) noexcept;

[[nodiscard]] CameraSettingsValidation ValidateCameraSettings(
    const CameraSettingsRequest &request, const CameraSettingRanges &ranges) noexcept;

}  // namespace swing_capture::preview

#endif  // SWING_CAPTURE_CAPTURE_PREVIEW_CAMERA_SETTINGS_H_
