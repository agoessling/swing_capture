package com.agoessling.swingcapture;

import com.agoessling.swingcapture.audio.ImpactDetector;
import java.util.Locale;

/** Selects the complete audio-impact detector tuning for one Android device model. */
final class DeviceAudioDetectorPolicy {
  private static final String GOOGLE_MANUFACTURER = "google";
  private static final String PIXEL_5A_MODEL = "pixel 5a";
  // The maximum-safe Feather fixture tone has measured 0.0115-0.0122 on the Pixel 5a. Keep
  // device-specific margin below that range while retaining the adaptive 8x-noise-floor gate.
  private static final float PIXEL_5A_MINIMUM_PEAK_AMPLITUDE = 0.010f;

  private DeviceAudioDetectorPolicy() {}

  static ImpactDetector.Config forDevice(String manufacturer, String model) {
    ImpactDetector.Config defaults = ImpactDetector.Config.defaults();
    if (!GOOGLE_MANUFACTURER.equals(normalizeCase(manufacturer))
        || !PIXEL_5A_MODEL.equals(normalizeCase(model))) {
      return defaults;
    }
    return new ImpactDetector.Config(
        defaults.thresholdMultiplier(),
        PIXEL_5A_MINIMUM_PEAK_AMPLITUDE,
        defaults.initialNoiseFloor(),
        defaults.noiseUpdateClipMultiplier(),
        defaults.noiseFloorTimeConstantSeconds(),
        defaults.peakConfirmationFrames(),
        defaults.cooldownFrames());
  }

  private static String normalizeCase(String value) {
    return value == null ? "" : value.toLowerCase(Locale.ROOT);
  }
}
