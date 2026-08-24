package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/** Pure rolling policy for bounded, directed application-level pair-network evidence. */
final class PairNetworkHealthPolicy {
  enum State {
    GOOD("good"),
    DEGRADED("degraded"),
    UNUSABLE("unusable");

    private final String wireName;

    State(String wireName) {
      this.wireName = wireName;
    }

    String wireName() {
      return wireName;
    }
  }

  record Config(
      long maximumSampleAgeNanos,
      long minimumTransferBitsPerSecond,
      int degradedRoundsToWorsen,
      int usableRoundsToRecover,
      int goodRoundsToRecover) {
    Config {
      if (maximumSampleAgeNanos <= 0
          || minimumTransferBitsPerSecond <= 0
          || degradedRoundsToWorsen <= 0
          || usableRoundsToRecover <= 0
          || goodRoundsToRecover < usableRoundsToRecover) {
        throw new IllegalArgumentException("pair network-health policy configuration is invalid");
      }
    }
  }

  record PeerTarget(String origin, String nodeId) {
    PeerTarget {
      Objects.requireNonNull(origin, "origin");
      Objects.requireNonNull(nodeId, "nodeId");
      if (origin.isBlank() || nodeId.isBlank()) {
        throw new IllegalArgumentException("peer origin and node ID cannot be blank");
      }
    }
  }

  /** Evidence for requests initiated in one direction and a download from their destination. */
  record DirectionEvidence(
      int attempts,
      int successes,
      int timeouts,
      List<Long> roundTripNanos,
      long transferBytes,
      long transferDurationNanos,
      boolean transferComplete) {
    DirectionEvidence {
      Objects.requireNonNull(roundTripNanos, "roundTripNanos");
      if (attempts <= 0
          || successes < 0
          || successes > attempts
          || timeouts < 0
          || timeouts > attempts - successes
          || roundTripNanos.size() != successes
          || transferBytes < 0
          || transferDurationNanos < 0
          || (transferComplete && (transferBytes == 0 || transferDurationNanos == 0))) {
        throw new IllegalArgumentException("directed network evidence is invalid");
      }
      ArrayList<Long> sorted = new ArrayList<>(roundTripNanos);
      for (long roundTrip : sorted) {
        if (roundTrip < 0) {
          throw new IllegalArgumentException("round-trip latency cannot be negative");
        }
      }
      Collections.sort(sorted);
      roundTripNanos = List.copyOf(sorted);
    }

    long minimumRoundTripNanos() {
      return roundTripNanos.isEmpty() ? 0 : roundTripNanos.get(0);
    }

    long medianRoundTripNanos() {
      return percentileRoundTripNanos(50);
    }

    long p95RoundTripNanos() {
      return percentileRoundTripNanos(95);
    }

    long maximumRoundTripNanos() {
      return roundTripNanos.isEmpty() ? 0 : roundTripNanos.get(roundTripNanos.size() - 1);
    }

    long jitterNanos() {
      return roundTripNanos.size() < 2
          ? 0
          : Math.subtractExact(maximumRoundTripNanos(), minimumRoundTripNanos());
    }

    double transferBitsPerSecond() {
      if (transferDurationNanos == 0) {
        return 0.0;
      }
      return transferBytes * 8.0 * 1_000_000_000.0 / transferDurationNanos;
    }

    boolean transferSlow(long minimumBitsPerSecond) {
      return transferComplete && transferBitsPerSecond() < minimumBitsPerSecond;
    }

    private long percentileRoundTripNanos(int percentile) {
      if (roundTripNanos.isEmpty()) {
        return 0;
      }
      int rank = (percentile * roundTripNanos.size() + 99) / 100;
      return roundTripNanos.get(Math.max(0, rank - 1));
    }
  }

