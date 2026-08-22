package com.agoessling.swingcapture.pose;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** End-to-end host replay CLI contract test. */
public final class PoseTriggerReplayCliTest {
  private PoseTriggerReplayCliTest() {}

  public static void main(String[] arguments) throws IOException {
    emitsCanonicalPassingResult();
    rejectsUnknownAndIncompleteOptions();
  }

  private static void emitsCanonicalPassingResult() throws IOException {
    Path observations =
        Path.of(System.getenv("TEST_TMPDIR"), "pose-trigger-observations.csv");
    Files.writeString(
        observations,
        PoseObservationCsv.HEADER
            + "\n0,0.9,0.2,0.05,true"
            + "\n200000,0.9,0.2,0.05,true"
            + "\n400000,0.9,0.2,0.05,true"
            + "\n600000,0.9,0.2,0.05,true"
            + "\n800000,0.9,0.2,0.05,true"
            + "\n1000000,0.9,0.8,0.05,true"
            + "\n1200000,0.9,0.8,0.05,true"
            + "\n1400000,0.9,0.8,0.05,true\n",
        StandardCharsets.UTF_8);

    String json =
        PoseTriggerReplayCli.run(
            new String[] {
              "--observations=" + observations,
              "--safe-arm-start-ms=800",
              "--preferred-arm-ms=1200",
              "--takeaway-ms=2000",
              "--startup-budget-ms=300",
              "--must-not-arm-ms=0:800"
            });

    check(
        json.equals(
            "{\"schema_version\":1,\"observation_count\":8,"
                + "\"arm_request_ns\":\"1400000000\","
                + "\"arm_offset_from_preferred_ns\":\"200000000\","
                + "\"high_speed_ready_ns\":\"1700000000\","
                + "\"ready_lead_before_takeaway_ns\":\"300000000\","
                + "\"armed_before_safe_window\":false,"
                + "\"armed_in_forbidden_interval\":false,"
                + "\"ready_by_takeaway\":true,\"passed\":true,"
                + "\"outcome\":\"acceptable\",\"arm_request_count\":1,"
                + "\"final_state\":\"arm_requested\"}"),
        "canonical result JSON");
  }

  private static void rejectsUnknownAndIncompleteOptions() {
    expectThrows(
        () -> PoseTriggerReplayCli.run(new String[] {"--unknown=value"}), "unknown option");
    expectThrows(() -> PoseTriggerReplayCli.run(new String[] {}), "missing options");
    expectThrows(
        () ->
            PoseTriggerReplayCli.run(
                new String[] {"--observations=x", "--observations=y"}),
        "duplicate options");
  }

  @FunctionalInterface
  private interface ThrowingRunnable {
    void run() throws Exception;
  }

  private static void expectThrows(ThrowingRunnable action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException | IOException expected) {
      return;
    } catch (Exception exception) {
      throw new AssertionError(message + " threw unexpected exception", exception);
    }
    throw new AssertionError(message + " did not throw");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
