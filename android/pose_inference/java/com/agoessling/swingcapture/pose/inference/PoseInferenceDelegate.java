package com.agoessling.swingcapture.pose.inference;

/** Delegate actually selected by the MediaPipe runtime. */
public enum PoseInferenceDelegate {
  CPU,
  GPU,
  NPU
}
