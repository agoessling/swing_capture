package com.agoessling.swingcapture;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.URI;
import java.util.Objects;

/** Locks a reverse network-health callback to the authenticated request's TCP peer. */
final class PairNetworkHealthCallbackPolicy {
  private PairNetworkHealthCallbackPolicy() {}

  static URI validate(String requestedOrigin, InetAddress acceptedSocketPeer, int listenerPort) {
    Objects.requireNonNull(acceptedSocketPeer, "acceptedSocketPeer");
    if (!(acceptedSocketPeer instanceof Inet4Address)
        || listenerPort <= 0
        || listenerPort > 65_535) {
      throw invalidCallback();
    }
    PeerTransportSecurityPolicy.ValidatedOrigin validated =
        PeerTransportSecurityPolicy.validateOrigin(
            requestedOrigin, PeerTransportSecurityPolicy.Requirement.TRUSTED_LAN_DEMO_ALLOWED);
    URI origin = validated.uri();
    if (validated.classification()
            != PeerTransportSecurityPolicy.Classification.CLEARTEXT_TRUSTED_LAN_DEMO
        || origin.getPort() != listenerPort
        || !origin.getHost().equals(acceptedSocketPeer.getHostAddress())) {
      throw invalidCallback();
    }
    return origin;
  }

  private static IllegalArgumentException invalidCallback() {
    return new IllegalArgumentException(
        "reverse callback must be the accepted IPv4 socket peer at the node listener port");
  }
}
