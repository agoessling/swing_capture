package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegate;
import java.util.Objects;

/** Thread-safe measurements for the continuously running pose standby path. */
public final class PoseStandbyMetrics {
  public record Snapshot(
      PoseInferenceDelegate delegate,
      long offeredImages,
      long scheduledImages,
      long droppedImages,
      long successfulInferences,
      long failedInferences,
      long totalInferenceDurationNs,
      long maximumInferenceDurationNs,
      long retainedObservationRows,
      long failedObservationRows,
      long offeredEvidenceFrames,
      long droppedEvidenceFrames,
      long encodedEvidenceFrames,
      long failedEvidenceFrames,
      long armEvidenceFlushPresent,
      long armEvidenceFlushFailed,
      long armEvidenceFlushTimedOut) {
    public Snapshot {
      Objects.requireNonNull(delegate, "delegate");
    }

    public long inferenceCount() {
      return successfulInferences + failedInferences;
    }

    public double meanInferenceDurationMs() {
      return inferenceCount() == 0
          ? 0.0
          : totalInferenceDurationNs / 1_000_000.0 / inferenceCount();
    }
  }

  private final PoseInferenceDelegate delegate;
  private long offeredImages;
  private long scheduledImages;
  private long droppedImages;
  private long successfulInferences;
  private long failedInferences;
  private long totalInferenceDurationNs;
  private long maximumInferenceDurationNs;
  private long encodedEvidenceFrames;
  private long failedEvidenceFrames;
  private long offeredEvidenceFrames;
  private long droppedEvidenceFrames;
  private long retainedObservationRows;
  private long failedObservationRows;
  private long armEvidenceFlushPresent;
  private long armEvidenceFlushFailed;
  private long armEvidenceFlushTimedOut;

  PoseStandbyMetrics(PoseInferenceDelegate delegate) {
    this.delegate = Objects.requireNonNull(delegate, "delegate");
  }

  synchronized void recordOffered() {
    offeredImages++;
  }

  synchronized void recordScheduled() {
    scheduledImages++;
  }

  synchronized void recordDropped() {
    droppedImages++;
  }

  synchronized void recordInference(long durationNs, boolean succeeded) {
    if (durationNs < 0) {
      throw new IllegalArgumentException("durationNs cannot be negative");
    }
    if (succeeded) {
      successfulInferences++;
    } else {
      failedInferences++;
    }
    totalInferenceDurationNs = Math.addExact(totalInferenceDurationNs, durationNs);
    maximumInferenceDurationNs = Math.max(maximumInferenceDurationNs, durationNs);
  }

  synchronized void recordEvidence(boolean succeeded) {
    if (succeeded) {
      encodedEvidenceFrames++;
    } else {
      failedEvidenceFrames++;
    }
  }

  synchronized void recordEvidenceOffered() {
    offeredEvidenceFrames++;
  }

  synchronized void recordObservation(boolean succeeded) {
    if (succeeded) {
      retainedObservationRows++;
    } else {
      failedObservationRows++;
    }
  }

  synchronized void recordEvidenceDropped() {
    droppedEvidenceFrames++;
  }

  synchronized void recordArmEvidenceFlush(ArmEvidenceFlush.Outcome outcome) {
    switch (Objects.requireNonNull(outcome, "outcome")) {
      case PRESENT -> armEvidenceFlushPresent++;
      case FAILED -> armEvidenceFlushFailed++;
      case TIMED_OUT -> armEvidenceFlushTimedOut++;
      case NOT_REQUESTED -> {
        // Shadow/external arms and non-debug operation have no local decision-frame JPEG.
      }
    }
  }

  public synchronized Snapshot snapshot() {
    return new Snapshot(
        delegate,
        offeredImages,
        scheduledImages,
        droppedImages,
        successfulInferences,
        failedInferences,
        totalInferenceDurationNs,
        maximumInferenceDurationNs,
        retainedObservationRows,
        failedObservationRows,
        offeredEvidenceFrames,
        droppedEvidenceFrames,
        encodedEvidenceFrames,
        failedEvidenceFrames,
        armEvidenceFlushPresent,
        armEvidenceFlushFailed,
        armEvidenceFlushTimedOut);
  }
}