  record Round(
      long measuredAtElapsedRealtimeNanos,
      DirectionEvidence localToPeer,
      DirectionEvidence peerToLocal) {
    Round {
      Objects.requireNonNull(localToPeer, "localToPeer");
      Objects.requireNonNull(peerToLocal, "peerToLocal");
      if (measuredAtElapsedRealtimeNanos < 0) {
        throw new IllegalArgumentException("network-health measurement time cannot be negative");
      }
    }
  }

  record Snapshot(
      PeerTarget peer,
      State state,
      State rawState,
      boolean measured,
      boolean stale,
      boolean transitionPending,
      long ageNanos,
      Round latestRound,
      List<String> issues) {
    Snapshot {
      Objects.requireNonNull(state, "state");
      Objects.requireNonNull(rawState, "rawState");
      Objects.requireNonNull(issues, "issues");
      if (ageNanos < 0) {
        throw new IllegalArgumentException("network-health evidence age cannot be negative");
      }
      issues = List.copyOf(issues);
      if (measured != (latestRound != null) || stale && !measured) {
        throw new IllegalArgumentException("network-health snapshot fields disagree");
      }
    }

    boolean configured() {
      return peer != null;
    }
  }

  private final Config config;
  private PeerTarget peer;
  private Round latestRound;
  private State stableState = State.UNUSABLE;
  private State latestRawState = State.UNUSABLE;
  private int consecutiveDegradedRounds;
  private int consecutiveUsableRounds;
  private int consecutiveGoodRounds;

  PairNetworkHealthPolicy(Config config) {
    this.config = Objects.requireNonNull(config, "config");
  }

  synchronized void configurePeer(PeerTarget requestedPeer) {
    Objects.requireNonNull(requestedPeer, "requestedPeer");
    if (!requestedPeer.equals(peer)) {
      peer = requestedPeer;
      resetEvidence();
    }
  }

  synchronized void clearPeer() {
    peer = null;
    resetEvidence();
  }

  /** Records current-peer evidence, returning false for a completion superseded by reconfiguration. */
  synchronized boolean record(PeerTarget measuredPeer, Round round) {
    Objects.requireNonNull(measuredPeer, "measuredPeer");
    Objects.requireNonNull(round, "round");
    if (!measuredPeer.equals(peer)) {
      return false;
    }
    if (latestRound != null
        && round.measuredAtElapsedRealtimeNanos()
            <= latestRound.measuredAtElapsedRealtimeNanos()) {
      throw new IllegalArgumentException("network-health rounds must be strictly time ordered");
    }

    boolean expiredBeforeRound =
        latestRound != null
            && round.measuredAtElapsedRealtimeNanos()
                    - latestRound.measuredAtElapsedRealtimeNanos()
                > config.maximumSampleAgeNanos();
    if (expiredBeforeRound) {
      stableState = State.UNUSABLE;
      resetStreaks();
    }

    State rawState = classify(round);
    applyHysteresis(rawState);
    latestRawState = rawState;
    latestRound = round;
    return true;
  }

  synchronized Snapshot snapshot(long nowElapsedRealtimeNanos) {
    if (nowElapsedRealtimeNanos < 0) {
      throw new IllegalArgumentException("snapshot time cannot be negative");
    }
    if (latestRound == null) {
      return new Snapshot(
          peer,
          State.UNUSABLE,
          State.UNUSABLE,
          false,
          false,
          false,
          0,
          null,
          List.of(
              peer == null
                  ? "A peer is not configured for network-health measurement."
                  : "Pair network health has not been measured."));
    }
    if (nowElapsedRealtimeNanos < latestRound.measuredAtElapsedRealtimeNanos()) {
      throw new IllegalArgumentException("snapshot time precedes the latest network-health round");
    }
    long age = nowElapsedRealtimeNanos - latestRound.measuredAtElapsedRealtimeNanos();
    boolean stale = age > config.maximumSampleAgeNanos();
    State visibleState = stale ? State.UNUSABLE : stableState;
    State visibleRawState = stale ? State.UNUSABLE : latestRawState;
    return new Snapshot(
        peer,
        visibleState,
        visibleRawState,
        true,
        stale,
        !stale && visibleState != visibleRawState,
        age,
        latestRound,
        issues(visibleState, visibleRawState, stale));
  }

