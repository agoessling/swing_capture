package com.agoessling.swingcapture.trigger;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Loader for compact, reviewable synthetic combined-trigger event fixtures. */
public final class CombinedTriggerFixture {
  private static final long MS = 1_000_000L;

  public record Fixture(String name, CombinedTriggerLifecycleSimulator.Scenario scenario) {
    public Fixture {
      if (name == null || name.isBlank() || scenario == null) {
        throw new IllegalArgumentException("fixture requires a name and scenario");
      }
    }
  }

  private CombinedTriggerFixture() {}

  public static Fixture load(Path path) throws IOException {
    ArrayList<CombinedTriggerLifecycleSimulator.Event> events = new ArrayList<>();
    HashMap<Integer, CombinedTriggerLifecycleSimulator.StartupPlan> startupOverrides =
        new HashMap<>();
    String name = "";
    long recordingEndNs = -1;

    List<String> lines = Files.readAllLines(path, StandardCharsets.UTF_8);
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
      String raw = lines.get(lineIndex).trim();
      if (raw.isEmpty() || raw.startsWith("#")) {
        continue;
      }
      String[] fields = raw.split(",", -1);
      for (int fieldIndex = 0; fieldIndex < fields.length; ++fieldIndex) {
        fields[fieldIndex] = fields[fieldIndex].trim();
      }
      try {
        switch (fields[0]) {
          case "name" -> {
            requireFieldCount(fields, 2);
            name = fields[1];
          }
          case "pose" -> addPoseRange(fields, events);
          case "audio" -> {
            requireFieldCount(fields, 3);
            events.add(
                new CombinedTriggerLifecycleSimulator.AudioCandidateEvent(
                    milliseconds(fields[1]), fields[2]));
          }
          case "target" -> {
            requireFieldCount(fields, 4);
            events.add(
                new CombinedTriggerLifecycleSimulator.TargetEvent(
                    fields[1], milliseconds(fields[2]), milliseconds(fields[3])));
          }
          case "startup" -> {
            int attemptIndex = Integer.parseInt(fields[1]);
            CombinedTriggerLifecycleSimulator.StartupPlan plan;
            if (fields.length == 4) {
              plan =
                  new CombinedTriggerLifecycleSimulator.StartupPlan(
                      delay(fields[2]), delay(fields[3]));
            } else if (fields.length == 6) {
              plan =
                  new CombinedTriggerLifecycleSimulator.StartupPlan(
                      delay(fields[2]),
                      delay(fields[3]),
                      delay(fields[4]),
                      CombinedTriggerLifecycleSimulator.PeerArmResolution.valueOf(
                          fields[5].toUpperCase(java.util.Locale.ROOT)));
            } else {
              throw new IllegalArgumentException(
                  "startup requires 4 shorthand fields or 6 explicit fields");
            }
            CombinedTriggerLifecycleSimulator.StartupPlan previous =
                startupOverrides.put(attemptIndex, plan);
            if (previous != null) {
              throw new IllegalArgumentException("duplicate startup override");
            }
          }
          case "end" -> {
            requireFieldCount(fields, 2);
            recordingEndNs = milliseconds(fields[1]);
          }
          default -> throw new IllegalArgumentException("unknown event type: " + fields[0]);
        }
      } catch (IllegalArgumentException failure) {
        throw new IllegalArgumentException(
            path + ":" + (lineIndex + 1) + ": " + failure.getMessage(), failure);
      }
    }
    if (name.isBlank()) {
      throw new IllegalArgumentException(path + ": fixture name is missing");
    }
    if (recordingEndNs < 0) {
      throw new IllegalArgumentException(path + ": recording end is missing");
    }
    return new Fixture(
        name,
        new CombinedTriggerLifecycleSimulator.Scenario(
            events, Map.copyOf(startupOverrides), recordingEndNs));
  }

  private static void addPoseRange(
      String[] fields, ArrayList<CombinedTriggerLifecycleSimulator.Event> events) {
    requireFieldCount(fields, 7);
    long startMs = Long.parseLong(fields[1]);
    long endMs = Long.parseLong(fields[2]);
    long periodMs = Long.parseLong(fields[3]);
    if (startMs < 0 || endMs < startMs || periodMs <= 0) {
      throw new IllegalArgumentException("pose range timing is invalid");
    }
    double person = Double.parseDouble(fields[4]);
    double address = Double.parseDouble(fields[5]);
    double motion = Double.parseDouble(fields[6]);
    for (long timestampMs = startMs; timestampMs <= endMs; timestampMs += periodMs) {
      events.add(
          new CombinedTriggerLifecycleSimulator.PoseEvent(
              Math.multiplyExact(timestampMs, MS), person, address, motion));
      if (timestampMs > Long.MAX_VALUE - periodMs) {
        break;
      }
    }
  }

  private static long milliseconds(String value) {
    return Math.multiplyExact(Long.parseLong(value), MS);
  }

  private static long delay(String value) {
    return value.equals("never") ? -1 : milliseconds(value);
  }

  private static void requireFieldCount(String[] fields, int expected) {
    if (fields.length != expected) {
      throw new IllegalArgumentException(
          "expected " + expected + " fields but found " + fields.length);
    }
  }
}
