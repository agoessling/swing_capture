package com.agoessling.swingcapture;

/** Decides whether an encoded-timeline discontinuity is recoverable during camera warmup. */
public final class CaptureStartupContinuityPolicy {
  public enum Action {
    RESET_WARMUP_AND_CONTINUE,
    FAIL_CAPTURE
  }

  private CaptureStartupContinuityPolicy() {}

  public static Action action(
      boolean recoverableSensorTimestampGap,
      boolean fullPreRollReady,
      boolean retentionCaptureActive) {
    return recoverableSensorTimestampGap && !fullPreRollReady && !retentionCaptureActive
        ? Action.RESET_WARMUP_AND_CONTINUE
        : Action.FAIL_CAPTURE;
  }
}
