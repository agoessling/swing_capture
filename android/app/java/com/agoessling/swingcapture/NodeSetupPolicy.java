package com.agoessling.swingcapture;

import java.net.URI;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/** Pure setup-contract policy shared by strict HTTP parsing and persistence validation. */
final class NodeSetupPolicy {
  static final int SCHEMA_VERSION = 1;

  enum PeerTopologyIssue {
    NONE,
    NON_LEADER_HAS_PEER,
    LEADER_MISSING_PEER,
    PEER_UNREACHABLE,
    SAME_NODE,
    LOCAL_ROLE_UNASSIGNED,
    PEER_ROLE_UNASSIGNED,
    SAME_ROLE,
    PEER_NOT_SHADOW
  }

  record PeerCredentials(String origin, String controlToken) {
    PeerCredentials {
      Objects.requireNonNull(origin, "origin");
      Objects.requireNonNull(controlToken, "controlToken");
      if (origin.isEmpty() != controlToken.isEmpty()) {
        throw new IllegalArgumentException("peer origin and control token must be configured together");
      }
    }
  }

  /** Deliberately contains no credential field and is safe to serialize to the browser. */
  record RedactedPeer(String origin) {
    RedactedPeer {
      if (origin == null || origin.isBlank()) {
        throw new IllegalArgumentException("redacted peer origin cannot be blank");
      }
    }
  }

  private NodeSetupPolicy() {}

  static PeerCredentials updatePeer(
      PeerCredentials current, String operation, String replacementOrigin, String replacementToken) {
    Objects.requireNonNull(current, "current");
    return switch (operation) {
      case "keep" -> current;
      case "clear" -> new PeerCredentials("", "");
      case "replace" -> new PeerCredentials(replacementOrigin, replacementToken);
      default -> throw new IllegalArgumentException("Unknown peer update operation: " + operation);
    };
  }

  static Optional<RedactedPeer> redact(PeerCredentials credentials) {
    Objects.requireNonNull(credentials, "credentials");
    return credentials.origin().isEmpty()
        ? Optional.empty()
        : Optional.of(new RedactedPeer(credentials.origin()));
  }

  static boolean revisionMatches(long expected, long current) {
    return expected == current;
  }

  static boolean isSameNode(String localNodeId, boolean peerReachable, String peerNodeId) {
    return peerReachable && localNodeId.equals(peerNodeId);
  }

  static void requireOutboundPeerAllowed(String localPoseMode, PeerCredentials peer) {
    Objects.requireNonNull(localPoseMode, "localPoseMode");
    Objects.requireNonNull(peer, "peer");
    if (!localPoseMode.equals("leader") && !peer.origin().isEmpty()) {
      throw new IllegalArgumentException(
          "Only pose leader mode may configure an outbound peer association");
    }
  }

  static PeerTopologyIssue peerTopologyIssue(
      String localNodeId,
      String localRole,
      String localPoseMode,
      boolean peerConfigured,
      boolean peerReachable,
      String peerNodeId,
      String peerRole,
      String peerPoseMode) {
    Objects.requireNonNull(localNodeId, "localNodeId");
    Objects.requireNonNull(localRole, "localRole");
    Objects.requireNonNull(localPoseMode, "localPoseMode");
    Objects.requireNonNull(peerNodeId, "peerNodeId");
    Objects.requireNonNull(peerRole, "peerRole");
    Objects.requireNonNull(peerPoseMode, "peerPoseMode");
    if (!localPoseMode.equals("leader")) {
      return peerConfigured ? PeerTopologyIssue.NON_LEADER_HAS_PEER : PeerTopologyIssue.NONE;
    }
    if (!peerConfigured) {
      return PeerTopologyIssue.LEADER_MISSING_PEER;
    }
    if (!peerReachable) {
      return PeerTopologyIssue.PEER_UNREACHABLE;
    }
    if (localNodeId.equals(peerNodeId)) {
      return PeerTopologyIssue.SAME_NODE;
    }
    if (localRole.equals("unassigned")) {
      return PeerTopologyIssue.LOCAL_ROLE_UNASSIGNED;
    }
    if (peerRole.equals("unassigned")) {
      return PeerTopologyIssue.PEER_ROLE_UNASSIGNED;
    }
    if (localRole.equals(peerRole)) {
      return PeerTopologyIssue.SAME_ROLE;
    }
    if (!peerPoseMode.equals("shadow")) {
      return PeerTopologyIssue.PEER_NOT_SHADOW;
    }
    return PeerTopologyIssue.NONE;
  }

  static boolean isDirectSelfOrigin(String origin, int port, List<String> advertisedUrls) {
    String normalized = origin.endsWith("/") ? origin.substring(0, origin.length() - 1) : origin;
    if (advertisedUrls.stream().anyMatch(normalized::equals)) {
      return true;
    }
    URI parsed = URI.create(origin);
    String host = parsed.getHost();
    return parsed.getPort() == port
        && ("localhost".equalsIgnoreCase(host)
            || "127.0.0.1".equals(host)
            || "0.0.0.0".equals(host));
  }
}
