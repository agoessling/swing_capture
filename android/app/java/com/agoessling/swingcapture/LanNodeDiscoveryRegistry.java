package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Thread-safe observation registry shared by the Android NSD adapter and deterministic tests. */
final class LanNodeDiscoveryRegistry {
  private final Map<String, LanNodeDiscovery.Observation> byServiceInstance = new HashMap<>();

  synchronized void observed(LanNodeDiscovery.Observation observation) {
    byServiceInstance.put(observation.serviceInstance(), observation);
  }

  synchronized void lost(String serviceInstance) {
    byServiceInstance.remove(serviceInstance);
  }

  synchronized List<LanNodeDiscovery.Observation> snapshot(long nowEpochMillis) {
    byServiceInstance.values().removeIf(value -> value.expiresAtEpochMillis() <= nowEpochMillis);
    List<LanNodeDiscovery.Observation> result = new ArrayList<>(byServiceInstance.values());
    result.sort(
        Comparator.comparing(LanNodeDiscovery.Observation::label)
            .thenComparing(LanNodeDiscovery.Observation::nodeId)
            .thenComparing(LanNodeDiscovery.Observation::origin));
    return List.copyOf(result);
  }
}
