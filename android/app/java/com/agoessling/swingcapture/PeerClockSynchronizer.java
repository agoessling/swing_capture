package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.ClockExchangeEstimateResult;
import com.agoessling.swingcapture.core.coordination.ClockExchangeEstimator;
import com.agoessling.swingcapture.core.coordination.ClockExchangeSample;
import com.agoessling.swingcapture.core.coordination.ClockOffsetEstimate;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.TimeUnit;

/** Maintains bounded, repeatedly refreshed evidence mapping one peer's boot clock to this node. */
final class PeerClockSynchronizer {
  private static final int SAMPLES_PER_ESTIMATE = 3;
  static final long MAXIMUM_SNAPSHOT_AGE_NANOS = TimeUnit.SECONDS.toNanos(10);
  // Direct screen-off phone-to-phone Wi-Fi measured a 43 ms best HTTP round trip. A 25 ms
  // midpoint bound admits that transport without approaching the separate 80 ms acoustic
  // candidate-matching tolerance.
  static final long MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS = TimeUnit.MILLISECONDS.toNanos(25);

  record Snapshot(
      String peerOrigin,
      String peerNodeId,
      long peerMinusLocalNanos,
      long uncertaintyNanos,
      long measuredAtLocalNanos,
      long minimumRoundTripNanos,
      long maximumRoundTripNanos,
      int sampleCount) {
    Snapshot {
      Objects.requireNonNull(peerOrigin, "peerOrigin");
      Objects.requireNonNull(peerNodeId, "peerNodeId");
      if (peerOrigin.isBlank() || peerNodeId.isBlank()) {
        throw new IllegalArgumentException("peer origin and node ID cannot be blank");
      }
      if (uncertaintyNanos < 0
          || measuredAtLocalNanos < 0
          || minimumRoundTripNanos < 0
          || maximumRoundTripNanos < minimumRoundTripNanos
          || sampleCount < 2) {
        throw new IllegalArgumentException("invalid peer clock estimate");
      }
    }

    boolean belongsTo(String expectedPeerOrigin, String expectedPeerNodeId) {
      Objects.requireNonNull(expectedPeerOrigin, "expectedPeerOrigin");
      Objects.requireNonNull(expectedPeerNodeId, "expectedPeerNodeId");
      return peerOrigin.equals(expectedPeerOrigin) && peerNodeId.equals(expectedPeerNodeId);
    }

    boolean usableAt(
        long nowLocalNanos, long maximumAgeNanos, long maximumUncertaintyNanos) {
      if (nowLocalNanos < 0 || maximumAgeNanos < 0 || maximumUncertaintyNanos < 0) {
        throw new IllegalArgumentException("clock freshness limits cannot be negative");
      }
      return nowLocalNanos >= measuredAtLocalNanos
          && nowLocalNanos - measuredAtLocalNanos <= maximumAgeNanos
          && uncertaintyNanos <= maximumUncertaintyNanos;
    }

    /** Maps a timestamp on the peer clock into this node's elapsed-realtime clock. */
    long peerToLocalNanos(long peerTimestampNanos) {
      if (peerTimestampNanos < 0) {
        throw new IllegalArgumentException("peer timestamp cannot be negative");
      }
      long localTimestamp = Math.subtractExact(peerTimestampNanos, peerMinusLocalNanos);
      if (localTimestamp < 0) {
        throw new IllegalArgumentException("mapped local timestamp cannot be negative");
      }
      return localTimestamp;
    }

    /** Maps a timestamp on this node's elapsed-realtime clock into the peer clock. */
    long localToPeerNanos(long localTimestampNanos) {
      if (localTimestampNanos < 0) {
        throw new IllegalArgumentException("local timestamp cannot be negative");
      }
      long peerTimestamp = Math.addExact(localTimestampNanos, peerMinusLocalNanos);
      if (peerTimestamp < 0) {
        throw new IllegalArgumentException("mapped peer timestamp cannot be negative");
      }
      return peerTimestamp;
    }
  }

  private final String peerOrigin;
  private final String peerNodeId;
  private ClockExchangeEstimator estimator;
  private long batchFirstCoordinatorReceiveNanos = -1;
  private long lastCoordinatorReceiveNanos = -1;
  private Snapshot latest;

