package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Maps MediaCodec presentation timestamps onto Camera2's REALTIME sensor clock. */
public final class CameraEncoderTimestampAlignment {
  private static final long MAXIMUM_OFFSET_SPAN_NANOS = 1_000_000;

  /** One Camera2 result and the frame number assigned by the capture session. */
  public static final class CameraSample {
    private final long frameNumber;
    private final long sensorTimestampNanos;

    public CameraSample(long frameNumber, long sensorTimestampNanos) {
      this.frameNumber = frameNumber;
      this.sensorTimestampNanos = sensorTimestampNanos;
    }

    public long frameNumber() {
      return frameNumber;
    }

    public long sensorTimestampNanos() {
      return sensorTimestampNanos;
    }
  }

  /** A measured ordinal mapping, or a diagnostic explaining why no mapping is safe. */
  public static final class Result {
    private final boolean valid;
    private final String diagnostic;
    private final long encoderToSensorOffsetNanos;
    private final long offsetSpanNanos;
    private final int pairCount;
    private final long firstMatchedFrameNumber;
    private final long lastMatchedFrameNumber;
    private final boolean completeOrdinalCoverage;

    private Result(
        boolean valid,
        String diagnostic,
        long encoderToSensorOffsetNanos,
        long offsetSpanNanos,
        int pairCount,
        long firstMatchedFrameNumber,
        long lastMatchedFrameNumber,
        boolean completeOrdinalCoverage) {
      this.valid = valid;
      this.diagnostic = diagnostic;
      this.encoderToSensorOffsetNanos = encoderToSensorOffsetNanos;
      this.offsetSpanNanos = offsetSpanNanos;
      this.pairCount = pairCount;
      this.firstMatchedFrameNumber = firstMatchedFrameNumber;
      this.lastMatchedFrameNumber = lastMatchedFrameNumber;
      this.completeOrdinalCoverage = completeOrdinalCoverage;
    }

    public boolean valid() {
      return valid;
    }

    public String diagnostic() {
      return diagnostic;
    }

    public long encoderToSensorOffsetNanos() {
      return encoderToSensorOffsetNanos;
    }

    public long offsetSpanNanos() {
      return offsetSpanNanos;
    }

    public int pairCount() {
      return pairCount;
    }

    public long firstMatchedFrameNumber() {
      return firstMatchedFrameNumber;
    }

    public long lastMatchedFrameNumber() {
      return lastMatchedFrameNumber;
    }

    public boolean completeOrdinalCoverage() {
      return completeOrdinalCoverage;
    }

    public long sensorTimestampNanos(long encoderPresentationTimeUs) {
      requireValid();
      return Math.addExact(
          Math.multiplyExact(encoderPresentationTimeUs, 1_000), encoderToSensorOffsetNanos);
    }

    public long encoderPresentationTimeUs(long sensorTimestampNanos) {
      requireValid();
      return Math.floorDiv(
          Math.subtractExact(sensorTimestampNanos, encoderToSensorOffsetNanos), 1_000);
    }

    private void requireValid() {
      if (!valid) {
        throw new IllegalStateException("Timestamp alignment is invalid: " + diagnostic);
      }
    }
  }

  private CameraEncoderTimestampAlignment() {}

  public static Result calculate(
      List<CameraSample> cameraSamples, List<Long> encoderPresentationTimesUs) {
    if (cameraSamples.size() < 2) {
      return invalid("fewer than two Camera2 timestamp samples");
    }
    if (encoderPresentationTimesUs.size() < 2) {
      return invalid("fewer than two encoder presentation timestamps");
    }

    long previousEncoderTimestamp = encoderPresentationTimesUs.get(0);
    for (int index = 1; index < encoderPresentationTimesUs.size(); ++index) {
      long timestamp = encoderPresentationTimesUs.get(index);
      if (timestamp <= previousEncoderTimestamp) {
        return invalid("encoder presentation timestamps are not strictly increasing");
      }
      previousEncoderTimestamp = timestamp;
    }

    List<Long> offsets = new ArrayList<>(cameraSamples.size());
    long previousFrameNumber = -1;
    try {
      for (CameraSample sample : cameraSamples) {
        long frameNumber = sample.frameNumber();
        if (frameNumber <= previousFrameNumber) {
          return invalid("Camera2 result frame numbers are not strictly increasing");
        }
        if (frameNumber < 0 || frameNumber >= encoderPresentationTimesUs.size()) {
          return invalid("Camera2 result frame number is outside the encoder output range");
        }
        long encoderTimestampNanos =
            Math.multiplyExact(encoderPresentationTimesUs.get((int) frameNumber), 1_000);
        offsets.add(Math.subtractExact(sample.sensorTimestampNanos(), encoderTimestampNanos));
        previousFrameNumber = frameNumber;
      }
    } catch (ArithmeticException overflow) {
      return invalid("timestamp arithmetic overflow");
    }

    List<Long> sortedOffsets = new ArrayList<>(offsets);
    Collections.sort(sortedOffsets);
    long minimumOffset = sortedOffsets.get(0);
    long maximumOffset = sortedOffsets.get(sortedOffsets.size() - 1);
    long offsetSpan;
    try {
      offsetSpan = Math.subtractExact(maximumOffset, minimumOffset);
    } catch (ArithmeticException overflow) {
      return invalid("timestamp offset span overflow");
    }
    long firstFrameNumber = cameraSamples.get(0).frameNumber();
    long lastFrameNumber = cameraSamples.get(cameraSamples.size() - 1).frameNumber();
    boolean completeOrdinalCoverage = lastFrameNumber + 1 == encoderPresentationTimesUs.size();
    if (!completeOrdinalCoverage) {
      return new Result(
          false,
          "the final Camera2 frame number does not account for every encoder output",
          sortedOffsets.get(sortedOffsets.size() / 2),
          offsetSpan,
          cameraSamples.size(),
          firstFrameNumber,
          lastFrameNumber,
          false);
    }
    if (offsetSpan > MAXIMUM_OFFSET_SPAN_NANOS) {
      return new Result(
          false,
          "Camera2-to-encoder timestamp offset is not stable",
          sortedOffsets.get(sortedOffsets.size() / 2),
          offsetSpan,
          cameraSamples.size(),
          firstFrameNumber,
          lastFrameNumber,
          true);
    }
    return new Result(
        true,
        "ordinal Camera2-to-encoder timestamp mapping is stable",
        sortedOffsets.get(sortedOffsets.size() / 2),
        offsetSpan,
        cameraSamples.size(),
        firstFrameNumber,
        lastFrameNumber,
        true);
  }

  private static Result invalid(String diagnostic) {
    return new Result(false, diagnostic, 0, 0, 0, -1, -1, false);
  }
}
