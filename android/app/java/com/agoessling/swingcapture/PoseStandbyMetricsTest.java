package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegate;

public final class PoseStandbyMetricsTest {
  public static void main(String[] args) {
    PoseStandbyMetrics metrics = new PoseStandbyMetrics(PoseInferenceDelegate.GPU);
    metrics.recordOffered();
    metrics.recordOffered();
    metrics.recordScheduled();
    metrics.recordDropped();
    metrics.recordInference(2_000_000, true);
    metrics.recordInference(4_000_000, false);
    metrics.recordEvidence(true);
    metrics.recordEvidence(false);
    metrics.recordEvidenceOffered();
    metrics.recordEvidenceOffered();
    metrics.recordEvidenceDropped();
    metrics.recordObservation(true);
    metrics.recordObservation(false);
    metrics.recordArmEvidenceFlush(ArmEvidenceFlush.Outcome.PRESENT);
    metrics.recordArmEvidenceFlush(ArmEvidenceFlush.Outcome.FAILED);
    metrics.recordArmEvidenceFlush(ArmEvidenceFlush.Outcome.TIMED_OUT);

    PoseStandbyMetrics.Snapshot snapshot = metrics.snapshot();
    check(snapshot.delegate() == PoseInferenceDelegate.GPU, "delegate mismatch");
    check(snapshot.offeredImages() == 2, "offered count mismatch");
    check(snapshot.scheduledImages() == 1, "scheduled count mismatch");
    check(snapshot.droppedImages() == 1, "drop count mismatch");
    check(snapshot.inferenceCount() == 2, "inference count mismatch");
    check(snapshot.successfulInferences() == 1, "success count mismatch");
    check(snapshot.failedInferences() == 1, "failure count mismatch");
    check(snapshot.totalInferenceDurationNs() == 6_000_000, "duration sum mismatch");
    check(snapshot.maximumInferenceDurationNs() == 4_000_000, "duration max mismatch");
    check(snapshot.meanInferenceDurationMs() == 3.0, "duration mean mismatch");
    check(snapshot.retainedObservationRows() == 1, "retained observations mismatch");
    check(snapshot.failedObservationRows() == 1, "failed observations mismatch");
    check(snapshot.offeredEvidenceFrames() == 2, "offered evidence mismatch");
    check(snapshot.droppedEvidenceFrames() == 1, "dropped evidence mismatch");
    check(snapshot.encodedEvidenceFrames() == 1, "encoded evidence mismatch");
    check(snapshot.failedEvidenceFrames() == 1, "failed evidence mismatch");
    check(snapshot.armEvidenceFlushPresent() == 1, "arm evidence present mismatch");
    check(snapshot.armEvidenceFlushFailed() == 1, "arm evidence failure mismatch");
    check(snapshot.armEvidenceFlushTimedOut() == 1, "arm evidence timeout mismatch");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
