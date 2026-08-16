package com.agoessling.swingcapture.core.coordination;

/**
 * One four-timestamp exchange. Coordinator timestamps are t1/t4 and node timestamps are t2/t3.
 * The model assumes nonnegative path delay and a stable offset during the exchange.
 */
public record ClockExchangeSample(
    long coordinatorSendNs, long nodeReceiveNs, long nodeSendNs, long coordinatorReceiveNs) {
  public ClockExchangeSample {
    if (coordinatorSendNs < 0
        || nodeReceiveNs < 0
        || nodeSendNs < 0
        || coordinatorReceiveNs < 0) {
      throw new IllegalArgumentException("clock-exchange timestamps cannot be negative");
    }
    if (coordinatorReceiveNs < coordinatorSendNs) {
      throw new IllegalArgumentException("coordinator receive timestamp precedes send timestamp");
    }
    if (nodeSendNs < nodeReceiveNs) {
      throw new IllegalArgumentException("node send timestamp precedes receive timestamp");
    }
    if (networkRoundTripNs(coordinatorSendNs, nodeReceiveNs, nodeSendNs, coordinatorReceiveNs)
        < 0) {
      throw new IllegalArgumentException("node processing time exceeds total exchange time");
    }
  }

  /** Lower inclusive bound for node-clock minus coordinator-clock offset. */
  public long offsetLowerBoundNs() {
    return Math.subtractExact(nodeSendNs, coordinatorReceiveNs);
  }

  /** Upper inclusive bound for node-clock minus coordinator-clock offset. */
  public long offsetUpperBoundNs() {
    return Math.subtractExact(nodeReceiveNs, coordinatorSendNs);
  }

  /** Total exchange duration after removing time spent processing on the node. */
  public long networkRoundTripNs() {
    return networkRoundTripNs(
        coordinatorSendNs, nodeReceiveNs, nodeSendNs, coordinatorReceiveNs);
  }

  private static long networkRoundTripNs(long t1, long t2, long t3, long t4) {
    long coordinatorElapsed = Math.subtractExact(t4, t1);
    long nodeProcessing = Math.subtractExact(t3, t2);
    return Math.subtractExact(coordinatorElapsed, nodeProcessing);
  }
}
