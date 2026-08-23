package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.ClockExchangeSample;

public final class PeerClockSynchronizerTest {
  private PeerClockSynchronizerTest() {}

  public static void main(String[] args) {
    requiresCompleteBatchAndMapsPeerClock();
    enforcesIdentityFreshnessAndUncertainty();
    refreshesInBoundedBatches();
    retainsFreshUsableSnapshotAcrossNoisyRefresh();
    rejectsNonNewerAndExpiredBatchSamples();
    withdrawsEstimateImmediatelyAfterPeerClockJump();
    rejectsInvalidMapping();
    mapsLocalClockAndRejectsOverflow();
  }

  private static void requiresCompleteBatchAndMapsPeerClock() {
    PeerClockSynchronizer synchronizer =
        new PeerClockSynchronizer("http://leader.local:8088", "leader");
    synchronizer.record(sample(1_000, 50, 8, 4, 8));
    synchronizer.record(sample(2_000, 50, 6, 3, 6));
    check(synchronizer.snapshot().isEmpty(), "two samples must not publish an estimate");
    synchronizer.record(sample(3_000, 50, 5, 2, 5));

    PeerClockSynchronizer.Snapshot snapshot = synchronizer.snapshot().orElseThrow();
    check(snapshot.peerNodeId().equals("leader"), "peer identity");
    check(snapshot.peerMinusLocalNanos() == 50, "exact intersected offset");
    check(snapshot.uncertaintyNanos() == 5, "intersected uncertainty");
    check(snapshot.peerToLocalNanos(4_050) == 4_000, "peer-to-local mapping");
  }

  private static void enforcesIdentityFreshnessAndUncertainty() {
    PeerClockSynchronizer synchronizer = readySynchronizer(10_000, 100);
    PeerClockSynchronizer.Snapshot snapshot = synchronizer.snapshot().orElseThrow();
    long measured = snapshot.measuredAtLocalNanos();
    check(snapshot.belongsTo("http://leader.local:8088", "leader"), "peer identity");
    check(
        !snapshot.belongsTo("http://other.local:8088", "leader"),
        "wrong peer origin");
    check(
        !snapshot.belongsTo("http://leader.local:8088", "other"),
        "wrong peer identity");
    check(snapshot.usableAt(measured + 1_000, 1_000, 10), "fresh estimate");
    check(
        !snapshot.usableAt(measured + 1_001, 1_000, 10),
        "stale estimate");
    check(!snapshot.usableAt(measured - 1, 1_000, 10), "future estimate");
    check(!snapshot.usableAt(measured, 1_000, 4), "excess uncertainty");
  }

  private static void refreshesInBoundedBatches() {
    PeerClockSynchronizer synchronizer = readySynchronizer(10_000, 100);
    PeerClockSynchronizer.Snapshot first = synchronizer.snapshot().orElseThrow();
    synchronizer.record(sample(20_000, 102, 5, 2, 5));
    synchronizer.record(sample(21_000, 102, 5, 2, 5));
    check(synchronizer.snapshot().orElseThrow().equals(first), "partial refresh is not published");
    synchronizer.record(sample(22_000, 102, 5, 2, 5));
    PeerClockSynchronizer.Snapshot refreshed = synchronizer.snapshot().orElseThrow();
    check(refreshed.peerMinusLocalNanos() == 102, "new batch replaces offset");
    check(refreshed.measuredAtLocalNanos() > first.measuredAtLocalNanos(), "newer evidence");
  }

  private static void retainsFreshUsableSnapshotAcrossNoisyRefresh() {
    PeerClockSynchronizer synchronizer = readySynchronizer(1_000_000_000L, 100);
    PeerClockSynchronizer.Snapshot usable = synchronizer.snapshot().orElseThrow();
    long noisyRoundTripHalf = 40_000_000L;
    synchronizer.record(sample(1_100_000_000L, 100, noisyRoundTripHalf, 2, noisyRoundTripHalf));
    synchronizer.record(sample(1_200_000_000L, 100, noisyRoundTripHalf, 2, noisyRoundTripHalf));
    synchronizer.record(sample(1_300_000_000L, 100, noisyRoundTripHalf, 2, noisyRoundTripHalf));

    check(
        synchronizer.snapshot().orElseThrow().equals(usable),
        "overlapping noisy batch must not evict a fresh usable mapping");
  }

