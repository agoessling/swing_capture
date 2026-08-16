package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.CaptureRuntime;
import java.util.Objects;

/** Decides whether persistent role/profile configuration may change in a capture state. */
public final class CaptureConfigurationPolicy {
  private CaptureConfigurationPolicy() {}

  public static boolean mayChange(CaptureRuntime.State state) {
    Objects.requireNonNull(state, "state");
    return state == CaptureRuntime.State.STOPPED || state == CaptureRuntime.State.ERROR;
  }
}