  PeerClockSynchronizer(String peerOrigin, String peerNodeId) {
    this.peerOrigin = Objects.requireNonNull(peerOrigin, "peerOrigin");
    this.peerNodeId = Objects.requireNonNull(peerNodeId, "peerNodeId");
    if (peerOrigin.isBlank() || peerNodeId.isBlank()) {
      throw new IllegalArgumentException("peer origin and node ID cannot be blank");
    }
    estimator = new ClockExchangeEstimator(peerNodeId, SAMPLES_PER_ESTIMATE);
  }

  String peerNodeId() {
    return peerNodeId;
  }

  /** Adds one bounded exchange. Every three samples replace the estimate as one atomic batch. */
  synchronized void record(ClockExchangeSample sample) {
    Objects.requireNonNull(sample, "sample");
    if (sample.coordinatorReceiveNs() <= lastCoordinatorReceiveNanos) {
      throw new IllegalArgumentException("clock exchanges must be strictly receive-ordered");
    }
    if (latest != null && !overlaps(latest, sample)) {
      // A peer reboot can retain the same node ID while moving its elapsed-realtime origin by
      // hours. Withdraw the now-invalid mapping on the first disjoint exchange rather than
      // waiting for a complete replacement batch or the normal freshness deadline.
      latest = null;
      estimator = new ClockExchangeEstimator(peerNodeId, SAMPLES_PER_ESTIMATE);
      batchFirstCoordinatorReceiveNanos = -1;
    }
    if (batchFirstCoordinatorReceiveNanos >= 0
        && sample.coordinatorReceiveNs() - batchFirstCoordinatorReceiveNanos
            > MAXIMUM_SNAPSHOT_AGE_NANOS) {
      // A late third response cannot make two expired samples fresh merely by donating its newer
      // receive timestamp. Start a new bounded batch with that response and publish nothing yet.
      estimator = new ClockExchangeEstimator(peerNodeId, SAMPLES_PER_ESTIMATE);
      batchFirstCoordinatorReceiveNanos = -1;
    }
    if (estimator.sampleCount() == 0) {
      batchFirstCoordinatorReceiveNanos = sample.coordinatorReceiveNs();
    }
    estimator.add(sample);
    lastCoordinatorReceiveNanos = sample.coordinatorReceiveNs();
    if (estimator.sampleCount() < SAMPLES_PER_ESTIMATE) {
      return;
    }
    ClockExchangeEstimateResult result = estimator.estimate();
    Optional<ClockOffsetEstimate> estimate = result.estimate();
    if (estimate.isPresent()) {
      ClockOffsetEstimate value = estimate.orElseThrow();
      Snapshot candidate =
          new Snapshot(
              peerOrigin,
              value.nodeId(),
              value.offsetNs(),
              value.uncertaintyNs(),
              sample.coordinatorReceiveNs(),
              value.minimumRoundTripNs(),
              value.maximumRoundTripNs(),
              value.sampleCount());
      // Do not let one overlapping, high-jitter batch evict a still-fresh mapping that satisfies
      // impact-delivery policy. A disjoint sample already withdraws the old mapping above, and an
      // expired mapping is still replaced so status exposes the newest diagnostic evidence.
      if (latest == null
          || candidate.uncertaintyNanos() <= MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS
          || !latest.usableAt(
              candidate.measuredAtLocalNanos(),
              MAXIMUM_SNAPSHOT_AGE_NANOS,
              MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS)) {
        latest = candidate;
      }
    }
    // Start a fresh bounded batch even when the prior bounds were inconsistent. Keep the most
    // recent good snapshot until its caller-enforced freshness deadline expires.
    estimator = new ClockExchangeEstimator(peerNodeId, SAMPLES_PER_ESTIMATE);
    batchFirstCoordinatorReceiveNanos = -1;
  }

  synchronized Optional<Snapshot> snapshot() {
    return Optional.ofNullable(latest);
  }

  private static boolean overlaps(Snapshot snapshot, ClockExchangeSample sample) {
    try {
      long snapshotLower =
          Math.subtractExact(snapshot.peerMinusLocalNanos(), snapshot.uncertaintyNanos());
      long snapshotUpper =
          Math.addExact(snapshot.peerMinusLocalNanos(), snapshot.uncertaintyNanos());
      return snapshotLower <= sample.offsetUpperBoundNs()
          && sample.offsetLowerBoundNs() <= snapshotUpper;
    } catch (ArithmeticException invalidBounds) {
      return false;
    }
  }
}
