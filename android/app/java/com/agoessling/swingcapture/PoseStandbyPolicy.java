package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import java.util.Objects;

/** Pure mappings shared by the live standby path and its deterministic tests. */
final class PoseStandbyPolicy {
  private PoseStandbyPolicy() {}

  static PoseProjection projectionForRole(CaptureRole role) {
    Objects.requireNonNull(role, "role");
    return switch (role) {
      case DOWN_THE_LINE -> PoseProjection.DOWN_THE_LINE;
      case FACE_ON -> PoseProjection.ACROSS_THE_LINE;
      case UNASSIGNED ->
          throw new IllegalArgumentException("pose standby requires an assigned capture role");
    };
  }

  static PreviewEvidence.ControllerState evidenceState(PoseTriggerController.State state) {
    Objects.requireNonNull(state, "state");
    return switch (state) {
      case WATCHING -> PreviewEvidence.ControllerState.MONITORING;
      case QUALIFYING -> PreviewEvidence.ControllerState.QUALIFYING;
      case ARM_REQUESTED -> PreviewEvidence.ControllerState.HIGH_SPEED_REQUESTED;
      case WAITING_FOR_CLEAR -> PreviewEvidence.ControllerState.COOLDOWN;
    };
  }
}
