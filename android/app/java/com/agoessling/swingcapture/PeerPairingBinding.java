package com.agoessling.swingcapture;

import java.util.Objects;

/** Durable, non-secret identity binding for the leader's outbound peer credential. */
record PeerPairingBinding(
    State state,
    String peerNodeId,
    String expectedRole,
    String label,
    String origin,
    long credentialGeneration,
    long verifiedAtEpochMillis,
    long revokedAtEpochMillis) {
  enum State {
    ACTIVE,
    REVOKED
  }

  PeerPairingBinding {
    Objects.requireNonNull(state, "state");
    peerNodeId = requireText(peerNodeId, "peer node ID");
    expectedRole = requireText(expectedRole, "expected role");
    label = requireText(label, "peer label");
    origin = requireText(origin, "peer origin");
    PeerTransportSecurityPolicy.validateOrigin(
        origin, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED);
    if (credentialGeneration < 1) {
      throw new IllegalArgumentException("credential generation must be positive");
    }
    if (verifiedAtEpochMillis < 0 || revokedAtEpochMillis < 0) {
      throw new IllegalArgumentException("pairing timestamps cannot be negative");
    }
    if (state == State.ACTIVE && revokedAtEpochMillis != 0) {
      throw new IllegalArgumentException("active pairing cannot have a revocation timestamp");
    }
    if (state == State.REVOKED && revokedAtEpochMillis == 0) {
      throw new IllegalArgumentException("revoked pairing requires a revocation timestamp");
    }
  }

  static PeerPairingBinding verifyAndActivate(
      PeerPairingBinding current,
      String localNodeId,
      String peerNodeId,
      String expectedRole,
      String label,
      String origin,
      long nowEpochMillis) {
    requireText(localNodeId, "local node ID");
    if (localNodeId.equals(peerNodeId)) {
      throw new IllegalArgumentException("Peer identity resolves to this phone");
    }
    long generation = current == null ? 1 : Math.addExact(current.credentialGeneration(), 1);
    if (current != null
        && current.state() == State.ACTIVE
        && !current.peerNodeId().equals(peerNodeId)) {
      throw new IllegalArgumentException(
          "A different peer identity is already bound; revoke it before pairing another phone");
    }
    if (current != null
        && current.state() == State.ACTIVE
        && !current.expectedRole().equals(expectedRole)) {
      throw new IllegalArgumentException(
          "The verified peer role changed; revoke and re-pair before adopting it");
    }
    if (current != null && current.state() == State.ACTIVE) {
      PeerTransportSecurityPolicy.requireNoActiveProtectionDowngrade(current.origin(), origin);
    }
    return new PeerPairingBinding(
        State.ACTIVE,
        peerNodeId,
        expectedRole,
        label,
        origin,
        generation,
        nowEpochMillis,
        0);
  }

  PeerPairingBinding revoke(long nowEpochMillis) {
    if (state == State.REVOKED) {
      return this;
    }
    return new PeerPairingBinding(
        State.REVOKED,
        peerNodeId,
        expectedRole,
        label,
        origin,
        credentialGeneration,
        verifiedAtEpochMillis,
        nowEpochMillis);
  }

  boolean matchesVerifiedIdentity(String nodeId, String role) {
    return state == State.ACTIVE && peerNodeId.equals(nodeId) && expectedRole.equals(role);
  }

  private static String requireText(String value, String label) {
    Objects.requireNonNull(value, label);
    if (value.isBlank()) {
      throw new IllegalArgumentException(label + " cannot be blank");
    }
    return value;
  }
}
