package com.agoessling.swingcapture.pose.inference;

/** Thread-safe counters for cadence, backpressure, failures, and inference cost. */
public final class PoseInferenceMetrics {
  public record Snapshot(
      long offered,
      long cadenceRejected,
      long scheduled,
      long droppedBackpressure,
      long succeeded,
      long failed,
      long totalInferenceNs,
      long maxInferenceNs) {
    public double meanInferenceMs() {
      return succeeded + failed == 0
          ? 0.0
          : totalInferenceNs / 1_000_000.0 / (succeeded + failed);
    }

    public String toJson() {
      return "{\"offered\":"
          + offered
          + ",\"cadence_rejected\":"
          + cadenceRejected
          + ",\"scheduled\":"
          + scheduled
          + ",\"dropped_backpressure\":"
          + droppedBackpressure
          + ",\"succeeded\":"
          + succeeded
          + ",\"failed\":"
          + failed
          + ",\"total_inference_ns\":"
          + totalInferenceNs
          + ",\"max_inference_ns\":"
          + maxInferenceNs
          + "}";
    }
  }

  private long offered;
  private long cadenceRejected;
  private long scheduled;
  private long droppedBackpressure;
  private long succeeded;
  private long failed;
  private long totalInferenceNs;
  private long maxInferenceNs;

  synchronized void recordOffered() {
    offered++;
  }

  synchronized void recordCadenceRejected() {
    cadenceRejected++;
  }

  synchronized void recordScheduled() {
    scheduled++;
  }

  synchronized void recordDroppedBackpressure() {
    droppedBackpressure++;
  }

  synchronized void recordCompleted(long durationNs, boolean success) {
    if (durationNs < 0) {
      throw new IllegalArgumentException("durationNs cannot be negative");
    }
    if (success) {
      succeeded++;
    } else {
      failed++;
    }
    totalInferenceNs += durationNs;
    maxInferenceNs = Math.max(maxInferenceNs, durationNs);
  }

  public synchronized Snapshot snapshot() {
    return new Snapshot(
        offered,
        cadenceRejected,
        scheduled,
        droppedBackpressure,
        succeeded,
        failed,
        totalInferenceNs,
        maxInferenceNs);
  }
}
