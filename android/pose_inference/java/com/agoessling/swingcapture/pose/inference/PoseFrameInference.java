package com.agoessling.swingcapture.pose.inference;

import com.agoessling.swingcapture.pose.PoseLandmarkFrame;

/** Model boundary used by both MediaPipe and deterministic host fakes. */
public interface PoseFrameInference extends AutoCloseable {
  PoseInferenceDelegate actualDelegate();

  PoseLandmarkFrame infer(RgbFrame frame);

  @Override
  void close();
}
