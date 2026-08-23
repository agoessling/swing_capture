package com.agoessling.swingcapture;

/** Deterministic tests for retained high-speed startup timing evidence. */
public final class CaptureStartupTimingManifestTest {
  private CaptureStartupTimingManifestTest() {}

  public static void main(String[] arguments) {
    serializesCanonicalContract();
    serializesContinuousStartupWithoutResets();
    rejectsInconsistentContinuityEvidence();
  }

  private static void serializesCanonicalContract() {
    CaptureStartupTimingManifest manifest =
        new CaptureStartupTimingManifest(
            new CaptureStartupTiming(100, 120, 210, 260, 2_800), 2, 900);

    check(
        manifest
            .toCanonicalJson()
            .equals(
                "{\"schema_version\":1,\"clock\":\"CLOCK_BOOTTIME\""
                    + ",\"arm_requested_elapsed_realtime_ns\":\"100\""
                    + ",\"engine_started_elapsed_realtime_ns\":\"120\""
                    + ",\"first_camera_frame_elapsed_realtime_ns\":\"210\""
                    + ",\"first_usable_encoded_frame_elapsed_realtime_ns\":\"260\""
                    + ",\"full_pre_roll_ready_elapsed_realtime_ns\":\"2800\""
                    + ",\"arm_to_engine_start_ns\":\"20\""
                    + ",\"engine_start_to_first_camera_frame_ns\":\"90\""
                    + ",\"arm_to_first_camera_frame_ns\":\"110\""
                    + ",\"first_camera_frame_to_first_usable_encoded_frame_ns\":\"50\""
                    + ",\"arm_to_first_usable_encoded_frame_ns\":\"160\""
                    + ",\"first_usable_encoded_frame_to_full_pre_roll_ready_ns\":\"2540\""
                    + ",\"arm_to_full_pre_roll_ready_ns\":\"2700\""
                    + ",\"startup_continuity_reset_count\":\"2\""
                    + ",\"maximum_startup_continuity_gap_ns\":\"900\"}"),
        "canonical startup timing JSON");
  }

  private static void serializesContinuousStartupWithoutResets() {
    CaptureStartupTimingManifest manifest =
        new CaptureStartupTimingManifest(new CaptureStartupTiming(5, 5, 5, 5, 5), 0, 0);

    check(
        manifest
            .toCanonicalJson()
            .endsWith(
                "\"startup_continuity_reset_count\":\"0\","
                    + "\"maximum_startup_continuity_gap_ns\":\"0\"}"),
        "zero continuity evidence remains explicit");
  }

  private static void rejectsInconsistentContinuityEvidence() {
    CaptureStartupTiming timing = new CaptureStartupTiming(1, 2, 3, 4, 5);
    expectThrows(() -> new CaptureStartupTimingManifest(timing, -1, 0), "negative count");
    expectThrows(() -> new CaptureStartupTimingManifest(timing, 0, 1), "gap without reset");
    expectThrows(() -> new CaptureStartupTimingManifest(timing, 1, 0), "reset without gap");
  }

  private static void expectThrows(Runnable action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException expected) {
      return;
    }
    throw new AssertionError(message + " did not throw");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
