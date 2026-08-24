package com.agoessling.swingcapture;

import java.util.List;
import java.util.concurrent.TimeUnit;

public final class PairNetworkHealthPolicyTest {
  private static final long MAXIMUM_AGE_NS = TimeUnit.SECONDS.toNanos(20);
  private static final long TRANSFER_FLOOR_BPS = 12_000_000;
  private static final PairNetworkHealthPolicy.Config CONFIG =
      new PairNetworkHealthPolicy.Config(MAXIMUM_AGE_NS, TRANSFER_FLOOR_BPS, 2, 2, 3);
  private static final PairNetworkHealthPolicy.PeerTarget PEER_A =
      new PairNetworkHealthPolicy.PeerTarget("http://192.168.1.20:8088", "peer-a");
  private static final PairNetworkHealthPolicy.PeerTarget PEER_B =
      new PairNetworkHealthPolicy.PeerTarget("http://192.168.1.21:8088", "peer-a");

  private PairNetworkHealthPolicyTest() {}

  public static void main(String[] arguments) {
    unknownAndStaleEvidenceFailUnusable();
    nominalRoundIsGoodAndRetainsMetrics();
    partialLossAndSlowTransferDegradeWithHysteresis();
    incompleteTransferIsDegraded();
    oneUnreachableDirectionFailsImmediately();
    recoveryFromTransientDisconnectIsHysteretic();
    originChangeWithdrawsPriorEvidence();
    lateOldPeerRoundCannotRestorePreviousTarget();
    rejectsInvalidOrOutOfOrderEvidence();
  }

  private static void unknownAndStaleEvidenceFailUnusable() {
    PairNetworkHealthPolicy policy = policy();
    policy.configurePeer(PEER_A);
    PairNetworkHealthPolicy.Snapshot unknown = policy.snapshot(1_000);
    check(unknown.configured(), "configured peer retained while unknown");
    check(!unknown.measured(), "unknown status has no invented measurement");
    check(unknown.state() == PairNetworkHealthPolicy.State.UNUSABLE, "unknown fails unusable");

    policy.record(PEER_A, goodRound(2_000));
    check(
        policy.snapshot(2_000).state() == PairNetworkHealthPolicy.State.UNUSABLE,
        "one initial good round remains fail-closed");
    policy.record(PEER_A, goodRound(3_000));
    check(
        policy.snapshot(3_000).state() == PairNetworkHealthPolicy.State.DEGRADED,
        "two initial good rounds recover only to degraded");
    policy.record(PEER_A, goodRound(4_000));
    PairNetworkHealthPolicy.Snapshot boundary = policy.snapshot(4_000 + MAXIMUM_AGE_NS);
    check(boundary.state() == PairNetworkHealthPolicy.State.GOOD, "freshness boundary inclusive");
    PairNetworkHealthPolicy.Snapshot stale = policy.snapshot(4_001 + MAXIMUM_AGE_NS);
    check(stale.stale(), "expired evidence marked stale");
    check(stale.state() == PairNetworkHealthPolicy.State.UNUSABLE, "stale evidence fails unusable");
    check(stale.latestRound() != null, "stale metrics remain diagnostic");
  }

  private static void nominalRoundIsGoodAndRetainsMetrics() {
    PairNetworkHealthPolicy policy = policy();
    policy.configurePeer(PEER_A);
    policy.record(PEER_A, goodRound(9_998));
    policy.record(PEER_A, goodRound(9_999));
    policy.record(PEER_A, goodRound(10_000));
    PairNetworkHealthPolicy.Snapshot snapshot = policy.snapshot(10_500);
    check(snapshot.state() == PairNetworkHealthPolicy.State.GOOD, "three complete rounds recover good");
    check(snapshot.rawState() == PairNetworkHealthPolicy.State.GOOD, "raw state good");
    check(snapshot.issues().isEmpty(), "good pair has no issue");

    PairNetworkHealthPolicy.DirectionEvidence direction = snapshot.latestRound().localToPeer();
    check(direction.minimumRoundTripNanos() == 10, "minimum RTT");
    check(direction.medianRoundTripNanos() == 20, "nearest-rank median RTT");
    check(direction.p95RoundTripNanos() == 40, "nearest-rank p95 RTT");
    check(direction.maximumRoundTripNanos() == 40, "maximum RTT");
    check(direction.jitterNanos() == 30, "RTT range retained as jitter");
    check(direction.transferBitsPerSecond() == 16_000_000.0, "transfer rate retained");
  }

