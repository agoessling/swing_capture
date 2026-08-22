package com.agoessling.swingcapture;

import java.util.List;

/** Setup schema, credential-redaction, revision, and self-peer policy coverage. */
public final class NodeSetupPolicyTest {
  private NodeSetupPolicyTest() {}

  public static void main(String[] arguments) {
    check(NodeSetupPolicy.SCHEMA_VERSION == 1, "setup schema version");
    NodeSetupPolicy.PeerCredentials existing =
        new NodeSetupPolicy.PeerCredentials("http://pixel5a:8080", "peer-secret-value");
    check(
        NodeSetupPolicy.updatePeer(existing, "keep", null, null).equals(existing),
        "keep preserves both secret fields atomically");
    check(
        NodeSetupPolicy.updatePeer(existing, "clear", null, null)
            .equals(new NodeSetupPolicy.PeerCredentials("", "")),
        "clear removes both secret fields atomically");
    NodeSetupPolicy.PeerCredentials replacement =
        NodeSetupPolicy.updatePeer(
            existing, "replace", "http://pixel6:8080", "replacement-secret");
    NodeSetupPolicy.RedactedPeer redacted = NodeSetupPolicy.redact(replacement).orElseThrow();
    check(redacted.origin().equals("http://pixel6:8080"), "redacted origin");
    check(!redacted.toString().contains("replacement-secret"), "token never enters response view");

    check(NodeSetupPolicy.revisionMatches(4, 4), "matching revision");
    check(!NodeSetupPolicy.revisionMatches(4, 5), "stale revision conflict");
    check(
        NodeSetupPolicy.isDirectSelfOrigin(
            "http://192.168.1.10:8080/", 8080, List.of("http://192.168.1.10:8080")),
        "advertised self origin");
    check(
        NodeSetupPolicy.isDirectSelfOrigin("http://127.0.0.1:8080", 8080, List.of()),
        "loopback self origin");
    check(NodeSetupPolicy.isSameNode("node-a", true, "node-a"), "probed same node");
    check(!NodeSetupPolicy.isSameNode("node-a", false, "node-a"), "offline is not proven self");
    NodeSetupPolicy.requireOutboundPeerAllowed(
        "leader", new NodeSetupPolicy.PeerCredentials("http://pixel5a:8080", "peer-secret"));
    NodeSetupPolicy.requireOutboundPeerAllowed(
        "shadow", new NodeSetupPolicy.PeerCredentials("", ""));
    expectFailure(
        () ->
            NodeSetupPolicy.requireOutboundPeerAllowed(
                "shadow",
                new NodeSetupPolicy.PeerCredentials("http://pixel6:8080", "peer-secret")),
        "shadow outbound peer");
    expectFailure(
        () ->
            NodeSetupPolicy.requireOutboundPeerAllowed(
                "disabled",
                new NodeSetupPolicy.PeerCredentials("http://pixel6:8080", "peer-secret")),
        "disabled outbound peer");
    check(
        topology("disabled", false, false, "", "", "")
            == NodeSetupPolicy.PeerTopologyIssue.NONE,
        "disabled pose does not require a peer");
    check(
        topology("disabled", true, true, "node-b", "face_on", "shadow")
            == NodeSetupPolicy.PeerTopologyIssue.NON_LEADER_HAS_PEER,
        "disabled pose cannot retain a dormant outbound peer");
    check(
        topology("leader", false, false, "", "", "")
            == NodeSetupPolicy.PeerTopologyIssue.LEADER_MISSING_PEER,
        "leader requires a peer");
    check(
        topology("leader", true, false, "", "", "")
            == NodeSetupPolicy.PeerTopologyIssue.PEER_UNREACHABLE,
        "leader requires a reachable peer descriptor");
    check(
        topology("leader", true, true, "node-a", "face_on", "shadow")
            == NodeSetupPolicy.PeerTopologyIssue.SAME_NODE,
        "leader cannot target itself");
    check(
        NodeSetupPolicy.peerTopologyIssue(
                "node-a", "unassigned", "leader", true, true, "node-b", "face_on", "shadow")
            == NodeSetupPolicy.PeerTopologyIssue.LOCAL_ROLE_UNASSIGNED,
        "leader requires an assigned local camera role");
    check(
        topology("leader", true, true, "node-b", "unassigned", "shadow")
            == NodeSetupPolicy.PeerTopologyIssue.PEER_ROLE_UNASSIGNED,
        "leader requires an assigned peer camera role");
    check(
        topology("leader", true, true, "node-b", "down_the_line", "shadow")
            == NodeSetupPolicy.PeerTopologyIssue.SAME_ROLE,
        "leader and peer require distinct camera roles");
    check(
        topology("leader", true, true, "node-b", "face_on", "leader")
            == NodeSetupPolicy.PeerTopologyIssue.PEER_NOT_SHADOW,
        "leader must target a shadow");
    check(
        topology("leader", true, true, "node-b", "face_on", "shadow")
            == NodeSetupPolicy.PeerTopologyIssue.NONE,
        "leader to distinct-role shadow is valid");
    check(
        topology("shadow", false, false, "", "", "")
            == NodeSetupPolicy.PeerTopologyIssue.NONE,
        "shadow does not require an outbound peer association");
    check(
        topology("shadow", true, true, "node-b", "face_on", "leader")
            == NodeSetupPolicy.PeerTopologyIssue.NON_LEADER_HAS_PEER,
        "shadow cannot form a reciprocal outbound peer association");
  }

  private static NodeSetupPolicy.PeerTopologyIssue topology(
      String localMode,
      boolean peerConfigured,
      boolean peerReachable,
      String peerNode,
      String peerRole,
      String peerMode) {
    return NodeSetupPolicy.peerTopologyIssue(
        "node-a",
        "down_the_line",
        localMode,
        peerConfigured,
        peerReachable,
        peerNode,
        peerRole,
        peerMode);
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
