package com.agoessling.swingcapture.pose.inference;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/** Bounded, deterministic NDJSON trace suitable for inclusion in a diagnostic archive. */
public final class PoseInferenceTrace {
  public record Event(
      long frameTimestampNs,
      long startedNs,
      long finishedNs,
      PoseInferenceDelegate delegate,
      String outcome,
      int poseCount) {}

  private final int capacity;
  private final ArrayDeque<Event> events = new ArrayDeque<>();

  public PoseInferenceTrace(int capacity) {
    if (capacity <= 0) {
      throw new IllegalArgumentException("capacity must be positive");
    }
    this.capacity = capacity;
  }

  public synchronized void add(Event event) {
    if (events.size() == capacity) {
      events.removeFirst();
    }
    events.addLast(event);
  }

  public synchronized List<Event> snapshot() {
    return List.copyOf(new ArrayList<>(events));
  }

  public synchronized String toNdjson() {
    StringBuilder result = new StringBuilder();
    for (Event event : events) {
      result
          .append("{\"frame_timestamp_ns\":")
          .append(event.frameTimestampNs())
          .append(",\"started_ns\":")
          .append(event.startedNs())
          .append(",\"finished_ns\":")
          .append(event.finishedNs())
          .append(",\"delegate\":\"")
          .append(event.delegate().name().toLowerCase(java.util.Locale.ROOT))
          .append("\",\"outcome\":\"")
          .append(escape(event.outcome()))
          .append("\",\"pose_count\":")
          .append(event.poseCount())
          .append("}\n");
    }
    return result.toString();
  }

  private static String escape(String value) {
    return value.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n");
  }
}
