package com.agoessling.swingcapture;

/** Deterministic protected-transport classification and downgrade coverage. */
public final class PeerTransportSecurityPolicyTest {
  private PeerTransportSecurityPolicyTest() {}

  public static void main(String[] arguments) {
    PeerTransportSecurityPolicy.ValidatedOrigin demo =
        PeerTransportSecurityPolicy.validateOrigin(
            "http://10.0.0.2:8088",
            PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED);
    check(
        demo.classification()
            == PeerTransportSecurityPolicy.Classification.CLEARTEXT_TRUSTED_LAN_DEMO,
        "HTTP is explicitly classified as the trusted-LAN demo exception");
    check(!demo.classification().protectedTransport(), "HTTP is never mislabeled protected");

    PeerTransportSecurityPolicy.ValidatedOrigin protectedOrigin =
        PeerTransportSecurityPolicy.validateOrigin(
            "https://capture.example:8443",
            PeerTransportSecurityPolicy.Requirement.PROTECTED_ONLY);
    check(
        protectedOrigin.classification()
            == PeerTransportSecurityPolicy.Classification.PROTECTED_HTTPS,
        "HTTPS satisfies protected-only policy");
    check(
        protectedOrigin.classification().wireName().equals("protected_https"),
        "stable protected classification");

    expectFailure(
        () ->
            PeerTransportSecurityPolicy.validateOrigin(
                "http://10.0.0.2:8088",
                PeerTransportSecurityPolicy.Requirement.PROTECTED_ONLY),
        "protected-only rejects HTTP");
    for (String invalid :
        new String[] {
          "ftp://capture.example:21",
          "https://capture.example",
          "https://user@capture.example:8443",
          "https://capture.example:8443/path",
          "https://capture.example:8443?token=secret",
          "https://capture.example:8443#fragment"
        }) {
      expectFailure(
          () ->
              PeerTransportSecurityPolicy.validateOrigin(
                  invalid, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED),
          "invalid exact origin " + invalid);
    }

    PeerTransportSecurityPolicy.requireNoActiveProtectionDowngrade(
        "http://10.0.0.2:8088", "https://capture.example:8443");
    PeerTransportSecurityPolicy.requireNoActiveProtectionDowngrade(
        "https://old.example:8443", "https://new.example:8443");
    expectFailure(
        () ->
            PeerTransportSecurityPolicy.requireNoActiveProtectionDowngrade(
                "https://capture.example:8443", "http://10.0.0.2:8088"),
        "active HTTPS binding cannot silently downgrade");
  }

  private static void expectFailure(Runnable action, String label) {
    try {
      action.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
