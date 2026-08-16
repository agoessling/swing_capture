package com.agoessling.swingcapture;

/** Deterministic host tests for immutable per-arm capture identity. */
public final class CaptureConfigurationSnapshotTest {
  private CaptureConfigurationSnapshotTest() {}

  public static void main(String[] args) {
    snapshotsIdentityRoleAndProfile();
    rejectsInvalidOrUnassignedIdentity();
  }

  private static void snapshotsIdentityRoleAndProfile() {
    MutableConfiguration source =
        new MutableConfiguration(
            "node-a", CaptureRole.DOWN_THE_LINE, CaptureProfile.FULL_HD_240);
    CaptureConfigurationSnapshot snapshot = source.snapshot();
    source.nodeId = "node-b";
    source.role = CaptureRole.FACE_ON;
    source.profile = CaptureProfile.HD_240;

    check(snapshot.nodeId().equals("node-a"), "node identity is fixed");
    check(snapshot.role() == CaptureRole.DOWN_THE_LINE, "role is fixed");
    check(snapshot.profile() == CaptureProfile.FULL_HD_240, "profile is fixed");
    check(snapshot.mediaFileName().equals("down_the_line.mp4"), "media name uses fixed role");
  }

  private static void rejectsInvalidOrUnassignedIdentity() {
    expectFailure(
        IllegalArgumentException.class,
        () ->
            new CaptureConfigurationSnapshot(
                " ", CaptureRole.DOWN_THE_LINE, CaptureProfile.FULL_HD_240),
        "blank node ID must fail");
    CaptureConfigurationSnapshot unassigned =
        new CaptureConfigurationSnapshot(
            "node-a", CaptureRole.UNASSIGNED, CaptureProfile.FULL_HD_240);
    expectFailure(
        IllegalStateException.class,
        unassigned::mediaFileName,
        "unassigned role cannot produce capture artifacts");
  }

  private static void expectFailure(
      Class<? extends Throwable> expectedType, Runnable operation, String message) {
    try {
      operation.run();
    } catch (Throwable failure) {
      if (expectedType.isInstance(failure)) {
        return;
      }
      throw new AssertionError(message + ": unexpected " + failure, failure);
    }
    throw new AssertionError(message);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static final class MutableConfiguration {
    private String nodeId;
    private CaptureRole role;
    private CaptureProfile profile;

    private MutableConfiguration(String nodeId, CaptureRole role, CaptureProfile profile) {
      this.nodeId = nodeId;
      this.role = role;
      this.profile = profile;
    }

    private CaptureConfigurationSnapshot snapshot() {
      return new CaptureConfigurationSnapshot(nodeId, role, profile);
    }
  }
}
