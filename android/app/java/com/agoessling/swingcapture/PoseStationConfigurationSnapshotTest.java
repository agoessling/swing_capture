package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegatePolicy;
import java.security.SecureRandom;

public final class PoseStationConfigurationSnapshotTest {
  private PoseStationConfigurationSnapshotTest() {}

  public static void main(String[] args) {
    PoseStationConfigurationSnapshot defaults = PoseStationConfigurationSnapshot.defaults();
    check(defaults.mode() == PoseNodeMode.DISABLED, "default mode");
    check(
        defaults.delegatePolicy() == PoseInferenceDelegatePolicy.GPU_PREFERRED,
        "default delegate");
    check(!defaults.debugEvidenceEnabled(), "default evidence is off to avoid continuous JPEGs");
    check(!defaults.hasPeer(), "default peer");

    String token = BearerAuthorization.generate(new SecureRandom());
    PoseStationConfigurationSnapshot configured =
        new PoseStationConfigurationSnapshot(
            PoseNodeMode.LEADER,
            PoseInferenceDelegatePolicy.CPU_ONLY,
            new NormalizedHittingRegion(0.2, 0.3, 0.8, 0.9),
            false,
            " http://192.168.1.20:8088 ",
            " " + token + " ");
    check(configured.hasPeer(), "configured peer");
    check(configured.peerOrigin().equals("http://192.168.1.20:8088"), "trimmed origin");
    check(configured.peerControlToken().equals(token), "trimmed token");
    check(configured.delegateWireName().equals("cpu_only"), "delegate wire name");

    for (PoseInferenceDelegatePolicy policy : PoseInferenceDelegatePolicy.values()) {
      check(
          PoseStationConfigurationSnapshot.parseDelegatePolicy(policy.name().toLowerCase())
              == policy,
          "delegate round trip");
    }
    expectFailure(
        () ->
            new PoseStationConfigurationSnapshot(
                PoseNodeMode.LEADER,
                PoseInferenceDelegatePolicy.CPU_ONLY,
                PoseStationConfigurationSnapshot.DEFAULT_HITTING_REGION,
                true,
                "http://host:8088",
                ""),
        "partial peer");
    expectFailure(
        () ->
            new PoseStationConfigurationSnapshot(
                PoseNodeMode.LEADER,
                PoseInferenceDelegatePolicy.CPU_ONLY,
                PoseStationConfigurationSnapshot.DEFAULT_HITTING_REGION,
                true,
                "https://host:8088",
                token),
        "https peer");
    expectFailure(
        () -> PoseStationConfigurationSnapshot.parseDelegatePolicy("npu"),
        "unknown delegate");
  }

  private static void expectFailure(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
