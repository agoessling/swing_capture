package com.agoessling.swingcapture.audio;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.Optional;

/**
 * Bounded, validating map from AudioRecord frame positions to Android BOOTTIME nanoseconds.
 *
 * <p>The Android boundary should pass each successful {@code AudioRecord.getTimestamp(...,
 * TIMEBASE_BOOTTIME)} result here. A map becomes usable only after two mutually consistent
 * observations establish that the audio clock is running at the expected rate.
 */
public final class AudioTimestampMapper {
  public record Config(
      int maximumEvidenceCount,
      long maximumEvidenceUncertaintyNanos,
      double maximumClockRateErrorPpm,
      long maximumExtrapolationNanos) {
    public Config {
      if (maximumEvidenceCount < 2) {
        throw new IllegalArgumentException("maximumEvidenceCount must be at least two");
      }
      if (maximumEvidenceUncertaintyNanos < 0) {
        throw new IllegalArgumentException("maximumEvidenceUncertaintyNanos cannot be negative");
      }
      if (!Double.isFinite(maximumClockRateErrorPpm) || maximumClockRateErrorPpm < 0.0) {
        throw new IllegalArgumentException("maximumClockRateErrorPpm must be finite and nonnegative");
      }
      if (maximumExtrapolationNanos < 0) {
        throw new IllegalArgumentException("maximumExtrapolationNanos cannot be negative");
      }
    }

    public static Config defaults() {
      return new Config(8, 2_000_000L, 2_000.0, 500_000_000L);
    }
  }

  public enum RejectionReason {
    NONE,
    NEGATIVE_FRAME_POSITION,
    INVALID_BOOTTIME,
    INVALID_UNCERTAINTY,
    NONMONOTONIC_FRAME_POSITION,
    NONMONOTONIC_BOOTTIME,
    CLOCK_RATE_OUT_OF_RANGE,
  }

  /** Result of validating one hardware timestamp observation. */
  public record ObservationResult(
      boolean accepted,
      RejectionReason rejectionReason,
      double observedRateHz,
      int retainedEvidenceCount) {}

  /** A BOOTTIME estimate with a conservative closed uncertainty interval. */
  public record Estimate(
      long framePosition,
      long boottimeNanos,
      long uncertaintyNanos,
      long earliestBoottimeNanos,
      long latestBoottimeNanos,
      int evidenceCount,
      double validatedRateHz) {
    public Estimate {
      if (uncertaintyNanos < 0) {
        throw new IllegalArgumentException("uncertaintyNanos cannot be negative");
      }
      if (earliestBoottimeNanos > boottimeNanos
          || latestBoottimeNanos < boottimeNanos) {
        throw new IllegalArgumentException("estimate lies outside its uncertainty interval");
      }
    }
  }

  private record Evidence(long framePosition, long boottimeNanos, long uncertaintyNanos) {}

  private final Config config;
  private final Deque<Evidence> evidence = new ArrayDeque<>();

  public AudioTimestampMapper() {
    this(Config.defaults());
  }

  public AudioTimestampMapper(Config config) {
    this.config = java.util.Objects.requireNonNull(config, "config");
  }

  /**
   * Adds a successful AudioRecord hardware timestamp.
   *
   * <p>{@code uncertaintyNanos} is supplied by the Android boundary and should include its
   * timestamp-query and scheduling uncertainty. Invalid evidence is rejected without changing the
   * retained model.
   */
  public synchronized ObservationResult observe(
      long framePosition, long boottimeNanos, long uncertaintyNanos) {
    RejectionReason basicRejection = validateBasic(framePosition, boottimeNanos, uncertaintyNanos);
    if (basicRejection != RejectionReason.NONE) {
      return rejected(basicRejection, Double.NaN);
    }
    if (!evidence.isEmpty()) {
      Evidence latest = evidence.getLast();
      if (framePosition <= latest.framePosition()) {
        return rejected(RejectionReason.NONMONOTONIC_FRAME_POSITION, Double.NaN);
      }
      if (boottimeNanos <= latest.boottimeNanos()) {
        return rejected(RejectionReason.NONMONOTONIC_BOOTTIME, Double.NaN);
      }
    }

    double observedRateHz = Double.NaN;
    for (Evidence previous : evidence) {
      long deltaFrames = framePosition - previous.framePosition();
      long deltaNanos = boottimeNanos - previous.boottimeNanos();
      observedRateHz = deltaFrames * 1_000_000_000.0 / deltaNanos;
      double expectedNanos = deltaFrames * 1_000_000_000.0 / ImpactDetector.SAMPLE_RATE_HZ;
      double allowedRateErrorNanos =
          expectedNanos * config.maximumClockRateErrorPpm() / 1_000_000.0;
      double allowedNanos =
          allowedRateErrorNanos + previous.uncertaintyNanos() + uncertaintyNanos + 1.0;
      if (Math.abs(deltaNanos - expectedNanos) > allowedNanos) {
        return rejected(RejectionReason.CLOCK_RATE_OUT_OF_RANGE, observedRateHz);
      }
    }

    evidence.addLast(new Evidence(framePosition, boottimeNanos, uncertaintyNanos));
    while (evidence.size() > config.maximumEvidenceCount()) {
      evidence.removeFirst();
    }
    return new ObservationResult(true, RejectionReason.NONE, observedRateHz, evidence.size());
  }

