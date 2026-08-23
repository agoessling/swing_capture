package com.agoessling.swingcapture;

/** Coordinates local pre-roll readiness with an optional paired-phone readiness barrier. */
final class AutomaticTriggerReadinessGate {
  enum PeerArmResolution {
    READY,
    FAILED,
    UNCONFIRMED
  }

  private final boolean automaticTriggersRequested;
  private final boolean peerReadinessRequired;
  private boolean localReady;
  private boolean peerArmResolved;
  private boolean peerReadinessConfirmedOrImpossible;

  AutomaticTriggerReadinessGate(
      boolean automaticTriggersRequested, boolean peerReadinessRequired) {
    this.automaticTriggersRequested = automaticTriggersRequested;
    this.peerReadinessRequired = peerReadinessRequired;
    peerArmResolved = !peerReadinessRequired;
    peerReadinessConfirmedOrImpossible = !peerReadinessRequired;
  }

  synchronized boolean markLocalReady() {
    localReady = true;
    return enabled();
  }

  synchronized boolean resolvePeerArm(PeerArmResolution resolution) {
    if (resolution == null) {
      throw new IllegalArgumentException("peer arm resolution is required");
    }
    peerArmResolved = true;
    peerReadinessConfirmedOrImpossible |= resolution != PeerArmResolution.UNCONFIRMED;
    return enabled();
  }

  synchronized boolean enabled() {
    return automaticTriggersRequested && captureReady();
  }

  synchronized boolean captureReady() {
    return localReady && peerArmResolved;
  }

  synchronized boolean activeEvidenceTimeoutAllowed() {
    return localReady && peerReadinessConfirmedOrImpossible;
  }
}
