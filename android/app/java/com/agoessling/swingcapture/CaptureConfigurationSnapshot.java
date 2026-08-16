package com.agoessling.swingcapture;

import java.util.Objects;

/** Immutable node identity, role, and capture profile fixed at the start of one capture run. */
public record CaptureConfigurationSnapshot(
    String nodeId, CaptureRole role, CaptureProfile profile) {
  public CaptureConfigurationSnapshot {
    Objects.requireNonNull(nodeId, "nodeId");
    Objects.requireNonNull(role, "role");
    Objects.requireNonNull(profile, "profile");
    if (nodeId.isBlank()) {
      throw new IllegalArgumentException("nodeId cannot be blank");
    }
  }

  public void requireAssignedRole() {
    if (role == CaptureRole.UNASSIGNED) {
      throw new IllegalStateException("Assign this node a camera role before capturing");
    }
  }

  public String mediaFileName() {
    requireAssignedRole();
    return role.wireName() + ".mp4";
  }
}
