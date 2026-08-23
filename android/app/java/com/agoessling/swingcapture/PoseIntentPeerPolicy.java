package com.agoessling.swingcapture;

import java.util.Objects;

/** Resolves peer fields when an activity intent changes the pose role. */
final class PoseIntentPeerPolicy {
  record Peer(String origin, String controlToken) {
    Peer {
      Objects.requireNonNull(origin, "origin");
      Objects.requireNonNull(controlToken, "controlToken");
    }
  }

  private PoseIntentPeerPolicy() {}

  static Peer resolve(
      PoseNodeMode requestedMode,
      boolean modeExplicit,
      String currentOrigin,
      String currentControlToken,
      boolean originExplicit,
      String requestedOrigin,
      boolean controlTokenExplicit,
      String requestedControlToken) {
    Objects.requireNonNull(requestedMode, "requestedMode");
    Objects.requireNonNull(currentOrigin, "currentOrigin");
    Objects.requireNonNull(currentControlToken, "currentControlToken");
    Objects.requireNonNull(requestedOrigin, "requestedOrigin");
    Objects.requireNonNull(requestedControlToken, "requestedControlToken");
    if (modeExplicit
        && requestedMode != PoseNodeMode.LEADER
        && !originExplicit
        && !controlTokenExplicit) {
      return new Peer("", "");
    }
    return new Peer(
        originExplicit ? requestedOrigin : currentOrigin,
        controlTokenExplicit ? requestedControlToken : currentControlToken);
  }
}
