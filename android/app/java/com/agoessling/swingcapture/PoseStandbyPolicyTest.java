package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.PoseTriggerController;

public final class PoseStandbyPolicyTest {
  public static void main(String[] args) {
    check(
        PoseStandbyPolicy.projectionForRole(CaptureRole.DOWN_THE_LINE)
            == PoseProjection.DOWN_THE_LINE,
        "DTL role must use DTL features");
    check(
        PoseStandbyPolicy.projectionForRole(CaptureRole.FACE_ON)
            == PoseProjection.ACROSS_THE_LINE,
        "face-on role must use ATL features");
    expectIllegalArgument(() -> PoseStandbyPolicy.projectionForRole(CaptureRole.UNASSIGNED));

    check(
        PoseStandbyPolicy.evidenceState(PoseTriggerController.State.WATCHING)
            == PreviewEvidence.ControllerState.MONITORING,
        "watching state mismatch");
    check(
        PoseStandbyPolicy.evidenceState(PoseTriggerController.State.QUALIFYING)
            == PreviewEvidence.ControllerState.QUALIFYING,
        "qualifying state mismatch");
    check(
        PoseStandbyPolicy.evidenceState(PoseTriggerController.State.ARM_REQUESTED)
            == PreviewEvidence.ControllerState.HIGH_SPEED_REQUESTED,
        "armed state mismatch");
    check(
        PoseStandbyPolicy.evidenceState(PoseTriggerController.State.WAITING_FOR_CLEAR)
            == PreviewEvidence.ControllerState.COOLDOWN,
        "cooldown state mismatch");
  }

  private static void expectIllegalArgument(Runnable action) {
    try {
      action.run();
      throw new AssertionError("expected IllegalArgumentException");
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
