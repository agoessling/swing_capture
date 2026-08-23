package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.inference.PoseModelVariant;

public final class PoseExperimentConfigurationTest {
  public static void main(String[] args) {
    PoseExperimentConfiguration lite =
        PoseExperimentConfiguration.parse(" LITE ", 640, 360);
    check(lite.modelVariant() == PoseModelVariant.LITE, "lite model parsed");
    check(lite.standbyWidth() == 640 && lite.standbyHeight() == 360, "default input parsed");

    PoseExperimentConfiguration heavy =
        PoseExperimentConfiguration.parse("heavy", 1280, 720);
    check(heavy.modelVariant() == PoseModelVariant.HEAVY, "heavy model parsed");

    expectInvalid(() -> PoseExperimentConfiguration.parse("unknown", 640, 360));
    expectInvalid(() -> PoseExperimentConfiguration.parse("full", 639, 360));
    expectInvalid(() -> PoseExperimentConfiguration.parse("full", 640, 352));
    expectInvalid(() -> PoseExperimentConfiguration.parse("full", 144, 90));
    expectInvalid(() -> PoseExperimentConfiguration.parse("full", 1296, 729));
  }

  private static void expectInvalid(Runnable action) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError("expected invalid experiment configuration");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
