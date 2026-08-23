package com.agoessling.swingcapture;

public final class PoseIntentPeerPolicyTest {
  private PoseIntentPeerPolicyTest() {}

  public static void main(String[] arguments) {
    PoseIntentPeerPolicy.Peer cleared =
        PoseIntentPeerPolicy.resolve(
            PoseNodeMode.SHADOW,
            true,
            "http://10.0.0.2:8088",
            "peer-token",
            false,
            "",
            false,
            "");
    check(cleared.origin().isEmpty(), "explicit shadow transition clears origin");
    check(cleared.controlToken().isEmpty(), "explicit shadow transition clears token");

    PoseIntentPeerPolicy.Peer kept =
        PoseIntentPeerPolicy.resolve(
            PoseNodeMode.LEADER,
            true,
            "http://10.0.0.2:8088",
            "peer-token",
            false,
            "",
            false,
            "");
    check(kept.origin().equals("http://10.0.0.2:8088"), "leader keeps peer origin");
    check(kept.controlToken().equals("peer-token"), "leader keeps peer token");

    PoseIntentPeerPolicy.Peer unchanged =
        PoseIntentPeerPolicy.resolve(
            PoseNodeMode.SHADOW,
            false,
            "",
            "",
            false,
            "",
            false,
            "");
    check(unchanged.origin().isEmpty(), "unrelated intent preserves empty origin");

    PoseIntentPeerPolicy.Peer replaced =
        PoseIntentPeerPolicy.resolve(
            PoseNodeMode.LEADER,
            false,
            "http://old:8088",
            "old-token",
            true,
            "http://new:8088",
            true,
            "new-token");
    check(replaced.origin().equals("http://new:8088"), "explicit origin replaces current value");
    check(replaced.controlToken().equals("new-token"), "explicit token replaces current value");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
