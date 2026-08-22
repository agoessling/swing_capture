package com.agoessling.swingcapture;

import com.agoessling.swingcapture.audio.ImpactDetector;

public final class DeviceAudioDetectorPolicyTest {
  private DeviceAudioDetectorPolicyTest() {}

  public static void main(String[] args) {
    pixel5aChangesOnlyMinimumPeakAmplitude();
    matchingNormalizesCaseOnly();
    modelNameMustBeExact();
    pixel6AndUnknownDevicesUseExactDefaults();
  }

  private static void pixel5aChangesOnlyMinimumPeakAmplitude() {
    ImpactDetector.Config defaults = ImpactDetector.Config.defaults();
    ImpactDetector.Config config = DeviceAudioDetectorPolicy.forDevice("Google", "Pixel 5a");

    checkFloat(config.minimumPeakAmplitude(), 0.010f, "Pixel 5a minimum peak");
    checkFloat(
        config.thresholdMultiplier(), defaults.thresholdMultiplier(), "threshold multiplier");
    checkFloat(config.initialNoiseFloor(), defaults.initialNoiseFloor(), "initial noise floor");
    checkFloat(
        config.noiseUpdateClipMultiplier(),
        defaults.noiseUpdateClipMultiplier(),
        "noise update clip multiplier");
    check(
        Double.doubleToLongBits(config.noiseFloorTimeConstantSeconds())
            == Double.doubleToLongBits(defaults.noiseFloorTimeConstantSeconds()),
        "noise floor time constant");
    check(
        config.peakConfirmationFrames() == defaults.peakConfirmationFrames(),
        "peak confirmation frames");
    check(config.cooldownFrames() == defaults.cooldownFrames(), "cooldown frames");
  }

  private static void matchingNormalizesCaseOnly() {
    ImpactDetector.Config config = DeviceAudioDetectorPolicy.forDevice("gOoGlE", "pIxEl 5A");
    checkFloat(config.minimumPeakAmplitude(), 0.010f, "case-normalized Pixel 5a minimum");
  }

  private static void modelNameMustBeExact() {
    ImpactDetector.Config defaults = ImpactDetector.Config.defaults();
    for (String model : new String[] {"Pixel 5", "Pixel 5a ", "Pixel 5a (5G)", "Pixel 5a XL"}) {
      check(
          DeviceAudioDetectorPolicy.forDevice("Google", model).equals(defaults),
          "nearby model must use defaults: " + model);
    }
    check(
        DeviceAudioDetectorPolicy.forDevice("Other", "Pixel 5a").equals(defaults),
        "non-Google Pixel 5a model string must use defaults");
  }

  private static void pixel6AndUnknownDevicesUseExactDefaults() {
    ImpactDetector.Config defaults = ImpactDetector.Config.defaults();
    check(
        DeviceAudioDetectorPolicy.forDevice("Google", "Pixel 6").equals(defaults),
        "Pixel 6 must use exact defaults");
    checkFloat(defaults.minimumPeakAmplitude(), 0.015f, "default minimum peak");
    check(
        DeviceAudioDetectorPolicy.forDevice(null, null).equals(defaults),
        "unknown device must use exact defaults");
    check(
        DeviceAudioDetectorPolicy.forDevice("Google", "").equals(defaults),
        "empty model must use exact defaults");
  }

  private static void checkFloat(float actual, float expected, String label) {
    check(Float.floatToIntBits(actual) == Float.floatToIntBits(expected), label);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
