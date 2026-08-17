package com.agoessling.swingcapture.pose;

import java.util.ArrayList;
import java.util.List;

/** Contract tests for the host-inference to shared-Java landmark boundary. */
public final class PoseLandmarkCsvTest {
  private PoseLandmarkCsvTest() {}

  public static void main(String[] arguments) {
    parsesPresentAndMissingLandmarks();
    rejectsMalformedRows();
  }

  private static void parsesPresentAndMissingLandmarks() {
    List<String> fields = rowPrefix("200000", "1.0");
    for (int index = 0; index < PoseJoint.values().length; ++index) {
      fields.add(Double.toString(0.10 + index * 0.01));
      fields.add(Double.toString(0.20 + index * 0.01));
      fields.add("0.8");
    }
    List<String> empty = rowPrefix("400000", "0.0");
    for (int index = 0; index < PoseJoint.values().length; ++index) {
      empty.add("");
      empty.add("");
      empty.add("");
    }
    List<PoseLandmarkFrame> frames =
        PoseLandmarkCsv.parse(
            PoseLandmarkCsv.HEADER
                + "\n"
                + String.join(",", fields)
                + "\n"
                + String.join(",", empty)
                + "\n");

    check(frames.size() == 2, "frame count");
    check(frames.get(0).timestampNs() == 200_000_000L, "timestamp units");
    check(frames.get(0).landmarks().size() == PoseJoint.values().length, "present joints");
    check(
        frames.get(0).landmark(PoseJoint.RIGHT_ANKLE).orElseThrow().visibility() == 0.8,
        "joint ordering");
    check(frames.get(1).landmarks().isEmpty(), "missing pose");
  }

  private static void rejectsMalformedRows() {
    expectThrows(() -> PoseLandmarkCsv.parse("wrong\n"), "header");
    expectThrows(() -> PoseLandmarkCsv.parse(PoseLandmarkCsv.HEADER + "\n"), "empty body");
    expectThrows(
        () -> PoseLandmarkCsv.parse(PoseLandmarkCsv.HEADER + "\n0,1.0\n"), "field count");

    List<String> partial = completeEmptyRow("0", "1.0");
    partial.set(2, "0.5");
    expectThrows(
        () ->
            PoseLandmarkCsv.parse(
                PoseLandmarkCsv.HEADER + "\n" + String.join(",", partial) + "\n"),
        "partial triple");

    List<String> outOfRange = completeEmptyRow("0", "1.0");
    outOfRange.set(2, "1.1");
    outOfRange.set(3, "0.5");
    outOfRange.set(4, "0.5");
    expectThrows(
        () ->
            PoseLandmarkCsv.parse(
                PoseLandmarkCsv.HEADER + "\n" + String.join(",", outOfRange) + "\n"),
        "coordinate range");

    List<String> first = completeEmptyRow("0", "0.0");
    expectThrows(
        () ->
            PoseLandmarkCsv.parse(
                PoseLandmarkCsv.HEADER
                    + "\n"
                    + String.join(",", first)
                    + "\n"
                    + String.join(",", first)
                    + "\n"),
        "timestamp ordering");
  }

  private static List<String> completeEmptyRow(String timestamp, String confidence) {
    List<String> fields = rowPrefix(timestamp, confidence);
    for (int index = 0; index < PoseJoint.values().length; ++index) {
      fields.add("");
      fields.add("");
      fields.add("");
    }
    return fields;
  }

  private static List<String> rowPrefix(String timestamp, String confidence) {
    List<String> fields = new ArrayList<>();
    fields.add(timestamp);
    fields.add(confidence);
    return fields;
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