  private static void rejectsNonNewerAndExpiredBatchSamples() {
    PeerClockSynchronizer initialBatch =
        new PeerClockSynchronizer("http://leader.local:8088", "leader");
    initialBatch.record(sample(2_000, 100, 5, 2, 5));
    expectFailure(() -> initialBatch.record(sample(1_999, 100, 5, 2, 5)));
    check(initialBatch.snapshot().isEmpty(), "out-of-order initial samples cannot publish");

    PeerClockSynchronizer synchronizer = readySynchronizer(10_000, 100);
    PeerClockSynchronizer.Snapshot current = synchronizer.snapshot().orElseThrow();
    expectFailure(() -> synchronizer.record(sample(5_000, 100, 5, 2, 5)));
    check(
        synchronizer.snapshot().orElseThrow().equals(current),
        "out-of-order sample cannot replace a newer snapshot");

    PeerClockSynchronizer delayed =
        new PeerClockSynchronizer("http://leader.local:8088", "leader");
    delayed.record(sample(1_000, 100, 5, 2, 5));
    delayed.record(sample(2_000, 100, 5, 2, 5));
    long delayedThird = 1_000 + PeerClockSynchronizer.MAXIMUM_SNAPSHOT_AGE_NANOS + 1;
    delayed.record(sample(delayedThird, 100, 5, 2, 5));
    check(delayed.snapshot().isEmpty(), "expired samples cannot become a fresh estimate");
    delayed.record(sample(delayedThird + 1_000, 100, 5, 2, 5));
    delayed.record(sample(delayedThird + 2_000, 100, 5, 2, 5));
    check(delayed.snapshot().isPresent(), "fresh replacement batch publishes normally");
  }

  private static void rejectsInvalidMapping() {
    PeerClockSynchronizer.Snapshot snapshot = readySynchronizer(10_000, 100).snapshot().orElseThrow();
    expectFailure(() -> snapshot.peerToLocalNanos(-1));
    PeerClockSynchronizer.Snapshot largeOffset =
        new PeerClockSynchronizer.Snapshot(
            "http://leader.local:8088", "leader", 1_000, 0, 1, 0, 0, 3);
    expectFailure(() -> largeOffset.peerToLocalNanos(999));
  }

  private static void mapsLocalClockAndRejectsOverflow() {
    PeerClockSynchronizer.Snapshot snapshot =
        new PeerClockSynchronizer.Snapshot(
            "http://peer.local:8088", "peer", 200, 5, 1_000, 10, 20, 3);
    check(snapshot.localToPeerNanos(1_000) == 1_200, "local-to-peer mapping");
    check(snapshot.peerToLocalNanos(1_200) == 1_000, "mapping round trip");
    expectFailure(() -> snapshot.localToPeerNanos(Long.MAX_VALUE));
    PeerClockSynchronizer.Snapshot negativeOffset =
        new PeerClockSynchronizer.Snapshot(
            "http://peer.local:8088", "peer", -200, 5, 1_000, 10, 20, 3);
    expectFailure(() -> negativeOffset.localToPeerNanos(100));
  }

  private static void withdrawsEstimateImmediatelyAfterPeerClockJump() {
    PeerClockSynchronizer synchronizer = readySynchronizer(10_000, 100);
    synchronizer.record(sample(20_000, 1_000_000, 5, 2, 5));
    check(synchronizer.snapshot().isEmpty(), "disjoint peer clock must withdraw old mapping");
    synchronizer.record(sample(20_100, 1_000_000, 5, 2, 5));
    synchronizer.record(sample(20_200, 1_000_000, 5, 2, 5));
    check(
        synchronizer.snapshot().orElseThrow().peerMinusLocalNanos() == 1_000_000,
        "replacement batch restores mapping");
  }

  private static PeerClockSynchronizer readySynchronizer(long start, long offset) {
    PeerClockSynchronizer synchronizer =
        new PeerClockSynchronizer("http://leader.local:8088", "leader");
    synchronizer.record(sample(start, offset, 5, 2, 5));
    synchronizer.record(sample(start + 100, offset, 5, 2, 5));
    synchronizer.record(sample(start + 200, offset, 5, 2, 5));
    return synchronizer;
  }

  private static ClockExchangeSample sample(
      long localSend, long peerMinusLocal, long outbound, long processing, long inbound) {
    long peerReceive = localSend + outbound + peerMinusLocal;
    long peerSend = peerReceive + processing;
    long localReceive = localSend + outbound + processing + inbound;
    return new ClockExchangeSample(localSend, peerReceive, peerSend, localReceive);
  }

  private static void expectFailure(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
    } catch (ArithmeticException | IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
