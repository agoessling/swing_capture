package com.agoessling.swingcapture;

import java.util.Objects;

/** Manifest contract for one completed transition into continuous high-speed capture. */
public record CaptureStartupTimingManifest(
    CaptureStartupTiming timing,
    long startupContinuityResetCount,
    long maximumStartupContinuityGapNs) {
  public static final int SCHEMA_VERSION = 1;
  public static final String MANIFEST_FIELD_NAME = "startup_timing";

  public CaptureStartupTimingManifest {
    Objects.requireNonNull(timing, "timing");
    if (startupContinuityResetCount < 0 || maximumStartupContinuityGapNs < 0) {
      throw new IllegalArgumentException("startup continuity evidence cannot be negative");
    }
    if ((startupContinuityResetCount == 0) != (maximumStartupContinuityGapNs == 0)) {
      throw new IllegalArgumentException(
          "startup continuity resets and maximum gap must either both be zero or both be present");
    }
  }

  /** Canonical compact JSON with all 64-bit values represented as decimal strings. */
  public String toCanonicalJson() {
    StringBuilder json = new StringBuilder(768);
    json.append("{\"schema_version\":").append(SCHEMA_VERSION);
    appendString(json, "clock", "CLOCK_BOOTTIME");
    appendDecimalString(json, "arm_requested_elapsed_realtime_ns", timing.armRequestedNs());
    appendDecimalString(json, "engine_started_elapsed_realtime_ns", timing.engineStartedNs());
    appendDecimalString(
        json, "first_camera_frame_elapsed_realtime_ns", timing.firstCameraFrameNs());
    appendDecimalString(
        json,
        "first_usable_encoded_frame_elapsed_realtime_ns",
        timing.firstUsableEncodedFrameNs());
    appendDecimalString(
        json, "full_pre_roll_ready_elapsed_realtime_ns", timing.fullPreRollReadyNs());
    appendDecimalString(json, "arm_to_engine_start_ns", timing.armToEngineStartNs());
    appendDecimalString(
        json,
        "engine_start_to_first_camera_frame_ns",
        timing.engineStartToFirstCameraFrameNs());
    appendDecimalString(
        json, "arm_to_first_camera_frame_ns", timing.armToFirstCameraFrameNs());
    appendDecimalString(
        json,
        "first_camera_frame_to_first_usable_encoded_frame_ns",
        timing.firstCameraFrameToFirstUsableEncodedFrameNs());
    appendDecimalString(
        json,
        "arm_to_first_usable_encoded_frame_ns",
        timing.armToFirstUsableEncodedFrameNs());
    appendDecimalString(
        json,
        "first_usable_encoded_frame_to_full_pre_roll_ready_ns",
        timing.firstUsableEncodedFrameToFullPreRollReadyNs());
    appendDecimalString(
        json, "arm_to_full_pre_roll_ready_ns", timing.armToFullPreRollReadyNs());
    appendDecimalString(
        json, "startup_continuity_reset_count", startupContinuityResetCount);
    appendDecimalString(
        json, "maximum_startup_continuity_gap_ns", maximumStartupContinuityGapNs);
    return json.append('}').toString();
  }

  private static void appendDecimalString(StringBuilder json, String name, long value) {
    appendString(json, name, Long.toString(value));
  }

  private static void appendString(StringBuilder json, String name, String value) {
    json.append(",\"").append(name).append("\":\"").append(value).append('"');
  }
}
