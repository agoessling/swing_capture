package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.CaptureRuntime;

/** Deterministic host tests for configuration mutation admission. */
public final class CaptureConfigurationPolicyTest {
  private CaptureConfigurationPolicyTest() {}

  public static void main(String[] args) {
    check(CaptureConfigurationPolicy.mayChange(CaptureRuntime.State.STOPPED), "stopped");
    check(CaptureConfigurationPolicy.mayChange(CaptureRuntime.State.ERROR), "error recovery");
    check(!CaptureConfigurationPolicy.mayChange(CaptureRuntime.State.STARTING), "starting");
    check(!CaptureConfigurationPolicy.mayChange(CaptureRuntime.State.ARMED), "armed");
    check(!CaptureConfigurationPolicy.mayChange(CaptureRuntime.State.POSTROLL), "post-roll");
    check(!CaptureConfigurationPolicy.mayChange(CaptureRuntime.State.PUBLISHING), "publishing");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
