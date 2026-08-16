package com.agoessling.swingcapture.core.coordination;

import java.util.Objects;

/** A bounded estimate of node-clock minus coordinator-clock offset. */
public record ClockOffsetEstimate(
    String nodeId,
    long offsetNs,
    long uncertaintyNs,
    long minimumRoundTripNs,
    long maximumRoundTripNs,
    int sampleCount) {
  public ClockOffsetEstimate {
    Objects.requireNonNull(nodeId, "nodeId");
    if (nodeId.isBlank()) {
      throw new IllegalArgumentException("nodeId cannot be blank");
    }
    if (uncertaintyNs < 0 || minimumRoundTripNs < 0 || maximumRoundTripNs < minimumRoundTripNs) {
      throw new IllegalArgumentException("clock uncertainty and RTT bounds are inconsistent");
    }
    if (sampleCount < 0) {
      throw new IllegalArgumentException("sampleCount cannot be negative");
    }
  }

  /** Identity mapping for a node whose clock is the coordinator clock. */
  public static ClockOffsetEstimate identity(String nodeId) {
    return new ClockOffsetEstimate(nodeId, 0, 0, 0, 0, 0);
  }

  public long offsetLowerBoundNs() {
    return Math.subtractExact(offsetNs, uncertaintyNs);
  }

  public long offsetUpperBoundNs() {
    return Math.addExact(offsetNs, uncertaintyNs);
  }
}