  private State classify(Round round) {
    if (round.localToPeer().successes() == 0 || round.peerToLocal().successes() == 0) {
      return State.UNUSABLE;
    }
    return directionGood(round.localToPeer()) && directionGood(round.peerToLocal())
        ? State.GOOD
        : State.DEGRADED;
  }

  private boolean directionGood(DirectionEvidence direction) {
    return direction.successes() == direction.attempts()
        && direction.timeouts() == 0
        && direction.transferComplete()
        && !direction.transferSlow(config.minimumTransferBitsPerSecond());
  }

  private void applyHysteresis(State rawState) {
    if (rawState == State.UNUSABLE) {
      stableState = State.UNUSABLE;
      resetStreaks();
      return;
    }

    ++consecutiveUsableRounds;
    if (rawState == State.GOOD) {
      ++consecutiveGoodRounds;
      consecutiveDegradedRounds = 0;
    } else {
      consecutiveGoodRounds = 0;
      ++consecutiveDegradedRounds;
    }

    if (stableState == State.UNUSABLE
        && consecutiveUsableRounds >= config.usableRoundsToRecover()) {
      stableState = State.DEGRADED;
    }
    if (stableState == State.DEGRADED
        && rawState == State.GOOD
        && consecutiveGoodRounds >= config.goodRoundsToRecover()) {
      stableState = State.GOOD;
    } else if (stableState == State.GOOD
        && rawState == State.DEGRADED
        && consecutiveDegradedRounds >= config.degradedRoundsToWorsen()) {
      stableState = State.DEGRADED;
    }
  }

  private List<String> issues(State visibleState, State rawState, boolean stale) {
    if (stale) {
      return List.of("Pair network-health evidence is stale.");
    }
    if (visibleState == State.UNUSABLE) {
      if (rawState == State.UNUSABLE) {
        return List.of("At least one phone-to-phone application direction is unreachable.");
      }
      return List.of("Pair network health is recovering from an unusable state.");
    }
    if (visibleState == State.GOOD && rawState == State.DEGRADED) {
      return List.of("One degraded network-health round is awaiting hysteresis confirmation.");
    }
    if (visibleState == State.DEGRADED) {
      return degradedIssues(latestRound);
    }
    return List.of();
  }

  private List<String> degradedIssues(Round round) {
    ArrayList<String> issues = new ArrayList<>();
    if (hasPartialRequests(round.localToPeer()) || hasPartialRequests(round.peerToLocal())) {
      issues.add("Some phone-to-phone application requests failed or timed out.");
    }
    if (!round.localToPeer().transferComplete() || !round.peerToLocal().transferComplete()) {
      issues.add("A representative phone-to-phone transfer did not complete.");
    }
    if (round.localToPeer().transferSlow(config.minimumTransferBitsPerSecond())
        || round.peerToLocal().transferSlow(config.minimumTransferBitsPerSecond())) {
      issues.add("A representative phone-to-phone transfer was slower than the configured floor.");
    }
    if (issues.isEmpty()) {
      issues.add("Pair network health is recovering before returning to good.");
    }
    return List.copyOf(issues);
  }

  private static boolean hasPartialRequests(DirectionEvidence direction) {
    return direction.successes() < direction.attempts() || direction.timeouts() > 0;
  }

  private void resetEvidence() {
    latestRound = null;
    latestRawState = State.UNUSABLE;
    stableState = State.UNUSABLE;
    resetStreaks();
  }

  private void resetStreaks() {
    consecutiveDegradedRounds = 0;
    consecutiveUsableRounds = 0;
    consecutiveGoodRounds = 0;
  }
}
