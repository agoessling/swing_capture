package com.agoessling.swingcapture.core.coordination;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/**
 * Intersects the offset bounds from repeated four-timestamp exchanges. An empty intersection is
 * reported rather than silently choosing an overconfident offset.
 */
public final class ClockExchangeEstimator {
  private static final int DEFAULT_MINIMUM_SAMPLES = 3;
  private static final int MAXIMUM_SAMPLES = 64;

  private final String nodeId;
  private final int minimumSamples;
  private final List<ClockExchangeSample> samples = new ArrayList<>();

  public ClockExchangeEstimator(String nodeId) {
    this(nodeId, DEFAULT_MINIMUM_SAMPLES);
  }

  public ClockExchangeEstimator(String nodeId, int minimumSamples) {
    this.nodeId = Objects.requireNonNull(nodeId, "nodeId");
    if (nodeId.isBlank()) {
      throw new IllegalArgumentException("nodeId cannot be blank");
    }
    if (minimumSamples < 2 || minimumSamples > MAXIMUM_SAMPLES) {
      throw new IllegalArgumentException("minimumSamples must be between 2 and 64");
    }
    this.minimumSamples = minimumSamples;
  }

  public void add(ClockExchangeSample sample) {
    Objects.requireNonNull(sample, "sample");
    if (samples.size() == MAXIMUM_SAMPLES) {
      throw new IllegalStateException("clock estimator sample capacity is exhausted");
    }
    if (!samples.isEmpty()
        && sample.coordinatorReceiveNs()
            <= samples.get(samples.size() - 1).coordinatorReceiveNs()) {
      throw new IllegalArgumentException(
          "clock exchanges must have increasing coordinator receive timestamps");
    }
    samples.add(sample);
  }

  public int sampleCount() {
    return samples.size();
  }

  public ClockExchangeEstimateResult estimate() {
    if (samples.size() < minimumSamples) {
      return new ClockExchangeEstimateResult(
          ClockExchangeEstimateStatus.INSUFFICIENT_SAMPLES,
          Optional.empty(),
          "requires " + minimumSamples + " samples; received " + samples.size());
    }

    long lowerBound = Long.MIN_VALUE;
    long upperBound = Long.MAX_VALUE;
    long minimumRoundTrip = Long.MAX_VALUE;
    long maximumRoundTrip = 0;
    for (ClockExchangeSample sample : samples) {
      lowerBound = Math.max(lowerBound, sample.offsetLowerBoundNs());
      upperBound = Math.min(upperBound, sample.offsetUpperBoundNs());
      minimumRoundTrip = Math.min(minimumRoundTrip, sample.networkRoundTripNs());
      maximumRoundTrip = Math.max(maximumRoundTrip, sample.networkRoundTripNs());
    }
    if (lowerBound > upperBound) {
      return new ClockExchangeEstimateResult(
          ClockExchangeEstimateStatus.INCONSISTENT_BOUNDS,
          Optional.empty(),
          "repeated offset bounds do not intersect");
    }

    long width = Math.subtractExact(upperBound, lowerBound);
    long midpoint = Math.addExact(lowerBound, width / 2);
    long uncertainty = Math.addExact(width / 2, width % 2);
    ClockOffsetEstimate estimate =
        new ClockOffsetEstimate(
            nodeId,
            midpoint,
            uncertainty,
            minimumRoundTrip,
            maximumRoundTrip,
            samples.size());
    return new ClockExchangeEstimateResult(
        ClockExchangeEstimateStatus.READY, Optional.of(estimate), "ready");
  }
}
