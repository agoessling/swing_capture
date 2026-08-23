package com.agoessling.swingcapture;

public final class AutomaticTriggerReadinessGateTest {
  private AutomaticTriggerReadinessGateTest() {}

  public static void main(String[] arguments) {
    localReadinessCannotRaceAheadOfThePeer();
    peerResolutionCanRaceAheadOfLocalReadiness();
    unpairedAndDisabledCapturesPreserveTheirPolicies();
  }

  private static void localReadinessCannotRaceAheadOfThePeer() {
    AutomaticTriggerReadinessGate gate = new AutomaticTriggerReadinessGate(true, true);

    check(!gate.markLocalReady(), "local pre-roll alone must not enable paired audio triggers");
    check(!gate.enabled(), "paired gate remains closed while the peer is transitioning");
    check(
        !gate.captureReady(),
        "active-evidence timeout remains deferred while the peer is transitioning");
    check(
        gate.resolvePeerArm(AutomaticTriggerReadinessGate.PeerArmResolution.READY),
        "peer arm resolution releases a locally ready capture");
    check(gate.captureReady(), "joint readiness allows the active-evidence timeout");
    check(gate.activeEvidenceTimeoutAllowed(), "confirmed readiness allows the timeout");
  }

  private static void peerResolutionCanRaceAheadOfLocalReadiness() {
    AutomaticTriggerReadinessGate gate = new AutomaticTriggerReadinessGate(true, true);

    check(
        !gate.resolvePeerArm(AutomaticTriggerReadinessGate.PeerArmResolution.READY),
        "peer readiness alone cannot precede local pre-roll");
    check(!gate.captureReady(), "peer readiness cannot race ahead of local capture readiness");
    check(gate.markLocalReady(), "local readiness completes the two-phone barrier");
    check(gate.captureReady(), "the barrier is independent of callback ordering");
    check(
        gate.resolvePeerArm(AutomaticTriggerReadinessGate.PeerArmResolution.UNCONFIRMED),
        "a reordered duplicate resolution keeps triggers enabled");
    check(
        gate.activeEvidenceTimeoutAllowed(),
        "a reordered duplicate cannot revoke confirmed peer readiness");
  }

  private static void unpairedAndDisabledCapturesPreserveTheirPolicies() {
    AutomaticTriggerReadinessGate unpaired = new AutomaticTriggerReadinessGate(true, false);
    check(unpaired.markLocalReady(), "unpaired capture enables at local readiness");

    AutomaticTriggerReadinessGate disabled = new AutomaticTriggerReadinessGate(false, false);
    check(!disabled.markLocalReady(), "operator-only capture keeps automatic triggers disabled");
    check(disabled.captureReady(), "operator-only local readiness still permits its timeout");
    check(
        disabled.activeEvidenceTimeoutAllowed(),
        "operator-only local readiness permits active-evidence timeout");
    check(
        !disabled.resolvePeerArm(AutomaticTriggerReadinessGate.PeerArmResolution.READY),
        "peer resolution cannot enable an operator-only capture");

    AutomaticTriggerReadinessGate unconfirmed =
        new AutomaticTriggerReadinessGate(true, true);
    check(!unconfirmed.markLocalReady(), "unconfirmed paired capture starts gated");
    check(
        unconfirmed.resolvePeerArm(
            AutomaticTriggerReadinessGate.PeerArmResolution.UNCONFIRMED),
        "bounded unconfirmed outcome preserves degraded local impact capture");
    check(
        !unconfirmed.activeEvidenceTimeoutAllowed(),
        "no-impact publication stays deferred when the shadow may still be transitioning");

    AutomaticTriggerReadinessGate failed = new AutomaticTriggerReadinessGate(true, true);
    check(!failed.markLocalReady(), "failed paired capture starts gated");
    check(
        failed.resolvePeerArm(AutomaticTriggerReadinessGate.PeerArmResolution.FAILED),
        "terminal peer rejection releases degraded local impact capture");
    check(
        failed.activeEvidenceTimeoutAllowed(),
        "terminal rejection permits bounded local-only no-impact evidence");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
