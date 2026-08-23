package com.agoessling.swingcapture;

import java.net.URI;
import java.util.Objects;

/** Exact-origin policy for peer traffic that may carry station credentials or capabilities. */
final class PeerTransportSecurityPolicy {
  enum Requirement {
    /** Prototype mode: cleartext is an explicit trusted-LAN exception, not a secure transport. */
    TRUSTED_LAN_DEMO_ALLOWED,
    /** Production boundary: credentials may be sent only over HTTPS. */
    PROTECTED_ONLY
  }

  enum Classification {
    CLEARTEXT_TRUSTED_LAN_DEMO("cleartext_trusted_lan_demo", false),
    PROTECTED_HTTPS("protected_https", true);

    private final String wireName;
    private final boolean protectedTransport;

    Classification(String wireName, boolean protectedTransport) {
      this.wireName = wireName;
      this.protectedTransport = protectedTransport;
    }

    String wireName() {
      return wireName;
    }

    boolean protectedTransport() {
      return protectedTransport;
    }
  }

  record ValidatedOrigin(URI uri, Classification classification) {
    ValidatedOrigin {
      Objects.requireNonNull(uri, "uri");
      Objects.requireNonNull(classification, "classification");
    }
  }

  private PeerTransportSecurityPolicy() {}

  static ValidatedOrigin validateOrigin(String value, Requirement requirement) {
    Objects.requireNonNull(value, "value");
    Objects.requireNonNull(requirement, "requirement");
    URI origin;
    try {
      origin = URI.create(value);
    } catch (IllegalArgumentException invalid) {
      throw new IllegalArgumentException("peer origin is not a valid URI", invalid);
    }

    String scheme = origin.getScheme();
    Classification classification =
        switch (scheme == null ? "" : scheme) {
          case "http" -> Classification.CLEARTEXT_TRUSTED_LAN_DEMO;
          case "https" -> Classification.PROTECTED_HTTPS;
          default -> throw invalidOrigin();
        };
    if (origin.getHost() == null
        || origin.getUserInfo() != null
        || origin.getQuery() != null
        || origin.getFragment() != null
        || !(origin.getPath().isEmpty() || origin.getPath().equals("/"))
        || origin.getPort() <= 0
        || origin.getPort() > 65_535) {
      throw invalidOrigin();
    }
    if (requirement == Requirement.PROTECTED_ONLY && !classification.protectedTransport()) {
      throw new IllegalArgumentException(
          "protected peer transport requires an https://host:port origin");
    }
    return new ValidatedOrigin(origin, classification);
  }

  static Classification classify(String origin) {
    return validateOrigin(origin, Requirement.TRUSTED_LAN_DEMO_ALLOWED).classification();
  }

  /** An active protected binding cannot silently become cleartext during address recovery. */
  static void requireNoActiveProtectionDowngrade(String currentOrigin, String replacementOrigin) {
    Classification current = classify(currentOrigin);
    Classification replacement = classify(replacementOrigin);
    if (current.protectedTransport() && !replacement.protectedTransport()) {
      throw new IllegalArgumentException(
          "An active protected peer cannot move to cleartext; revoke and explicitly re-pair");
    }
  }

  private static IllegalArgumentException invalidOrigin() {
    return new IllegalArgumentException(
        "peer origin must be http://host:port or https://host:port with no path");
  }
}