  /** Maps one frame only after at least two accepted observations validate the audio clock. */
  public synchronized Optional<Estimate> estimateBoottime(long framePosition) {
    if (framePosition < 0 || evidence.size() < 2) {
      return Optional.empty();
    }

    Evidence first = evidence.getFirst();
    Evidence last = evidence.getLast();
    long extrapolationFrames = 0;
    if (framePosition < first.framePosition()) {
      extrapolationFrames = first.framePosition() - framePosition;
    } else if (framePosition > last.framePosition()) {
      extrapolationFrames = framePosition - last.framePosition();
    }
    double extrapolationNanos =
        extrapolationFrames * 1_000_000_000.0 / ImpactDetector.SAMPLE_RATE_HZ;
    if (extrapolationNanos > config.maximumExtrapolationNanos()) {
      return Optional.empty();
    }

    Evidence best = null;
    long bestUncertainty = Long.MAX_VALUE;
    for (Evidence candidate : evidence) {
      long frameDistance = absoluteDifference(framePosition, candidate.framePosition());
      double durationNanos =
          frameDistance * 1_000_000_000.0 / ImpactDetector.SAMPLE_RATE_HZ;
      long rateUncertainty =
          saturatedCeiling(durationNanos * config.maximumClockRateErrorPpm() / 1_000_000.0);
      long candidateUncertainty = saturatedAdd(candidate.uncertaintyNanos(), rateUncertainty);
      candidateUncertainty = saturatedAdd(candidateUncertainty, 1L);
      if (candidateUncertainty < bestUncertainty) {
        best = candidate;
        bestUncertainty = candidateUncertainty;
      }
    }

    if (best == null) {
      return Optional.empty();
    }
    double offsetNanos =
        (framePosition - best.framePosition())
            * 1_000_000_000.0
            / ImpactDetector.SAMPLE_RATE_HZ;
    if (!Double.isFinite(offsetNanos)
        || offsetNanos > Long.MAX_VALUE
        || offsetNanos < Long.MIN_VALUE) {
      return Optional.empty();
    }
    long mapped = saturatedAddSigned(best.boottimeNanos(), Math.round(offsetNanos));
    long earliest = saturatedSubtract(mapped, bestUncertainty);
    long latest = saturatedAdd(mapped, bestUncertainty);
    return Optional.of(
        new Estimate(
            framePosition,
            mapped,
            bestUncertainty,
            earliest,
            latest,
            evidence.size(),
            validatedRateHz()));
  }

  public synchronized int retainedEvidenceCount() {
    return evidence.size();
  }

  public synchronized boolean clockValidated() {
    return evidence.size() >= 2;
  }

  public synchronized double validatedRateHz() {
    if (evidence.size() < 2) {
      return Double.NaN;
    }
    Evidence first = evidence.getFirst();
    Evidence last = evidence.getLast();
    return (last.framePosition() - first.framePosition())
        * 1_000_000_000.0
        / (last.boottimeNanos() - first.boottimeNanos());
  }

  public synchronized void reset() {
    evidence.clear();
  }

  public Config config() {
    return config;
  }

  private RejectionReason validateBasic(
      long framePosition, long boottimeNanos, long uncertaintyNanos) {
    if (framePosition < 0) {
      return RejectionReason.NEGATIVE_FRAME_POSITION;
    }
    if (boottimeNanos <= 0) {
      return RejectionReason.INVALID_BOOTTIME;
    }
    if (uncertaintyNanos < 0
        || uncertaintyNanos > config.maximumEvidenceUncertaintyNanos()) {
      return RejectionReason.INVALID_UNCERTAINTY;
    }
    return RejectionReason.NONE;
  }

  private ObservationResult rejected(RejectionReason reason, double observedRateHz) {
    return new ObservationResult(false, reason, observedRateHz, evidence.size());
  }

  private static long absoluteDifference(long left, long right) {
    if (left >= right) {
      return left - right;
    }
    return right - left;
  }

  private static long saturatedCeiling(double value) {
    if (value >= Long.MAX_VALUE) {
      return Long.MAX_VALUE;
    }
    return (long) Math.ceil(value);
  }

  private static long saturatedAdd(long left, long right) {
    if (left > Long.MAX_VALUE - right) {
      return Long.MAX_VALUE;
    }
    return left + right;
  }

  private static long saturatedSubtract(long value, long decrement) {
    if (value < Long.MIN_VALUE + decrement) {
      return Long.MIN_VALUE;
    }
    return value - decrement;
  }

  private static long saturatedAddSigned(long value, long increment) {
    if (increment > 0 && value > Long.MAX_VALUE - increment) {
      return Long.MAX_VALUE;
    }
    if (increment < 0 && value < Long.MIN_VALUE - increment) {
      return Long.MIN_VALUE;
    }
    return value + increment;
  }
}
