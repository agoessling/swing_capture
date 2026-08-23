package com.agoessling.swingcapture.core.coordination;

import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle.Action;
import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle.ImmutableStoreOutcome;
import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle.LastOutcome;
import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle.State;
import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle.Transition;

/** Deterministic fault-injection coverage for the leader-owned autonomous pair lifecycle. */
public final class AutonomousPairLifecycleTest {
  private static final long DEADLINE = 10_000;

  private AutonomousPairLifecycleTest() {}

  public static void main(String[] arguments) {
    completesNominalPairAndRearmsWithoutBrowser();
    recoversLostStartRequestAndResponseIdempotently();
    retainsLocalEvidenceWhenPeerUnavailable();
    rejectsPeerRestartSplitSession();
    staleClockRetainsBothClipsButDoesNotPair();
    conflictingReplicaFailsClosed();
    partialPublicationBecomesReviewableLocalEvidence();
    pairAdmissionWaitsForBothPublishedClipsInEitherOrder();
    lateAndDuplicateArmResponsesAreIdempotent();
    lostArmAcknowledgementRecoversFromSameSessionEvidence();
    peerTransportLossAfterArmPreservesTheActiveSession();
    unavailableReplicationRetainsAuthoritativeLocalRecord();
    duplicateLocalClipFailsClosed();
    stationStopOwnsPeerStop();
    restartDuringArmKeepsSharedSessionAndPollsExistingEvidence();
    restartDuringCaptureNeverSynthesizesAnotherSession();
    restartDuringPublicationRetainsPublishedLocalClip();
    restartDuringDurableRecordStorageIsIdempotent();
    missingDurableRecordFailsClosed();
    recoveredBacklogConflictFailsClosed();
  }

  private static void completesNominalPairAndRearmsWithoutBrowser() {
    AutonomousPairLifecycle lifecycle = started();
    require(lifecycle.claimSwing("swing-1", 0), Action.ARM_PEER_FOR_SWING);
    lifecycle.peerArmAccepted("swing-1");
    Transition trigger = lifecycle.localTriggered("swing-1", "local-1");
    require(trigger, Action.SEND_PEER_IMPACT);
    require(trigger, Action.POLL_PEER_TRIGGER);
    lifecycle.peerTriggered("swing-1", "peer-1", true);
    lifecycle.localPublished("swing-1", "local-1");
    require(lifecycle.peerPublished("swing-1", "peer-1"), Action.ADMIT_PAIR);
    require(lifecycle.pairAdmitted("swing-1"), Action.STORE_LOCAL_RECORD);
    require(
        lifecycle.localRecordStored("swing-1", ImmutableStoreOutcome.STORED),
        Action.REPLICATE_RECORD_TO_PEER);
    Transition replicated =
        lifecycle.peerRecordStored("swing-1", ImmutableStoreOutcome.ALREADY_PRESENT);
    require(replicated, Action.REARM_LOCAL);
    require(replicated, Action.REARM_PEER);
    lifecycle.rearmed(true);
    check(lifecycle.snapshot().state() == State.MONITORING, "nominal monitoring rearm");
    check(lifecycle.snapshot().lastOutcome() == LastOutcome.PAIRED, "nominal paired outcome");
  }

  private static void recoversLostStartRequestAndResponseIdempotently() {
    AutonomousPairLifecycle lifecycle = lifecycle();
    require(lifecycle.startStation(), Action.START_PEER_STANDBY);
    // A lost request is represented by no acknowledgement. The leader retries the same action.
    lifecycle.peerUnavailable("request timeout");
    require(lifecycle.retryPeer(), Action.START_PEER_STANDBY);
    lifecycle.peerStandbyStarted();
    // A lost response followed by an idempotent retry can produce a duplicate acknowledgement.
    lifecycle.peerStandbyStarted();
    check(lifecycle.snapshot().state() == State.MONITORING, "duplicate start acknowledgement");
  }

  private static void retainsLocalEvidenceWhenPeerUnavailable() {
    AutonomousPairLifecycle lifecycle = lifecycle();
    lifecycle.startStation();
    lifecycle.peerUnavailable("Wi-Fi down");
    require(lifecycle.claimSwing("swing-local", 0), Action.ARM_PEER_FOR_SWING);
    lifecycle.peerArmFailed("swing-local", "Wi-Fi remains down");
    lifecycle.localTriggered("swing-local", "local-only");
    Transition ready = lifecycle.localPublished("swing-local", "local-only");
    require(ready, Action.REARM_LOCAL);
    check(!ready.requests(Action.REARM_PEER), "offline peer is not treated as rearmed");
    lifecycle.rearmed(false);
    check(lifecycle.snapshot().state() == State.DEGRADED_MONITORING, "degraded monitoring");
    check(lifecycle.snapshot().lastOutcome() == LastOutcome.LOCAL_ONLY, "local evidence retained");
  }

