package com.agoessling.swingcapture;

import java.util.List;
import java.util.Objects;

/** Platform-neutral LAN discovery boundary. Discovery observations are never authentication. */
interface LanNodeDiscovery extends AutoCloseable {
  record Advertisement(String nodeId, String role, String label, int port) {
    public Advertisement {
      nodeId = requireText(nodeId, "node ID");
      role = requireText(role, "role");
      label = requireText(label, "label");
      if (port < 1 || port > 65_535) {
        throw new IllegalArgumentException("port is invalid");
      }
    }
  }

  record Observation(
      String serviceInstance,
      String nodeId,
      String role,
      String label,
      String origin,
      long observedAtEpochMillis,
      long expiresAtEpochMillis) {
    public Observation {
      serviceInstance = requireText(serviceInstance, "service instance");
      nodeId = requireText(nodeId, "node ID");
      role = requireText(role, "role");
      label = requireText(label, "label");
      origin = requireText(origin, "origin");
      if (observedAtEpochMillis < 0 || expiresAtEpochMillis <= observedAtEpochMillis) {
        throw new IllegalArgumentException("discovery lifetime is invalid");
      }
    }
  }

  void start(Advertisement advertisement);

  void updateAdvertisement(Advertisement advertisement);

  List<Observation> snapshot(long nowEpochMillis);

  @Override
  void close();

  private static String requireText(String value, String label) {
    Objects.requireNonNull(value, label);
    if (value.isBlank()) {
      throw new IllegalArgumentException(label + " cannot be blank");
    }
    return value;
  }
}
