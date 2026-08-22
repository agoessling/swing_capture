package com.agoessling.swingcapture;

import com.agoessling.swingcapture.pose.PoseTriggerController;

/** Production deadline while 5 Hz observations are unavailable during 240 fps capture. */
final class PoseHighSpeedNoImpactDeadline {
  record Deadline(long activeEvidenceStopNs, long thermalHardStopNs) {
    Deadline {
      if (activeEvidenceStopNs <= 0
          || thermalHardStopNs <= 0
          || activeEvidenceStopNs > thermalHardStopNs) {
        throw new IllegalArgumentException("pose high-speed deadlines are inconsistent");
      }
    }

    long effectiveStopNs() {
      return Math.min(activeEvidenceStopNs, thermalHardStopNs);
    }

    long delayFrom(long nowNs) {
      return Math.max(0, effectiveStopNs() - nowNs);
    }

    long thermalHardCapDelayFrom(long nowNs) {
      return Math.max(0, thermalHardStopNs - nowNs);
    }

    String reason() {
      return activeEvidenceStopNs < thermalHardStopNs
          ? "active_evidence_expired"
          : "thermal_hard_cap_reached";
    }
  }

  private PoseHighSpeedNoImpactDeadline() {}

  static Deadline fromDecision(PoseTriggerController.Decision decision) {
    return new Deadline(
        decision.activeUntilNs().orElseThrow(
            () -> new IllegalArgumentException("pose decision has no active evidence deadline")),
        decision.thermalHardStopNs().orElseThrow(
            () -> new IllegalArgumentException("pose decision has no thermal hard deadline")));
  }

  static Deadline externalArm(long nowNs, PoseTriggerController.Config config) {
    if (nowNs < 0) {
      throw new IllegalArgumentException("arm time cannot be negative");
    }
    return new Deadline(
        saturatedAdd(nowNs, config.maximumArmedDurationNs()),
        saturatedAdd(nowNs, config.thermalHardCapNs()));
  }

  private static long saturatedAdd(long value, long increment) {
    return value > Long.MAX_VALUE - increment ? Long.MAX_VALUE : value + increment;
  }
}
