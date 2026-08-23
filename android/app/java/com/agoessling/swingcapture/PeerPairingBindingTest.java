package com.agoessling.swingcapture;

/** Deterministic identity, address recovery, credential, and revocation transition coverage. */
public final class PeerPairingBindingTest {
  private PeerPairingBindingTest() {}

  public static void main(String[] arguments) {
    PeerPairingBinding first =
        PeerPairingBinding.verifyAndActivate(
            null,
            "leader-id",
            "shadow-id",
            "face_on",
            "Pixel 6",
            "http://10.0.0.2:8088",
            100);
    check(first.state() == PeerPairingBinding.State.ACTIVE, "initial binding active");
    check(first.credentialGeneration() == 1, "initial credential generation");

    PeerPairingBinding moved =
        PeerPairingBinding.verifyAndActivate(
            first,
            "leader-id",
            "shadow-id",
            "face_on",
            "Pixel 6",
            "http://10.0.0.9:8088",
            200);
    check(moved.origin().equals("http://10.0.0.9:8088"), "authenticated address recovery");
    check(moved.credentialGeneration() == 2, "replacement rotates credential generation");

    expectFailure(
        () ->
            PeerPairingBinding.verifyAndActivate(
                moved,
                "leader-id",
                "duplicate-installation-id",
                "face_on",
                "Unknown phone",
                "http://10.0.0.10:8088",
                300),
        "active identity substitution");
    expectFailure(
        () ->
            PeerPairingBinding.verifyAndActivate(
                moved,
                "leader-id",
                "shadow-id",
                "down_the_line",
                "Pixel 6",
                "http://10.0.0.9:8088",
                300),
        "active role conflict");
    expectFailure(
        () ->
            PeerPairingBinding.verifyAndActivate(
                null,
                "leader-id",
                "leader-id",
                "face_on",
                "Self",
                "http://127.0.0.1:8088",
                300),
        "self identity");

    PeerPairingBinding revoked = moved.revoke(400);
    check(revoked.state() == PeerPairingBinding.State.REVOKED, "revocation tombstone");
    check(revoked.revokedAtEpochMillis() == 400, "revocation timestamp");
    check(!revoked.matchesVerifiedIdentity("shadow-id", "face_on"), "revoked identity inactive");

    PeerPairingBinding repaired =
        PeerPairingBinding.verifyAndActivate(
            revoked,
            "leader-id",
            "new-shadow-id",
            "face_on",
            "Replacement Pixel",
            "http://10.0.0.11:8088",
            500);
    check(repaired.peerNodeId().equals("new-shadow-id"), "explicit post-revoke re-pair");
    check(repaired.credentialGeneration() == 3, "generation survives revocation and re-pair");

    PeerPairingBinding protectedBinding =
        PeerPairingBinding.verifyAndActivate(
            null,
            "leader-id",
            "protected-shadow",
            "face_on",
            "Protected peer",
            "https://capture.example:8443",
            600);
    PeerPairingBinding protectedAddressRecovery =
        PeerPairingBinding.verifyAndActivate(
            protectedBinding,
            "leader-id",
            "protected-shadow",
            "face_on",
            "Protected peer",
            "https://new-capture.example:8443",
            700);
    check(
        protectedAddressRecovery.origin().equals("https://new-capture.example:8443"),
        "protected address recovery stays protected");
    expectFailure(
        () ->
            PeerPairingBinding.verifyAndActivate(
                protectedAddressRecovery,
                "leader-id",
                "protected-shadow",
                "face_on",
                "Protected peer",
                "http://10.0.0.2:8088",
                800),
        "active protected binding downgrade");
    PeerPairingBinding explicitlyRevoked = protectedAddressRecovery.revoke(800);
    PeerPairingBinding explicitDemoRepair =
        PeerPairingBinding.verifyAndActivate(
            explicitlyRevoked,
            "leader-id",
            "protected-shadow",
            "face_on",
            "Protected peer",
            "http://10.0.0.2:8088",
            900);
    check(
        explicitDemoRepair.origin().equals("http://10.0.0.2:8088"),
        "revocation makes trusted-LAN downgrade explicit");
  }

  private static void expectFailure(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
