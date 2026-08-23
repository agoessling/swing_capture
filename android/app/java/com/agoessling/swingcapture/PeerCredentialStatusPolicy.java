package com.agoessling.swingcapture;

/** Redacted setup health for a leader's write-only outbound peer credential. */
final class PeerCredentialStatusPolicy {
  enum Status {
    VERIFIED("verified"),
    RE_PAIR_REQUIRED("re_pair_required"),
    UNAVAILABLE("unavailable"),
    REVOKED("revoked");

    private final String wireName;

    Status(String wireName) {
      this.wireName = wireName;
    }

    String wireName() {
      return wireName;
    }
  }

  private PeerCredentialStatusPolicy() {}

  static Status evaluate(
      boolean revoked,
      boolean peerConfigured,
      boolean originMatches,
      boolean reachable,
      boolean identityMatches,
      boolean authenticationRejected) {
    if (revoked) {
      return Status.REVOKED;
    }
    if (!peerConfigured || !originMatches) {
      return Status.RE_PAIR_REQUIRED;
    }
    if (reachable && identityMatches) {
      return Status.VERIFIED;
    }
    return authenticationRejected ? Status.RE_PAIR_REQUIRED : Status.UNAVAILABLE;
  }
}
