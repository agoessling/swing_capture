package com.agoessling.swingcapture;

public final class PoseHilEndpointAccessTest {
  public static void main(String[] args) {
    PoseHilEndpointAccess.requireEnabled(true);

    try {
      PoseHilEndpointAccess.requireEnabled(false);
      throw new AssertionError("production launch must reject the deterministic HIL endpoint");
    } catch (IllegalStateException expected) {
      if (!"HIL pose-arm endpoint is disabled".equals(expected.getMessage())) {
        throw new AssertionError("unexpected rejection diagnostic: " + expected.getMessage());
      }
    }
  }
}
