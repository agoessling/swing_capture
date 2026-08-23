package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegate;

public final class PoseStandbyMetricsTest {
  public static void main(String[] args) {
    recordsCountersDeadlinesAndDecisionAge();
    reportsNearestRankPercentilesWithBoundedStorage();
  }

  private static void recordsCountersDeadlinesAndDecisionAge() {
    PoseStandbyMetrics metrics = new PoseStandbyMetrics(PoseInferenceDelegate.GPU);
    metrics.recordOffered();
    metrics.recordOffered();
    metrics.recordScheduled();
    metrics.recordDropped();
    metrics.recordWarmup(714_000_000, true);
    metrics.recordInference(200_000_000, true);
    metrics.recordInference(201_000_000, false);
    metrics.recordInference(401_000_000, true);
    metrics.recordDecision(1_000_000_000, 1_150_000_000);
    metrics.recordDecision(2_000_000_000L, 2_350_000_000L);
    metrics.recordDecision(4_000_000_000L, 3_999_999_999L);
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
    check(snapshot.warmupInferenceCount() == 1, "warm-up count mismatch");
    check(snapshot.successfulWarmupInferences() == 1, "warm-up success mismatch");
    check(snapshot.failedWarmupInferences() == 0, "warm-up failure mismatch");
    check(snapshot.totalWarmupDurationNs() == 714_000_000, "warm-up duration mismatch");
    check(snapshot.maximumWarmupDurationNs() == 714_000_000, "warm-up max mismatch");
    check(snapshot.inferenceCount() == 3, "inference count mismatch");
    check(snapshot.successfulInferences() == 2, "success count mismatch");
    check(snapshot.failedInferences() == 1, "failure count mismatch");
    check(snapshot.totalInferenceDurationNs() == 802_000_000, "duration sum mismatch");
    check(snapshot.maximumInferenceDurationNs() == 401_000_000, "duration max mismatch");
    check(snapshot.inferenceDurationP50Ns() == 201_000_000, "duration p50 mismatch");
    check(snapshot.inferenceDurationP90Ns() == 401_000_000, "duration p90 mismatch");
    check(snapshot.inferenceDurationP95Ns() == 401_000_000, "duration p95 mismatch");
    check(snapshot.inferenceDurationP99Ns() == 401_000_000, "duration p99 mismatch");
    check(snapshot.inferenceDeadlineMisses() == 2, "deadline misses mismatch");
    check(snapshot.inferenceOutliers() == 1, "outliers mismatch");
    check(snapshot.decisionAgeSamples() == 2, "decision age sample count mismatch");
    check(snapshot.rejectedDecisionTimestamps() == 1, "timestamp rejection mismatch");
    check(snapshot.totalDecisionAgeNs() == 500_000_000, "decision age sum mismatch");
    check(snapshot.maximumDecisionAgeNs() == 350_000_000, "decision age max mismatch");
    check(snapshot.decisionAgeP50Ns() == 150_000_000, "decision age p50 mismatch");
    check(snapshot.decisionAgeP90Ns() == 350_000_000, "decision age p90 mismatch");
    check(snapshot.decisionAgeP95Ns() == 350_000_000, "decision age p95 mismatch");
    check(snapshot.decisionAgeP99Ns() == 350_000_000, "decision age p99 mismatch");
    check(snapshot.meanDecisionAgeMs() == 250.0, "decision age mean mismatch");
    check(snapshot.retainedObservationRows() == 1, "retained observations mismatch");
    check(snapshot.failedObservationRows() == 1, "failed observations mismatch");
    check(snapshot.offeredEvidenceFrames() == 2, "offered evidence mismatch");
    check(snapshot.droppedEvidenceFrames() == 1, "dropped evidence mismatch");
    check(snapshot.encodedEvidenceFrames() == 1, "encoded evidence mismatch");
    check(snapshot.failedEvidenceFrames() == 1, "failed evidence mismatch");
    check(snapshot.armEvidenceFlushPresent() == 1, "arm evidence present mismatch");
    check(snapshot.armEvidenceFlushFailed() == 1, "arm evidence failure mismatch");
    check(snapshot.armEvidenceFlushTimedOut() == 1, "arm evidence timeout mismatch");

    boolean rejectedSecondWarmup = false;
    try {
      metrics.recordWarmup(1, true);
    } catch (IllegalStateException expected) {
      rejectedSecondWarmup = true;
    }
    check(rejectedSecondWarmup, "second warm-up sample must be rejected");

    PoseStandbyMetrics failedWarmup = new PoseStandbyMetrics(PoseInferenceDelegate.CPU);
    failedWarmup.recordWarmup(333_000_000, false);
    check(failedWarmup.snapshot().failedWarmupInferences() == 1, "failed warm-up mismatch");
    check(failedWarmup.snapshot().inferenceCount() == 0, "warm-up must not pollute steady metrics");

    PoseStandbyMetrics overflow = new PoseStandbyMetrics(PoseInferenceDelegate.CPU);
    overflow.recordInference(10_000_000_000L, true);
    overflow.recordInference(10_500_000_000L, true);
    check(
        overflow.snapshot().inferenceDurationP50Ns() == 10_000_000_000L,
        "last regular histogram bucket must remain distinct from overflow");
    check(
        overflow.snapshot().inferenceDurationP90Ns() == 10_500_000_000L,
        "histogram overflow must retain a conservative upper bound");
  }

  private static void reportsNearestRankPercentilesWithBoundedStorage() {
    PoseStandbyMetrics empty = new PoseStandbyMetrics(PoseInferenceDelegate.GPU);
    check(empty.snapshot().inferenceDurationP99Ns() == 0, "empty inference percentile");
    check(empty.snapshot().decisionAgeP99Ns() == 0, "empty decision-age percentile");

    PoseStandbyMetrics distribution = new PoseStandbyMetrics(PoseInferenceDelegate.CPU);
    for (int milliseconds = 1; milliseconds <= 100; ++milliseconds) {
      distribution.recordInference(milliseconds * 1_000_000L, true);
      distribution.recordDecision(0, milliseconds * 1_000_000L);
    }
    PoseStandbyMetrics.Snapshot snapshot = distribution.snapshot();
    check(snapshot.inferenceDurationP50Ns() == 50_000_000, "distribution p50");
    check(snapshot.inferenceDurationP90Ns() == 90_000_000, "distribution p90");
    check(snapshot.inferenceDurationP95Ns() == 95_000_000, "distribution p95");
    check(snapshot.inferenceDurationP99Ns() == 99_000_000, "distribution p99");
    check(snapshot.decisionAgeP95Ns() == 95_000_000, "decision-age distribution p95");

    PoseStandbyMetrics rounded = new PoseStandbyMetrics(PoseInferenceDelegate.CPU);
    rounded.recordInference(1_000_001, true);
    check(
        rounded.snapshot().inferenceDurationP50Ns() == 2_000_000,
        "sub-millisecond remainder must round to an inclusive bucket upper bound");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
