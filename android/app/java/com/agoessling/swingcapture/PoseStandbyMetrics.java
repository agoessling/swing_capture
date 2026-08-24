package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegate;
import java.util.Arrays;
import java.util.Objects;

/** Thread-safe measurements for the continuously running pose standby path. */
public final class PoseStandbyMetrics {
  private static final long LATENCY_BUCKET_WIDTH_NS = 1_000_000L;
  private static final int LATENCY_REGULAR_BUCKET_COUNT = 10_000;
  private static final int LATENCY_BUCKET_COUNT = LATENCY_REGULAR_BUCKET_COUNT + 1;
  static final int RECENT_INFERENCE_WINDOW_CAPACITY = 150;
  static final long INFERENCE_DEADLINE_NS = 200_000_000L;
  static final long INFERENCE_OUTLIER_BOUND_NS = 400_000_000L;

  public record Snapshot(
      PoseInferenceDelegate delegate,
      long offeredImages,
      long scheduledImages,
      long droppedImages,
      long successfulWarmupInferences,
      long failedWarmupInferences,
      long totalWarmupDurationNs,
      long maximumWarmupDurationNs,
      long successfulInferences,
      long failedInferences,
      long totalInferenceDurationNs,
      long maximumInferenceDurationNs,
      long inferenceDurationP50Ns,
      long inferenceDurationP90Ns,
      long inferenceDurationP95Ns,
      long inferenceDurationP99Ns,
      long inferenceDeadlineMisses,
      long inferenceOutliers,
      long recentInferenceSampleCount,
      long recentInferenceDurationP95Ns,
      long recentMaximumInferenceDurationNs,
      long recentInferenceDeadlineMisses,
      long decisionAgeSamples,
      long rejectedDecisionTimestamps,
      long totalDecisionAgeNs,
      long maximumDecisionAgeNs,
      long decisionAgeP50Ns,
      long decisionAgeP90Ns,
      long decisionAgeP95Ns,
      long decisionAgeP99Ns,
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

    public long warmupInferenceCount() {
      return successfulWarmupInferences + failedWarmupInferences;
    }

    public double meanInferenceDurationMs() {
      return inferenceCount() == 0
          ? 0.0
          : totalInferenceDurationNs / 1_000_000.0 / inferenceCount();
    }

    public double meanDecisionAgeMs() {
      return decisionAgeSamples == 0
          ? 0.0
          : totalDecisionAgeNs / 1_000_000.0 / decisionAgeSamples;
    }
  }

  private final PoseInferenceDelegate delegate;
  private long offeredImages;
  private long scheduledImages;
  private long droppedImages;
  private long successfulWarmupInferences;
  private long failedWarmupInferences;
  private long totalWarmupDurationNs;
  private long maximumWarmupDurationNs;
  private long successfulInferences;
  private long failedInferences;
  private long totalInferenceDurationNs;
  private long maximumInferenceDurationNs;
  private final LatencyHistogram inferenceDuration = new LatencyHistogram();
  private final RecentInferenceWindow recentInferenceDuration = new RecentInferenceWindow();
  private long inferenceDeadlineMisses;
  private long inferenceOutliers;
  private long decisionAgeSamples;
  private long rejectedDecisionTimestamps;
  private long totalDecisionAgeNs;
  private long maximumDecisionAgeNs;
  private final LatencyHistogram decisionAge = new LatencyHistogram();
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

  synchronized void recordWarmup(long durationNs, boolean succeeded) {
    if (durationNs < 0) {
      throw new IllegalArgumentException("durationNs cannot be negative");
    }
    if (successfulWarmupInferences + failedWarmupInferences != 0) {
      throw new IllegalStateException("pose warm-up may only be recorded once");
    }
    if (succeeded) {
      successfulWarmupInferences++;
    } else {
      failedWarmupInferences++;
    }
    totalWarmupDurationNs = durationNs;
    maximumWarmupDurationNs = durationNs;
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
    inferenceDuration.record(durationNs);
    recentInferenceDuration.record(durationNs);
    if (durationNs > INFERENCE_DEADLINE_NS) {
      inferenceDeadlineMisses++;
    }
    if (durationNs > INFERENCE_OUTLIER_BOUND_NS) {
      inferenceOutliers++;
    }
  }

