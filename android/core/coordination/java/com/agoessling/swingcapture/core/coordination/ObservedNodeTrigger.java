package com.agoessling.swingcapture.core.coordination;

import java.util.Objects;

/** A trigger mapped to the coordinator monotonic clock with a conservative half-width. */
public record ObservedNodeTrigger(
    NodeTriggerReport report,
    long coordinatorTimestampNs,
    long coordinatorUncertaintyNs,
    long receivedAtCoordinatorNs) {
  public ObservedNodeTrigger {
    Objects.requireNonNull(report, "report");
    if (coordinatorTimestampNs < 0
        || coordinatorUncertaintyNs < 0
        || receivedAtCoordinatorNs < 0) {
      throw new IllegalArgumentException("mapped trigger values cannot be negative");
    }
  }

  public static ObservedNodeTrigger map(
      NodeTriggerReport report, ClockOffsetEstimate estimate, long receivedAtCoordinatorNs) {
    Objects.requireNonNull(report, "report");
    Objects.requireNonNull(estimate, "estimate");
    if (!report.nodeId().equals(estimate.nodeId())) {
      throw new IllegalArgumentException("clock estimate belongs to a different node");
    }
    long mappedTimestamp = Math.subtractExact(report.triggerTimestampNs(), estimate.offsetNs());
    long uncertainty =
        Math.addExact(report.timestampUncertaintyNs(), estimate.uncertaintyNs());
    return new ObservedNodeTrigger(report, mappedTimestamp, uncertainty, receivedAtCoordinatorNs);
  }

  public long earliestCoordinatorTimestampNs() {
    return Math.subtractExact(coordinatorTimestampNs, coordinatorUncertaintyNs);
  }

  public long latestCoordinatorTimestampNs() {
    return Math.addExact(coordinatorTimestampNs, coordinatorUncertaintyNs);
  }
}
