package com.agoessling.swingcapture.diagnostics;

import java.nio.charset.StandardCharsets;
import java.util.Objects;

/** One immutable 5 Hz observation plus an optional lower-cadence compressed preview frame. */
public final class PreviewEvidence {
  public static final int MAXIMUM_MODEL_ID_BYTES = 128;
  public static final int MAXIMUM_REASON_BYTES = 512;

  public enum ControllerState {
    MONITORING,
    QUALIFYING,
    HIGH_SPEED_REQUESTED,
    HIGH_SPEED_READY,
    COOLDOWN,
  }

  private final long timestampBoottimeNanos;
  private final byte[] compressedFrame;
  private final String modelId;
  private final int imageRotationDegrees;
  private final long inferenceDurationNanos;
  private final double personConfidence;
  private final double addressConfidence;
  private final double motionMagnitude;
  private final boolean hittingRegionOccupied;
  private final ControllerState controllerState;
  private final String decisionReason;

  public PreviewEvidence(
      long timestampBoottimeNanos,
      byte[] compressedFrame,
      String modelId,
      long inferenceDurationNanos,
      double personConfidence,
      double addressConfidence,
      double motionMagnitude,
      boolean hittingRegionOccupied,
      ControllerState controllerState,
      String decisionReason) {
    this(
        timestampBoottimeNanos,
        compressedFrame,
        modelId,
        0,
        inferenceDurationNanos,
        personConfidence,
        addressConfidence,
        motionMagnitude,
        hittingRegionOccupied,
        controllerState,
        decisionReason);
  }

  public PreviewEvidence(
      long timestampBoottimeNanos,
      byte[] compressedFrame,
      String modelId,
      int imageRotationDegrees,
      long inferenceDurationNanos,
      double personConfidence,
      double addressConfidence,
      double motionMagnitude,
      boolean hittingRegionOccupied,
      ControllerState controllerState,
      String decisionReason) {
    if (timestampBoottimeNanos < 0 || timestampBoottimeNanos == Long.MAX_VALUE) {
      throw new IllegalArgumentException("preview timestamp must permit a positive exclusive end");
    }
    Objects.requireNonNull(compressedFrame, "compressedFrame");
    requireBoundedText(modelId, MAXIMUM_MODEL_ID_BYTES, "modelId", false);
    if (imageRotationDegrees != 0
        && imageRotationDegrees != 90
        && imageRotationDegrees != 180
        && imageRotationDegrees != 270) {
      throw new IllegalArgumentException("imageRotationDegrees must be 0, 90, 180, or 270");
    }
    if (inferenceDurationNanos < 0) {
      throw new IllegalArgumentException("inference duration must be nonnegative");
    }
    requireUnitInterval(personConfidence, "personConfidence");
    requireUnitInterval(addressConfidence, "addressConfidence");
    if (!Double.isFinite(motionMagnitude) || motionMagnitude < 0.0) {
      throw new IllegalArgumentException("motionMagnitude must be finite and nonnegative");
    }
    Objects.requireNonNull(controllerState, "controllerState");
    requireBoundedText(decisionReason, MAXIMUM_REASON_BYTES, "decisionReason", true);
    this.timestampBoottimeNanos = timestampBoottimeNanos;
    this.compressedFrame = compressedFrame.clone();
    this.modelId = modelId;
    this.imageRotationDegrees = imageRotationDegrees;
    this.inferenceDurationNanos = inferenceDurationNanos;
    this.personConfidence = personConfidence;
    this.addressConfidence = addressConfidence;
    this.motionMagnitude = motionMagnitude;
    this.hittingRegionOccupied = hittingRegionOccupied;
    this.controllerState = controllerState;
    this.decisionReason = decisionReason;
  }

  public long timestampBoottimeNanos() {
    return timestampBoottimeNanos;
  }

  public int compressedFrameBytes() {
    return compressedFrame.length;
  }

  public boolean hasCompressedFrame() {
    return compressedFrame.length > 0;
  }

  /** Returns the same observation with a detached encoded frame attached. */
  public PreviewEvidence withCompressedFrame(byte[] frame) {
    Objects.requireNonNull(frame, "frame");
    if (frame.length == 0) {
      throw new IllegalArgumentException("attached preview frame must be nonempty");
    }
    return new PreviewEvidence(
        timestampBoottimeNanos,
        frame,
        modelId,
        imageRotationDegrees,
        inferenceDurationNanos,
        personConfidence,
        addressConfidence,
        motionMagnitude,
        hittingRegionOccupied,
        controllerState,
        decisionReason);
  }

  /** Returns a detached copy; callers cannot mutate retained or snapshotted evidence. */
  public byte[] copyCompressedFrame() {
    return compressedFrame.clone();
  }

  public String modelId() {
    return modelId;
  }

  /** Camera2 rotation applied by MediaPipe to this exact inference input. */
  public int imageRotationDegrees() {
    return imageRotationDegrees;
  }

  public long inferenceDurationNanos() {
    return inferenceDurationNanos;
  }

  public double personConfidence() {
    return personConfidence;
  }

  public double addressConfidence() {
    return addressConfidence;
  }

  public double motionMagnitude() {
    return motionMagnitude;
  }

  public boolean hittingRegionOccupied() {
    return hittingRegionOccupied;
  }

  public ControllerState controllerState() {
    return controllerState;
  }

  public String decisionReason() {
    return decisionReason;
  }

  private static void requireUnitInterval(double value, String name) {
    if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
      throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
    }
  }

  private static void requireBoundedText(
      String value, int maximumBytes, String name, boolean allowEmpty) {
    Objects.requireNonNull(value, name);
    if ((!allowEmpty && value.isEmpty())
        || value.getBytes(StandardCharsets.UTF_8).length > maximumBytes) {
      throw new IllegalArgumentException(name + " has an invalid UTF-8 length");
    }
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      if (Character.isSurrogate(character)) {
        if (!Character.isHighSurrogate(character)
            || index + 1 >= value.length()
            || !Character.isLowSurrogate(value.charAt(index + 1))) {
          throw new IllegalArgumentException(name + " contains an unpaired surrogate");
        }
        ++index;
      } else if (character < 0x20 && character != '\n' && character != '\r' && character != '\t') {
        throw new IllegalArgumentException(name + " contains a prohibited control character");
      }
    }
  }
}
