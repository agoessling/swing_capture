package com.agoessling.swingcapture;

/** Guards the deterministic pose transition endpoint behind an explicit HIL launch. */
final class PoseHilEndpointAccess {
  private PoseHilEndpointAccess() {}

  static void requireEnabled(boolean enabled) {
    if (!enabled) {
      throw new IllegalStateException("HIL pose-arm endpoint is disabled");
    }
  }
}
