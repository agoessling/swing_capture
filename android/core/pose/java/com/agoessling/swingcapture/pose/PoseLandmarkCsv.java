package com.agoessling.swingcapture.pose;

import java.util.ArrayList;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Strict fixed-width interchange between MediaPipe host inference and shared feature extraction. */
public final class PoseLandmarkCsv {
  private static final long MICROS_TO_NANOS = 1_000L;
  private static final int PREFIX_FIELDS = 2;
  private static final int FIELDS_PER_JOINT = 3;
  public static final String HEADER = buildHeader();

  private PoseLandmarkCsv() {}

  public static List<PoseLandmarkFrame> parse(String csv) {
    Objects.requireNonNull(csv, "csv");
    String[] lines = csv.split("\\R", -1);
    if (lines.length == 0 || !lines[0].equals(HEADER)) {
      throw new IllegalArgumentException("landmark CSV has an unexpected header");
    }
    List<PoseLandmarkFrame> frames = new ArrayList<>();
    long previousTimestampNs = -1;
    for (int lineIndex = 1; lineIndex < lines.length; ++lineIndex) {
      String line = lines[lineIndex];
      if (line.isEmpty() && lineIndex == lines.length - 1) {
        continue;
      }
      if (line.isBlank()) {
        throw new IllegalArgumentException("blank landmark CSV row at line " + (lineIndex + 1));
      }
      String[] fields = line.split(",", -1);
      int expectedFields = PREFIX_FIELDS + FIELDS_PER_JOINT * PoseJoint.values().length;
      if (fields.length != expectedFields) {
        throw new IllegalArgumentException(
            "landmark CSV row has "
                + fields.length
                + " fields instead of "
                + expectedFields
                + " at line "
                + (lineIndex + 1));
      }
      long timestampNs =
          Math.multiplyExact(
              parseNonnegativeLong(fields[0], "timestamp_us", lineIndex), MICROS_TO_NANOS);
      if (previousTimestampNs >= 0 && timestampNs <= previousTimestampNs) {
        throw new IllegalArgumentException(
            "landmark timestamps must increase at line " + (lineIndex + 1));
      }
      previousTimestampNs = timestampNs;
      double personConfidence = parseDouble(fields[1], "detector_person_confidence", lineIndex);
      Map<PoseJoint, NormalizedPoseLandmark> landmarks = new EnumMap<>(PoseJoint.class);
      PoseJoint[] joints = PoseJoint.values();
      for (int jointIndex = 0; jointIndex < joints.length; ++jointIndex) {
        int offset = PREFIX_FIELDS + FIELDS_PER_JOINT * jointIndex;
        boolean xEmpty = fields[offset].isEmpty();
        boolean yEmpty = fields[offset + 1].isEmpty();
        boolean visibilityEmpty = fields[offset + 2].isEmpty();
        if (xEmpty && yEmpty && visibilityEmpty) {
          continue;
        }
        if (xEmpty || yEmpty || visibilityEmpty) {
          throw new IllegalArgumentException(
              "landmark triples must be wholly present or absent at line " + (lineIndex + 1));
        }
        landmarks.put(
            joints[jointIndex],
            new NormalizedPoseLandmark(
                parseDouble(fields[offset], joints[jointIndex] + " x", lineIndex),
                parseDouble(fields[offset + 1], joints[jointIndex] + " y", lineIndex),
                parseDouble(
                    fields[offset + 2], joints[jointIndex] + " visibility", lineIndex)));
      }
      frames.add(new PoseLandmarkFrame(timestampNs, personConfidence, landmarks));
    }
    if (frames.isEmpty()) {
      throw new IllegalArgumentException("landmark CSV must contain at least one row");
    }
    return List.copyOf(frames);
  }

  private static String buildHeader() {
    StringBuilder header = new StringBuilder("timestamp_us,detector_person_confidence");
    for (PoseJoint joint : PoseJoint.values()) {
      String name = joint.name().toLowerCase(java.util.Locale.ROOT);
      header.append(',').append(name).append("_x");
      header.append(',').append(name).append("_y");
      header.append(',').append(name).append("_visibility");
    }
    return header.toString();
  }

  private static long parseNonnegativeLong(String value, String name, int lineIndex) {
    try {
      long parsed = Long.parseLong(value);
      if (parsed < 0) {
        throw new IllegalArgumentException(name + " cannot be negative");
      }
      return parsed;
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException(
          "invalid " + name + " at line " + (lineIndex + 1), exception);
    }
  }

  private static double parseDouble(String value, String name, int lineIndex) {
    try {
      return Double.parseDouble(value);
    } catch (NumberFormatException exception) {
      throw new IllegalArgumentException(
          "invalid " + name + " at line " + (lineIndex + 1), exception);
    }
  }
}