  synchronized void recordDecision(long frameTimestampNs, long decisionTimestampNs) {
    if (frameTimestampNs < 0 || decisionTimestampNs < 0) {
      throw new IllegalArgumentException("pose decision timestamps cannot be negative");
    }
    if (decisionTimestampNs < frameTimestampNs) {
      rejectedDecisionTimestamps++;
      return;
    }
    long ageNs = decisionTimestampNs - frameTimestampNs;
    decisionAgeSamples++;
    totalDecisionAgeNs = Math.addExact(totalDecisionAgeNs, ageNs);
    maximumDecisionAgeNs = Math.max(maximumDecisionAgeNs, ageNs);
    decisionAge.record(ageNs);
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
    RecentInferenceWindow.Snapshot recent = recentInferenceDuration.snapshot();
    return new Snapshot(
        delegate,
        offeredImages,
        scheduledImages,
        droppedImages,
        successfulWarmupInferences,
        failedWarmupInferences,
        totalWarmupDurationNs,
        maximumWarmupDurationNs,
        successfulInferences,
        failedInferences,
        totalInferenceDurationNs,
        maximumInferenceDurationNs,
        inferenceDuration.percentileUpperBoundNs(50),
        inferenceDuration.percentileUpperBoundNs(90),
        inferenceDuration.percentileUpperBoundNs(95),
        inferenceDuration.percentileUpperBoundNs(99),
        inferenceDeadlineMisses,
        inferenceOutliers,
        recent.sampleCount(),
        recent.p95Ns(),
        recent.maximumNs(),
        recent.deadlineMisses(),
        decisionAgeSamples,
        rejectedDecisionTimestamps,
        totalDecisionAgeNs,
        maximumDecisionAgeNs,
        decisionAge.percentileUpperBoundNs(50),
        decisionAge.percentileUpperBoundNs(90),
        decisionAge.percentileUpperBoundNs(95),
        decisionAge.percentileUpperBoundNs(99),
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

  /** Latest 30 seconds at the nominal five-Hz cadence, independent of lifetime dilution. */
  private static final class RecentInferenceWindow {
    record Snapshot(long sampleCount, long p95Ns, long maximumNs, long deadlineMisses) {}

    private final long[] durationsNs = new long[RECENT_INFERENCE_WINDOW_CAPACITY];
    private int nextIndex;
    private int sampleCount;
    private long deadlineMisses;

    void record(long durationNs) {
      if (sampleCount == durationsNs.length) {
        if (durationsNs[nextIndex] > INFERENCE_DEADLINE_NS) {
          deadlineMisses--;
        }
      } else {
        sampleCount++;
      }
      durationsNs[nextIndex] = durationNs;
      nextIndex = (nextIndex + 1) % durationsNs.length;
      if (durationNs > INFERENCE_DEADLINE_NS) {
        deadlineMisses++;
      }
    }

    Snapshot snapshot() {
      if (sampleCount == 0) {
        return new Snapshot(0, 0, 0, 0);
      }
      long[] sorted = Arrays.copyOf(durationsNs, sampleCount);
      Arrays.sort(sorted);
      long maximumNs = sorted[sampleCount - 1];
      int p95Index = (sampleCount * 95 + 99) / 100 - 1;
      long p95Ns = sorted[p95Index];
      long regularMaximumNs = LATENCY_REGULAR_BUCKET_COUNT * LATENCY_BUCKET_WIDTH_NS;
      long p95UpperBoundNs =
          p95Ns > regularMaximumNs
              ? maximumNs
              : p95Ns == 0
                  ? LATENCY_BUCKET_WIDTH_NS
                  : ((p95Ns - 1) / LATENCY_BUCKET_WIDTH_NS + 1) * LATENCY_BUCKET_WIDTH_NS;
      return new Snapshot(sampleCount, p95UpperBoundNs, maximumNs, deadlineMisses);
    }
  }

  /** Fixed-memory, one-millisecond histogram; percentile values are inclusive upper bounds. */
  private static final class LatencyHistogram {
    private final long[] buckets = new long[LATENCY_BUCKET_COUNT];
    private long samples;
    private long overflowMaximumNs;

    void record(long durationNs) {
      int bucket =
          durationNs == 0
              ? 0
              : (int)
                  Math.min(
                      (durationNs - 1) / LATENCY_BUCKET_WIDTH_NS,
                      LATENCY_REGULAR_BUCKET_COUNT);
      buckets[bucket]++;
      samples++;
      if (bucket == LATENCY_REGULAR_BUCKET_COUNT) {
        overflowMaximumNs = Math.max(overflowMaximumNs, durationNs);
      }
    }

    long percentileUpperBoundNs(int percentile) {
      if (percentile < 1 || percentile > 100) {
        throw new IllegalArgumentException("percentile must be between 1 and 100");
      }
      if (samples == 0) {
        return 0;
      }
      long wholeHundreds = samples / 100;
      long remainder = samples % 100;
      long rank =
          wholeHundreds * percentile + (remainder * percentile + 99) / 100;
      long cumulative = 0;
      for (int index = 0; index < buckets.length; ++index) {
        cumulative += buckets[index];
        if (cumulative >= rank) {
          if (index == LATENCY_REGULAR_BUCKET_COUNT) {
            return overflowMaximumNs;
          }
          return (index + 1L) * LATENCY_BUCKET_WIDTH_NS;
        }
      }
      throw new IllegalStateException("latency histogram sample count is inconsistent");
    }
  }
}