  private static void partialLossAndSlowTransferDegradeWithHysteresis() {
    PairNetworkHealthPolicy policy = readyPolicy(1_000);
    policy.record(PEER_A, partialLossRound(2_000));
    PairNetworkHealthPolicy.Snapshot pending = policy.snapshot(2_000);
    check(pending.state() == PairNetworkHealthPolicy.State.GOOD, "one degraded round does not flap");
    check(pending.rawState() == PairNetworkHealthPolicy.State.DEGRADED, "partial loss is raw degraded");
    check(pending.transitionPending(), "pending degradation is explicit");

    policy.record(PEER_A, slowTransferRound(3_000));
    PairNetworkHealthPolicy.Snapshot degraded = policy.snapshot(3_000);
    check(degraded.state() == PairNetworkHealthPolicy.State.DEGRADED, "second degraded round worsens");
    check(
        degraded.issues().stream().anyMatch(issue -> issue.contains("slower")),
        "slow transfer diagnostic retained");

    policy.record(PEER_A, goodRound(4_000));
    policy.record(PEER_A, goodRound(5_000));
    check(
        policy.snapshot(5_000).state() == PairNetworkHealthPolicy.State.DEGRADED,
        "two good rounds do not prematurely recover");
    policy.record(PEER_A, goodRound(6_000));
    check(
        policy.snapshot(6_000).state() == PairNetworkHealthPolicy.State.GOOD,
        "third good round recovers");
  }

  private static void incompleteTransferIsDegraded() {
    PairNetworkHealthPolicy policy = policy();
    policy.configurePeer(PEER_A);
    policy.record(PEER_A, incompleteTransferRound(1_000));
    policy.record(PEER_A, incompleteTransferRound(2_000));
    PairNetworkHealthPolicy.Snapshot snapshot = policy.snapshot(2_000);
    check(snapshot.state() == PairNetworkHealthPolicy.State.DEGRADED, "incomplete transfer degraded");
    check(
        snapshot.issues().stream().anyMatch(issue -> issue.contains("did not complete")),
        "incomplete transfer diagnostic");
  }

  private static void oneUnreachableDirectionFailsImmediately() {
    PairNetworkHealthPolicy policy = readyPolicy(1_000);
    policy.record(
        PEER_A,
        new PairNetworkHealthPolicy.Round(
            2_000, goodDirection(), unreachableDirection()));
    PairNetworkHealthPolicy.Snapshot snapshot = policy.snapshot(2_000);
    check(snapshot.state() == PairNetworkHealthPolicy.State.UNUSABLE, "one zero-success edge fails");
    check(!snapshot.transitionPending(), "unusable is not delayed by hysteresis");
    check(
        snapshot.issues().stream().anyMatch(issue -> issue.contains("unreachable")),
        "unreachable diagnostic");
  }

  private static void recoveryFromTransientDisconnectIsHysteretic() {
    PairNetworkHealthPolicy policy = readyPolicy(1_000);
    policy.record(
        PEER_A,
        new PairNetworkHealthPolicy.Round(2_000, unreachableDirection(), goodDirection()));
    policy.record(PEER_A, goodRound(3_000));
    check(
        policy.snapshot(3_000).state() == PairNetworkHealthPolicy.State.UNUSABLE,
        "one good round cannot clear unusable");
    policy.record(PEER_A, goodRound(4_000));
    check(
        policy.snapshot(4_000).state() == PairNetworkHealthPolicy.State.DEGRADED,
        "second usable round reaches degraded");
    policy.record(PEER_A, goodRound(5_000));
    check(
        policy.snapshot(5_000).state() == PairNetworkHealthPolicy.State.GOOD,
        "third good round completes recovery");

    policy.record(PEER_A, goodRound(5_000 + MAXIMUM_AGE_NS + 1));
    check(
        policy.snapshot(5_000 + MAXIMUM_AGE_NS + 1).state()
            == PairNetworkHealthPolicy.State.UNUSABLE,
        "a gap beyond freshness enters hysteretic recovery");
  }

  private static void originChangeWithdrawsPriorEvidence() {
    PairNetworkHealthPolicy policy = readyPolicy(1_000);
    policy.configurePeer(PEER_B);
    PairNetworkHealthPolicy.Snapshot reset = policy.snapshot(2_000);
    check(reset.peer().equals(PEER_B), "replacement origin retained");
    check(!reset.measured(), "old-origin evidence withdrawn");
    check(reset.state() == PairNetworkHealthPolicy.State.UNUSABLE, "changed origin fails unknown");

    policy.record(PEER_B, goodRound(3_000));
    policy.record(PEER_B, goodRound(4_000));
    policy.record(PEER_B, goodRound(5_000));
    check(
        policy.snapshot(5_000).state() == PairNetworkHealthPolicy.State.GOOD,
        "new origin earns independent initial status");
    policy.clearPeer();
    PairNetworkHealthPolicy.Snapshot cleared = policy.snapshot(6_000);
    check(!cleared.configured(), "cleared peer is explicit");
    check(!cleared.measured(), "clearing peer removes measurements");
  }

