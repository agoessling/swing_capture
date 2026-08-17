package com.agoessling.swingcapture.pose;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** Strict interchange format between a pose-inference adapter and deterministic replay. */
public final class PoseObservationCsv {
  public static final String HEADER =
      "timestamp_us,person_confidence,address_confidence,motion_magnitude,inside_hitting_region";
  private static final long MICROS_TO_NANOS = 1_000L;

  private PoseObservationCsv() {}

  public static String serialize(List<PoseTriggerController.Observation> observations) {
    Objects.requireNonNull(observations, "observations");
    if (observations.isEmpty()) {
      throw new IllegalArgumentException("observations cannot be empty");
    }
    StringBuilder csv = new StringBuilder(HEADER).append('\n');
    long previousTimestampNs = -1;
    for (PoseTriggerController.Observation observation : observations) {
      Objects.requireNonNull(observation, "observation");
      if (observation.timestampNs() % MICROS_TO_NANOS != 0) {
        throw new IllegalArgumentException("observation timestamp must have microsecond precision");
      }
      if (previousTimestampNs >= 0 && observation.timestampNs() <= previousTimestampNs) {
        throw new IllegalArgumentException("observation timestamps must increase");
      }
      previousTimestampNs = observation.timestampNs();
      csv.append(observation.timestampNs() / MICROS_TO_NANOS)
          .append(',')
          .append(observation.personConfidence())
          .append(',')
          .append(observation.addressConfidence())
          .append(',')
          .append(observation.motionMagnitude())
          .append(',')
          .append(observation.insideHittingRegion())
          .append('\n');
    }
    return csv.toString();
  }

  public static List<PoseTriggerController.Observation> parse(String csv) {
    Objects.requireNonNull(csv, "csv");
    String[] lines = csv.split("\\R", -1);
    if (lines.length == 0 || !lines[0].equals(HEADER)) {
      throw new IllegalArgumentException("observation CSV has an unexpected header");
    }

    List<PoseTriggerController.Observation> observations = new ArrayList<>();
    long previousTimestampNs = -1;
    for (int lineIndex = 1; lineIndex < lines.length; ++lineIndex) {
      String line = lines[lineIndex];
      if (line.isEmpty() && lineIndex == lines.length - 1) {
        continue;
      }
      if (line.isBlank()) {
        throw new IllegalArgumentException("blank observation CSV row at line " + (lineIndex + 1));
      }
      String[] fields = line.split(",", -1);
      if (fields.length != 5) {
        throw new IllegalArgumentException(
            "observation CSV row must have five fields at line " + (lineIndex + 1));
      }

      long timestampNs =
          Math.multiplyExact(
              parseNonnegativeLong(fields[0], "timestamp_us", lineIndex), MICROS_TO_NANOS);
      if (previousTimestampNs >= 0 && timestampNs <= previousTimestampNs) {
        throw new IllegalArgumentException(
            "observation timestamps must increase at line " + (lineIndex + 1));
      }
      previousTimestampNs = timestampNs;
      observations.add(
          new PoseTriggerController.Observation(
              timestampNs,
              parseDouble(fields[1], "person_confidence", lineIndex),
              parseDouble(fields[2], "address_confidence", lineIndex),
              parseDouble(fields[3], "motion_magnitude", lineIndex),
              parseBoolean(fields[4], lineIndex)));
    }
    if (observations.isEmpty()) {
      throw new IllegalArgumentException("observation CSV must contain at least one row");
    }
    return List.copyOf(observations);
  }

  private static long parseNonnegativeLong(String field, String name, int lineIndex) {
    try {
      long value = Long.parseLong(field);
      if (value < 0) {
        throw new IllegalArgumentException(name + " cannot be negative");
      }
      return value;
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException(
          "invalid " + name + " at line " + (lineIndex + 1), exception);
    }
  }

  private static double parseDouble(String field, String name, int lineIndex) {
    try {
      return Double.parseDouble(field);
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException(
          "invalid " + name + " at line " + (lineIndex + 1), exception);
    }
  }

  private static boolean parseBoolean(String field, int lineIndex) {
    if (field.equals("true")) {
      return true;
    }
    if (field.equals("false")) {
      return false;
    }
    throw new IllegalArgumentException(
        "inside_hitting_region must be true or false at line " + (lineIndex + 1));
  }
}
