package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.PoseTriggerController;

/** Ensures production stops false arms at the evidence lease without weakening the hard cap. */
public final class PoseHighSpeedNoImpactDeadlineTest {
  private static final long SECOND = 1_000_000_000L;

  private PoseHighSpeedNoImpactDeadlineTest() {}

  public static void main(String[] arguments) {
    PoseTriggerController.Config config =
        PoseTriggerController.Config.defaultsForFiveFramesPerSecond();
    PoseHighSpeedNoImpactDeadline.Deadline external =
        PoseHighSpeedNoImpactDeadline.externalArm(5 * SECOND, config);
    check(external.activeEvidenceStopNs() == 20 * SECOND, "external 15-second lease");
    check(external.thermalHardStopNs() == 35 * SECOND, "external 30-second hard cap");
    check(external.delayFrom(8 * SECOND) == 12 * SECOND, "startup consumes lease time");
    check(external.delayFrom(21 * SECOND) == 0, "late startup expires immediately");
    check(external.thermalHardCapDelayFrom(8 * SECOND) == 27 * SECOND, "hard cap is separate");
    check(external.thermalHardCapDelayFrom(36 * SECOND) == 0, "late hard cap is immediate");
    check(external.reason().equals("active_evidence_expired"), "evidence expiry reason");

    PoseTriggerController controller = new PoseTriggerController(config);
    controller.observe(address(0));
    controller.observe(address(200_000_000L));
    PoseTriggerController.Decision armed = controller.observe(address(400_000_000L));
    PoseHighSpeedNoImpactDeadline.Deadline local =
        PoseHighSpeedNoImpactDeadline.fromDecision(armed);
    check(local.activeEvidenceStopNs() == 15_400_000_000L, "decision lease preserved");
    check(local.thermalHardStopNs() == 30_400_000_000L, "decision cap preserved");
  }

  private static PoseTriggerController.Observation address(long timestampNs) {
    return new PoseTriggerController.Observation(timestampNs, 0.9, 0.9, 0.1, true);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
