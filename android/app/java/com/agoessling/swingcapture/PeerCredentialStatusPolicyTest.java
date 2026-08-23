package com.agoessling.swingcapture;

/** Deterministic coverage for stale-token and transient-unavailability setup semantics. */
public final class PeerCredentialStatusPolicyTest {
  private PeerCredentialStatusPolicyTest() {}

  public static void main(String[] arguments) {
    check(
        status(false, true, true, true, true, false)
            == PeerCredentialStatusPolicy.Status.VERIFIED,
        "authenticated matching peer verified");
    check(
        status(false, true, true, false, false, true)
            == PeerCredentialStatusPolicy.Status.RE_PAIR_REQUIRED,
        "remote rotation rejection requires explicit re-pair");
    check(
        status(false, true, true, false, false, false)
            == PeerCredentialStatusPolicy.Status.UNAVAILABLE,
        "network outage does not silently revoke or verify");
    check(
        status(false, false, false, false, false, false)
            == PeerCredentialStatusPolicy.Status.RE_PAIR_REQUIRED,
        "missing write-only credential requires re-pair");
    check(
        status(true, true, true, true, true, false)
            == PeerCredentialStatusPolicy.Status.REVOKED,
        "revocation remains terminal");
  }

  private static PeerCredentialStatusPolicy.Status status(
      boolean revoked,
      boolean configured,
      boolean originMatches,
      boolean reachable,
      boolean identityMatches,
      boolean authenticationRejected) {
    return PeerCredentialStatusPolicy.evaluate(
        revoked,
        configured,
        originMatches,
        reachable,
        identityMatches,
        authenticationRejected);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