  private static void rejectsPeerRestartSplitSession() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-before-restart", 0);
    lifecycle.peerArmAccepted("swing-before-restart");
    lifecycle.localTriggered("swing-before-restart", "local-before-restart");
    lifecycle.peerUnavailable("peer app restarted");
    lifecycle.peerTriggered("different-session", "peer-after-restart", true);
    check(lifecycle.snapshot().state() == State.TERMINAL_FAILURE, "split session fails closed");
  }

  private static void staleClockRetainsBothClipsButDoesNotPair() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-stale-clock", 0);
    lifecycle.peerArmAccepted("swing-stale-clock");
    lifecycle.localTriggered("swing-stale-clock", "local-clock");
    lifecycle.peerTriggered("swing-stale-clock", "peer-clock", false);
    lifecycle.localPublished("swing-stale-clock", "local-clock");
    Transition ready = lifecycle.peerPublished("swing-stale-clock", "peer-clock");
    check(!ready.requests(Action.ADMIT_PAIR), "stale clock cannot admit a pair");
    require(ready, Action.REARM_LOCAL);
    check(lifecycle.snapshot().lastOutcome() == LastOutcome.LOCAL_ONLY, "clips retained unpaired");
  }

  private static void conflictingReplicaFailsClosed() {
    AutonomousPairLifecycle lifecycle = readyToReplicate("swing-conflict");
    lifecycle.peerRecordStored("swing-conflict", ImmutableStoreOutcome.CONFLICT);
    check(lifecycle.snapshot().state() == State.TERMINAL_FAILURE, "replica conflict terminal");
    check(lifecycle.snapshot().lastOutcome() == LastOutcome.CONFLICT, "conflict audited");
  }

  private static void partialPublicationBecomesReviewableLocalEvidence() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-partial", 0);
    lifecycle.peerArmAccepted("swing-partial");
    lifecycle.localTriggered("swing-partial", "local-partial");
    lifecycle.peerTriggered("swing-partial", "peer-partial", true);
    lifecycle.localPublished("swing-partial", "local-partial");
    Transition expired = lifecycle.tick(DEADLINE);
    require(expired, Action.REARM_LOCAL);
    check(lifecycle.snapshot().lastOutcome() == LastOutcome.LOCAL_ONLY, "partial local outcome");
  }

  private static void pairAdmissionWaitsForBothPublishedClipsInEitherOrder() {
    AutonomousPairLifecycle localFirst = started();
    localFirst.claimSwing("swing-local-first", 0);
    localFirst.peerArmAccepted("swing-local-first");
    localFirst.localTriggered("swing-local-first", "local-first");
    localFirst.peerTriggered("swing-local-first", "peer-second", true);
    Transition onlyLocal = localFirst.localPublished("swing-local-first", "local-first");
    check(!onlyLocal.requests(Action.ADMIT_PAIR), "local manifest alone cannot admit a pair");
    check(
        onlyLocal.snapshot().state() == State.WAITING_EVIDENCE,
        "local-first publication waits for peer clip");
    require(localFirst.peerPublished("swing-local-first", "peer-second"), Action.ADMIT_PAIR);

    AutonomousPairLifecycle peerFirst = started();
    peerFirst.claimSwing("swing-peer-first", 0);
    peerFirst.peerArmAccepted("swing-peer-first");
    peerFirst.localTriggered("swing-peer-first", "local-second");
    peerFirst.peerTriggered("swing-peer-first", "peer-first", true);
    Transition onlyPeer = peerFirst.peerPublished("swing-peer-first", "peer-first");
    check(!onlyPeer.requests(Action.ADMIT_PAIR), "peer manifest alone cannot admit a pair");
    check(
        onlyPeer.snapshot().state() == State.WAITING_EVIDENCE,
        "peer-first publication waits for local clip");
    require(peerFirst.localPublished("swing-peer-first", "local-second"), Action.ADMIT_PAIR);
  }

  private static void duplicateLocalClipFailsClosed() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-duplicate", 0);
    lifecycle.localTriggered("swing-duplicate", "local-a");
    lifecycle.localTriggered("swing-duplicate", "local-b");
    check(lifecycle.snapshot().state() == State.TERMINAL_FAILURE, "duplicate local terminal");
  }

  private static void lateAndDuplicateArmResponsesAreIdempotent() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-late-arm", 0);
    lifecycle.localTriggered("swing-late-arm", "local-late-arm");
    lifecycle.peerArmAccepted("swing-late-arm");
    lifecycle.peerArmAccepted("swing-late-arm");
    check(
        lifecycle.snapshot().state() == State.WAITING_EVIDENCE,
        "lost arm response retry stays in active session");
    check(lifecycle.snapshot().peerArmed(), "late arm acknowledgement is retained");
  }

  private static void unavailableReplicationRetainsAuthoritativeLocalRecord() {
    AutonomousPairLifecycle lifecycle = readyToReplicate("swing-local-record");
    Transition degraded =
        lifecycle.peerRecordStored("swing-local-record", ImmutableStoreOutcome.UNAVAILABLE);
    require(degraded, Action.REARM_LOCAL);
    check(
        lifecycle.snapshot().lastOutcome() == LastOutcome.LOCAL_ONLY,
        "authoritative local record remains reviewable");
  }

  private static void lostArmAcknowledgementRecoversFromSameSessionEvidence() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-lost-arm-response", 0);
    Transition trigger =
        lifecycle.localTriggered("swing-lost-arm-response", "local-lost-arm-response");
    require(trigger, Action.SEND_PEER_IMPACT);
    require(trigger, Action.POLL_PEER_TRIGGER);
    lifecycle.localPublished("swing-lost-arm-response", "local-lost-arm-response");
    check(
        lifecycle.snapshot().state() == State.WAITING_EVIDENCE,
        "local publication waits while the arm request is pending");
    lifecycle.peerArmUnconfirmed("swing-lost-arm-response", "response timeout");
    lifecycle.peerTriggered("swing-lost-arm-response", "peer-lost-arm-response", true);
    require(
        lifecycle.peerPublished("swing-lost-arm-response", "peer-lost-arm-response"),
        Action.ADMIT_PAIR);
    check(
        lifecycle.snapshot().state() == State.ADMITTING_PAIR,
        "same-session evidence proves an unconfirmed arm succeeded");
  }

  private static void peerTransportLossAfterArmPreservesTheActiveSession() {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing("swing-midflight-wifi", 0);
    lifecycle.peerArmAccepted("swing-midflight-wifi");
    lifecycle.peerUnavailable("clock poll timeout");
    Transition trigger =
        lifecycle.localTriggered("swing-midflight-wifi", "local-midflight-wifi");
    require(trigger, Action.SEND_PEER_IMPACT);
    lifecycle.peerTriggered("swing-midflight-wifi", "peer-midflight-wifi", true);
    check(
        lifecycle.snapshot().state() == State.WAITING_EVIDENCE,
        "transport loss does not revoke an already accepted arm");
  }

  private static void stationStopOwnsPeerStop() {
    AutonomousPairLifecycle lifecycle = started();
    require(lifecycle.stopStation(), Action.STOP_PEER_STANDBY);
    lifecycle.stopped();
    check(lifecycle.snapshot().state() == State.STOPPED, "station stopped");
  }

  private static void restartDuringArmKeepsSharedSessionAndPollsExistingEvidence() {
    AutonomousPairLifecycle before = started();
    before.claimSwing("swing-restart-arm", 100);
    AutonomousPairLifecycle recovered = restore(before);
    Transition transition = recovered.recover(false, 200);
    check(
        transition.snapshot().activeSessionId().equals("swing-restart-arm"),
        "arm restart keeps shared session");
    check(
        transition.snapshot().state() == State.WAITING_EVIDENCE,
        "arm restart enters bounded evidence recovery");
    require(transition, Action.POLL_PEER_TRIGGER);
    require(transition, Action.POLL_PEER_PUBLICATION);
    Transition repeated = recovered.recover(false, 300);
    check(
        repeated.snapshot().activeSessionId().equals("swing-restart-arm"),
        "repeated recovery remains idempotent");
  }

  private static void restartDuringCaptureNeverSynthesizesAnotherSession() {
    AutonomousPairLifecycle before = started();
    before.claimSwing("swing-restart-capture", 100);
    before.peerArmAccepted("swing-restart-capture");
    AutonomousPairLifecycle recovered = restore(before);
    recovered.recover(false, 200);
    Transition rejected = recovered.claimSwing("different-swing", 300);
    check(
        rejected.snapshot().state() == State.TERMINAL_FAILURE,
        "recovery rejects a replacement shared session");
    check(
        rejected.snapshot().lastCompletedSessionId().isEmpty(),
        "replacement session was never accepted");
  }

  private static void restartDuringPublicationRetainsPublishedLocalClip() {
    AutonomousPairLifecycle before = started();
    before.claimSwing("swing-restart-publication", 100);
    before.peerArmAccepted("swing-restart-publication");
    before.localTriggered("swing-restart-publication", "local-publication");
    before.localPublished("swing-restart-publication", "local-publication");
    AutonomousPairLifecycle recovered = restore(before);
    Transition transition = recovered.recover(false, 200);
    check(transition.snapshot().localPublished(), "published local evidence survives restart");
    recovered.tick(DEADLINE + 100);
    check(
        recovered.snapshot().lastOutcome() == LastOutcome.LOCAL_ONLY,
        "deadline retains the existing local clip");
  }

  private static void restartDuringDurableRecordStorageIsIdempotent() {
    AutonomousPairLifecycle before = started();
    before.claimSwing("swing-restart-store", 100);
    before.peerArmAccepted("swing-restart-store");
    before.localTriggered("swing-restart-store", "local-store");
    before.peerTriggered("swing-restart-store", "peer-store", true);
    before.localPublished("swing-restart-store", "local-store");
    before.peerPublished("swing-restart-store", "peer-store");
    before.pairAdmitted("swing-restart-store");
    AutonomousPairLifecycle recovered = restore(before);
    Transition first = recovered.recover(true, 200);
    require(first, Action.STORE_LOCAL_RECORD);
    Transition second = recovered.recover(true, 300);
    require(second, Action.STORE_LOCAL_RECORD);
    check(
        second.snapshot().activeSessionId().equals("swing-restart-store"),
        "record recovery keeps the admitted shared session");
  }

  private static void missingDurableRecordFailsClosed() {
    AutonomousPairLifecycle before = started();
    before.claimSwing("swing-missing-record", 100);
    before.peerArmAccepted("swing-missing-record");
    before.localTriggered("swing-missing-record", "local-missing");
    before.peerTriggered("swing-missing-record", "peer-missing", true);
    before.localPublished("swing-missing-record", "local-missing");
    before.peerPublished("swing-missing-record", "peer-missing");
    before.pairAdmitted("swing-missing-record");
    AutonomousPairLifecycle recovered = restore(before);
    recovered.recover(false, 200);
    check(
        recovered.snapshot().state() == State.TERMINAL_FAILURE,
        "missing admitted record fails closed");
  }

  private static void recoveredBacklogConflictFailsClosed() {
    AutonomousPairLifecycle recovered = started();
    recovered.backlogReplicationConflict("swing-old-conflict");
    check(recovered.snapshot().state() == State.TERMINAL_FAILURE, "backlog conflict terminal");
    check(
        recovered.snapshot().lastCompletedSessionId().equals("swing-old-conflict"),
        "backlog conflict identifies immutable record");
  }

  private static AutonomousPairLifecycle restore(AutonomousPairLifecycle before) {
    return AutonomousPairLifecycle.restore(
        new AutonomousPairLifecycle.Config(DEADLINE), before.checkpoint());
  }

  private static AutonomousPairLifecycle readyToReplicate(String sessionId) {
    AutonomousPairLifecycle lifecycle = started();
    lifecycle.claimSwing(sessionId, 0);
    lifecycle.peerArmAccepted(sessionId);
    lifecycle.localTriggered(sessionId, "local");
    lifecycle.peerTriggered(sessionId, "peer", true);
    lifecycle.localPublished(sessionId, "local");
    lifecycle.peerPublished(sessionId, "peer");
    lifecycle.pairAdmitted(sessionId);
    lifecycle.localRecordStored(sessionId, ImmutableStoreOutcome.STORED);
    return lifecycle;
  }

  private static AutonomousPairLifecycle started() {
    AutonomousPairLifecycle lifecycle = lifecycle();
    lifecycle.startStation();
    lifecycle.peerStandbyStarted();
    return lifecycle;
  }

  private static AutonomousPairLifecycle lifecycle() {
    return new AutonomousPairLifecycle(new AutonomousPairLifecycle.Config(DEADLINE));
  }

  private static void require(Transition transition, Action action) {
    check(transition.requests(action), "expected action " + action + " in " + transition.actions());
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
