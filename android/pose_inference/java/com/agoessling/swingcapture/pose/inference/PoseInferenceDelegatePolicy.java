package com.agoessling.swingcapture.pose.inference;

/** Explicit delegate policy; no device model is silently penalized for compatibility. */
public enum PoseInferenceDelegatePolicy {
  CPU_ONLY,
  GPU_PREFERRED,
  GPU_REQUIRED
}
