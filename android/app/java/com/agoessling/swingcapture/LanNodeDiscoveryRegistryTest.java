package com.agoessling.swingcapture;

import java.util.List;

/** Discovery expiry, address-change, and duplicate-identity coverage. */
public final class LanNodeDiscoveryRegistryTest {
  private LanNodeDiscoveryRegistryTest() {}

  public static void main(String[] arguments) {
    LanNodeDiscoveryRegistry registry = new LanNodeDiscoveryRegistry();
    registry.observed(observation("svc-a", "node-a", "http://10.0.0.2:8088", 100, 200));
    check(registry.snapshot(150).size() == 1, "fresh discovery retained");
    check(registry.snapshot(200).isEmpty(), "stale discovery removed");

    registry.observed(observation("svc-a", "node-a", "http://10.0.0.2:8088", 300, 500));
    registry.observed(observation("svc-a", "node-a", "http://10.0.0.8:8088", 350, 550));
    check(
        registry.snapshot(400).get(0).origin().equals("http://10.0.0.8:8088"),
        "service address refresh replaces old hint");

    registry.observed(observation("svc-duplicate", "node-a", "http://10.0.0.9:8088", 350, 550));
    List<LanNodeDiscovery.Observation> duplicates = registry.snapshot(400);
    check(duplicates.size() == 2, "duplicate identity retained for conflict reporting");
    check(duplicates.get(0).nodeId().equals(duplicates.get(1).nodeId()), "duplicate node ID visible");
    registry.lost("svc-a");
    check(registry.snapshot(400).size() == 1, "lost service removed");
  }

  private static LanNodeDiscovery.Observation observation(
      String service, String nodeId, String origin, long observed, long expires) {
    return new LanNodeDiscovery.Observation(
        service, nodeId, "face_on", "Pixel", origin, observed, expires);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