  private static void rejectsInvalidOrOutOfOrderEvidence() {
    expectFailure(
        () -> new PairNetworkHealthPolicy.Config(0, TRANSFER_FLOOR_BPS, 2, 2, 3),
        "zero maximum age");
    expectFailure(
        () ->
            new PairNetworkHealthPolicy.DirectionEvidence(
                3, 2, 2, List.of(1L, 2L), 1, 1, true),
        "attempt accounting");
    expectFailure(
        () ->
            new PairNetworkHealthPolicy.DirectionEvidence(
                1, 1, 0, List.of(-1L), 1, 1, true),
        "negative RTT");
    PairNetworkHealthPolicy policy = readyPolicy(2_000);
    expectFailure(() -> policy.record(PEER_A, goodRound(2_000)), "duplicate round time");
    expectFailure(() -> policy.snapshot(1_999), "snapshot before evidence");

    PairNetworkHealthPolicy.DirectionEvidence huge =
        new PairNetworkHealthPolicy.DirectionEvidence(
            1, 1, 0, List.of(Long.MAX_VALUE), Long.MAX_VALUE, 1, true);
    check(Double.isFinite(huge.transferBitsPerSecond()), "transfer arithmetic cannot overflow");
  }

  private static void lateOldPeerRoundCannotRestorePreviousTarget() {
    PairNetworkHealthPolicy policy = readyPolicy(1_000);
    policy.configurePeer(PEER_B);

    check(
        !policy.record(PEER_A, goodRound(2_000)),
        "late old-peer completion is rejected without affecting the scheduler");
    PairNetworkHealthPolicy.Snapshot afterLateRound = policy.snapshot(2_000);
    check(afterLateRound.peer().equals(PEER_B), "late round cannot restore prior peer");
    check(!afterLateRound.measured(), "late round cannot restore prior evidence");

    policy.record(PEER_B, goodRound(3_000));
    policy.record(PEER_B, goodRound(4_000));
    policy.record(PEER_B, goodRound(5_000));
    check(
        policy.snapshot(5_000).state() == PairNetworkHealthPolicy.State.GOOD,
        "current peer round remains accepted");
  }

  private static PairNetworkHealthPolicy policy() {
    return new PairNetworkHealthPolicy(CONFIG);
  }

  private static PairNetworkHealthPolicy readyPolicy(long measuredAt) {
    PairNetworkHealthPolicy policy = policy();
    policy.configurePeer(PEER_A);
    policy.record(PEER_A, goodRound(measuredAt - 2));
    policy.record(PEER_A, goodRound(measuredAt - 1));
    policy.record(PEER_A, goodRound(measuredAt));
    return policy;
  }

  private static PairNetworkHealthPolicy.Round goodRound(long measuredAt) {
    return new PairNetworkHealthPolicy.Round(measuredAt, goodDirection(), goodDirection());
  }

  private static PairNetworkHealthPolicy.Round partialLossRound(long measuredAt) {
    PairNetworkHealthPolicy.DirectionEvidence partial =
        new PairNetworkHealthPolicy.DirectionEvidence(
            4, 3, 1, List.of(15L, 25L, 80L), 200_000, 100_000_000, true);
    return new PairNetworkHealthPolicy.Round(measuredAt, partial, goodDirection());
  }

  private static PairNetworkHealthPolicy.Round slowTransferRound(long measuredAt) {
    PairNetworkHealthPolicy.DirectionEvidence slow =
        new PairNetworkHealthPolicy.DirectionEvidence(
            4, 4, 0, List.of(10L, 20L, 30L, 40L), 100_000, 100_000_000, true);
    return new PairNetworkHealthPolicy.Round(measuredAt, goodDirection(), slow);
  }

  private static PairNetworkHealthPolicy.Round incompleteTransferRound(long measuredAt) {
    PairNetworkHealthPolicy.DirectionEvidence incomplete =
        new PairNetworkHealthPolicy.DirectionEvidence(
            4, 4, 0, List.of(10L, 20L, 30L, 40L), 50_000, 50_000_000, false);
    return new PairNetworkHealthPolicy.Round(measuredAt, goodDirection(), incomplete);
  }

  private static PairNetworkHealthPolicy.DirectionEvidence goodDirection() {
    return new PairNetworkHealthPolicy.DirectionEvidence(
        4, 4, 0, List.of(40L, 10L, 30L, 20L), 200_000, 100_000_000, true);
  }

  private static PairNetworkHealthPolicy.DirectionEvidence unreachableDirection() {
    return new PairNetworkHealthPolicy.DirectionEvidence(3, 0, 3, List.of(), 0, 0, false);
  }

  private static void expectFailure(Runnable action, String label) {
    try {
      action.run();
      throw new AssertionError(label + " did not fail");
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
