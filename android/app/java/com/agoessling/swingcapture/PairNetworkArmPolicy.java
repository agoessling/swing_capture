package com.agoessling.swingcapture;

import java.util.Objects;

/** Fail-closed pair-network admission policy for capture arm requests. */
final class PairNetworkArmPolicy {
  static final String DEGRADED_MESSAGE =
      "pair network health is degraded; set allow_degraded_network=true to acknowledge degraded service";
  static final String UNUSABLE_MESSAGE =
      "pair network health is unusable; restore bidirectional phone-to-phone connectivity before arming";

  private PairNetworkArmPolicy() {}

  /**
   * Rejects unsafe leader arm requests with an {@link IllegalStateException} suitable for an HTTP
   * 409 response. Disarm requests and nodes that do not coordinate an outbound peer are unaffected.
   */
  static void requireAllowed(
      boolean armed,
      PoseNodeMode poseMode,
      PairNetworkHealthPolicy.State stabilizedState,
      boolean allowDegradedNetwork) {
    Objects.requireNonNull(poseMode, "poseMode");
    Objects.requireNonNull(stabilizedState, "stabilizedState");
    if (!armed || poseMode != PoseNodeMode.LEADER) {
      return;
    }
    switch (stabilizedState) {
      case GOOD -> {
        return;
      }
      case DEGRADED -> {
        if (allowDegradedNetwork) {
          return;
        }
        throw new IllegalStateException(DEGRADED_MESSAGE);
      }
      case UNUSABLE -> throw new IllegalStateException(UNUSABLE_MESSAGE);
    }
  }
}
