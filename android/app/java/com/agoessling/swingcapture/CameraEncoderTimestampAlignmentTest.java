package com.agoessling.swingcapture;

import java.util.List;

/** Host-side regression tests for Camera2/MediaCodec timestamp alignment. */
public final class CameraEncoderTimestampAlignmentTest {
  private CameraEncoderTimestampAlignmentTest() {}

  public static void main(String[] arguments) {
    stableDifferentEpochsMapInBothDirections();
    missingFinalEncoderOutputIsRejected();
    unstableOffsetsAreRejected();
    nonmonotonicEncoderTimestampsAreRejected();
  }

  private static void stableDifferentEpochsMapInBothDirections() {
    List<Long> encoderUs = List.of(10_000L, 14_167L, 18_333L, 22_500L, 26_667L, 30_833L);
    long offsetNanos = 196_104_000_000L;
    List<CameraEncoderTimestampAlignment.CameraSample> camera =
        List.of(
            sample(1, encoderUs, offsetNanos, 250),
            sample(3, encoderUs, offsetNanos, 750),
            sample(5, encoderUs, offsetNanos, 500));

    CameraEncoderTimestampAlignment.Result result =
        CameraEncoderTimestampAlignment.calculate(camera, encoderUs);

    assert result.valid() : result.diagnostic();
    assert result.completeOrdinalCoverage();
    assert result.pairCount() == 3;
    assert result.firstMatchedFrameNumber() == 1;
    assert result.lastMatchedFrameNumber() == 5;
    assert result.encoderToSensorOffsetNanos() == offsetNanos + 500;
    assert result.offsetSpanNanos() == 500;
    assert result.sensorTimestampNanos(22_500L) == offsetNanos + 22_500_000L + 500;
    assert result.encoderPresentationTimeUs(offsetNanos + 22_500_999L + 500) == 22_500L;
  }

  private static void missingFinalEncoderOutputIsRejected() {
    List<Long> encoderUs = List.of(1_000L, 2_000L, 3_000L, 4_000L);
    CameraEncoderTimestampAlignment.Result result =
        CameraEncoderTimestampAlignment.calculate(
            List.of(sample(1, encoderUs, 50_000L, 0), sample(2, encoderUs, 50_000L, 0)),
            encoderUs);
    assert !result.valid();
    assert !result.completeOrdinalCoverage();
  }

  private static void unstableOffsetsAreRejected() {
    List<Long> encoderUs = List.of(1_000L, 2_000L, 3_000L);
    CameraEncoderTimestampAlignment.Result result =
        CameraEncoderTimestampAlignment.calculate(
            List.of(sample(1, encoderUs, 50_000L, 0), sample(2, encoderUs, 2_000_000L, 0)),
            encoderUs);
    assert !result.valid();
    assert result.completeOrdinalCoverage();
    assert result.offsetSpanNanos() > 1_000_000L;
  }

  private static void nonmonotonicEncoderTimestampsAreRejected() {
    List<Long> encoderUs = List.of(1_000L, 1_000L, 3_000L);
    CameraEncoderTimestampAlignment.Result result =
        CameraEncoderTimestampAlignment.calculate(
            List.of(sample(1, encoderUs, 50_000L, 0), sample(2, encoderUs, 50_000L, 0)),
            encoderUs);
    assert !result.valid();
  }

  private static CameraEncoderTimestampAlignment.CameraSample sample(
      int frameNumber, List<Long> encoderUs, long offsetNanos, long jitterNanos) {
    return new CameraEncoderTimestampAlignment.CameraSample(
        frameNumber, encoderUs.get(frameNumber) * 1_000 + offsetNanos + jitterNanos);
  }
}
