package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.inference.PoseModelVariant;
import java.util.Objects;

/** Validated knobs available only to the manual pose standby experiment entry point. */
record PoseExperimentConfiguration(
    PoseModelVariant modelVariant, int standbyWidth, int standbyHeight) {
  static final int MINIMUM_WIDTH = 160;
  static final int MAXIMUM_WIDTH = 1280;
  static final int MINIMUM_HEIGHT = 90;
  static final int MAXIMUM_HEIGHT = 720;

  PoseExperimentConfiguration {
    Objects.requireNonNull(modelVariant, "modelVariant");
    if (standbyWidth < MINIMUM_WIDTH
        || standbyWidth > MAXIMUM_WIDTH
        || standbyHeight < MINIMUM_HEIGHT
        || standbyHeight > MAXIMUM_HEIGHT
        || (standbyWidth & 1) != 0
        || (standbyHeight & 1) != 0
        || (long) standbyWidth * 9 != (long) standbyHeight * 16) {
      throw new IllegalArgumentException(
          "pose experiment standby input must be an even 16:9 size from 160x90 through 1280x720");
    }
  }

  static PoseExperimentConfiguration parse(String modelVariant, int width, int height) {
    return new PoseExperimentConfiguration(PoseModelVariant.parse(modelVariant), width, height);
  }
}
