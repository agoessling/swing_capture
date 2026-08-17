package com.agoessling.swingcapture.pose;

import java.util.EnumMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;

/** One model-independent pose result on the same monotonic clock as camera control. */
public record PoseLandmarkFrame(
    long timestampNs,
    double personConfidence,
    Map<PoseJoint, NormalizedPoseLandmark> landmarks) {
  public PoseLandmarkFrame {
    if (timestampNs < 0) {
      throw new IllegalArgumentException("timestampNs cannot be negative");
    }
    if (!Double.isFinite(personConfidence)
        || personConfidence < 0.0
        || personConfidence > 1.0) {
      throw new IllegalArgumentException("personConfidence must be finite and in [0, 1]");
    }
    Objects.requireNonNull(landmarks, "landmarks");
    EnumMap<PoseJoint, NormalizedPoseLandmark> copy = new EnumMap<>(PoseJoint.class);
    for (Map.Entry<PoseJoint, NormalizedPoseLandmark> entry : landmarks.entrySet()) {
      copy.put(
          Objects.requireNonNull(entry.getKey(), "landmark joint"),
          Objects.requireNonNull(entry.getValue(), "landmark value"));
    }
    landmarks = Map.copyOf(copy);
  }

  public Optional<NormalizedPoseLandmark> landmark(PoseJoint joint) {
    return Optional.ofNullable(landmarks.get(Objects.requireNonNull(joint, "joint")));
  }
}
