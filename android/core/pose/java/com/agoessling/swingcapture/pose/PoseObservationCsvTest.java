package com.agoessling.swingcapture.pose;

import java.util.List;

/** Strict parser tests for the inference-to-replay observation boundary. */
public final class PoseObservationCsvTest {
  private PoseObservationCsvTest() {}

  public static void main(String[] arguments) {
    parsesOrderedObservations();
    serializesRoundTrip();
    rejectsMalformedInputs();
  }

  private static void parsesOrderedObservations() {
    List<PoseTriggerController.Observation> observations =
        PoseObservationCsv.parse(
            PoseObservationCsv.HEADER
                + "\n0,0.9,0.2,0.1,true\n"
                + "200000,0.8,0.7,0.05,false\n");

    check(observations.size() == 2, "observation count");
    check(observations.get(1).timestampNs() == 200_000_000L, "microsecond timestamp");
    check(observations.get(1).personConfidence() == 0.8, "person confidence");
    check(!observations.get(1).insideHittingRegion(), "inside region flag");
  }

  private static void serializesRoundTrip() {
    List<PoseTriggerController.Observation> expected =
        List.of(
            new PoseTriggerController.Observation(0, 0.9, 0.2, 0.1, true),
            new PoseTriggerController.Observation(200_000_000L, 0.8, 0.7, 0.05, false));
    check(PoseObservationCsv.parse(PoseObservationCsv.serialize(expected)).equals(expected), "round trip");
    expectThrows(() -> PoseObservationCsv.serialize(List.of()), "empty serialization");
    expectThrows(
        () ->
            PoseObservationCsv.serialize(
                List.of(new PoseTriggerController.Observation(1, 0.8, 0.7, 0.05, false))),
        "sub-microsecond timestamp");
  }

  private static void rejectsMalformedInputs() {
    expectThrows(
        () -> PoseObservationCsv.parse("wrong\n0,0.9,0.9,0.0,true\n"), "wrong header");
    expectThrows(() -> PoseObservationCsv.parse(PoseObservationCsv.HEADER + "\n"), "empty body");
    expectThrows(
        () -> PoseObservationCsv.parse(PoseObservationCsv.HEADER + "\n0,0.9,0.9,0.0\n"),
        "field count");
    expectThrows(
        () ->
            PoseObservationCsv.parse(
                PoseObservationCsv.HEADER + "\n0,0.9,0.9,0.0,true\n0,0.9,0.9,0.0,true\n"),
        "duplicate timestamp");
    expectThrows(
        () -> PoseObservationCsv.parse(PoseObservationCsv.HEADER + "\n0,0.9,0.9,0.0,TRUE\n"),
        "boolean syntax");
    expectThrows(
        () -> PoseObservationCsv.parse(PoseObservationCsv.HEADER + "\n0,NaN,0.9,0.0,true\n"),
        "nonfinite confidence");
    expectThrows(
        () ->
            PoseObservationCsv.parse(
                PoseObservationCsv.HEADER + "\n9223372036854775807,0.9,0.9,0.0,true\n"),
        "timestamp overflow");
  }

  private static void expectThrows(Runnable action, String message) {
    try {
      action.run();
    } catch (IllegalArgumentException | ArithmeticException expected) {
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
