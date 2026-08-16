package com.agoessling.swingcapture;

import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.RetentionException;
import java.nio.ByteBuffer;

/** Pure regression tests for retention-to-HIL continuity evidence and canonical report JSON. */
public final class ContinuousCaptureContinuityDiagnosticTest {
  private ContinuousCaptureContinuityDiagnosticTest() {}

  public static void main(String[] arguments) throws Exception {
    mapsTypedRetentionFailureAndExactCorrelation();
    resetFailureIsSuppressedBehindOriginalContinuityFailure();
    noncontinuityFailureHasNoSyntheticEvidence();
  }

  private static void mapsTypedRetentionFailureAndExactCorrelation() throws Exception {
    RetentionException retentionFailure = sensorGapFailure();
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    mapping.observeEncoder(0, 1_000);
    mapping.observeEncoder(1, 5_167);
    mapping.observeEncoder(2, 9_334);
    mapping.observeEncoder(3, 13_501);
    mapping.observeCamera(0, 5_001_000);
    mapping.observeCamera(1, 9_168_000);
    mapping.observeCamera(3, 17_502_000);
    mapping.observeCamera(8, 38_337_000);

    ContinuousCaptureContinuityException failure =
        new ContinuousCaptureContinuityException(
            retentionFailure, mapping.diagnosticSnapshot(3));
    ContinuousCaptureContinuityDiagnostic diagnostic = failure.diagnostic();
    check(
        failure.getMessage().equals(retentionFailure.getMessage()),
        "ordinary retention error text is preserved");
    check(failure.getCause() == retentionFailure, "typed retention failure remains the cause");
    check(
        diagnostic.failure() == EncodedAccessUnitRetention.Failure.SENSOR_TIMESTAMP_GAP,
        "failure enum");
    check(diagnostic.previousAccessUnit().ordinal() == 2, "previous access unit");
    check(diagnostic.currentAccessUnit().ordinal() == 3, "current access unit");
    check(diagnostic.ordinalDelta() == 1, "ordinal delta");
    check(diagnostic.presentationTimestampGapUs() == 4_167, "PTS delta");
    check(diagnostic.mappedSensorTimestampGapNs() == 20_001_000, "mapped sensor delta");
    check(diagnostic.maximumMappedSensorTimestampGapNs() == 20_000_000, "20 ms gate");

    String json = diagnostic.toJson();
    check(json.contains("\"failure\":\"SENSOR_TIMESTAMP_GAP\""), "failure JSON");
    check(
        json.contains(
            "\"previous_access_unit\":{\"ordinal\":\"2\","
                + "\"presentation_time_us\":\"30167\","
                + "\"mapped_sensor_timestamp_ns\":\"25167000\"}"),
        "previous mapped AU JSON");
    check(
        json.contains("\"mapped_sensor_timestamp_gap_ns\":\"20001000\""),
        "mapped gap JSON");
    check(
        json.contains("\"maximum_mapped_sensor_timestamp_gap_ns\":\"20000000\""),
        "maximum JSON");
    check(
        json.contains(
            "\"nearest_camera2_samples\":[{\"frame_number\":\"0\","
                + "\"sensor_timestamp_ns\":\"5001000\"}"),
        "raw Camera2 JSON");
    check(
        json.contains(
            "\"latest_camera2_sample\":{\"frame_number\":\"8\","
                + "\"sensor_timestamp_ns\":\"38337000\"}"),
        "latest Camera2 JSON");
  }

  private static void resetFailureIsSuppressedBehindOriginalContinuityFailure()
      throws Exception {
    RetentionException appendFailure = sensorGapFailure();
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    mapping.observeEncoder(0, 1_000);
    mapping.observeEncoder(1, 5_167);
    mapping.observeEncoder(2, 9_334);
    mapping.observeEncoder(3, 13_501);

    EncodedAccessUnitRetention activeCapture =
        new EncodedAccessUnitRetention(
            new EncodedAccessUnitRetention.Limits(
                256,
                8,
                100_000_000,
                8,
                1,
                0,
                50_000_000,
                0,
                20_000_000,
                20_000_000));
    activeCapture.append(0, 1_000, 1_000_000, 0, true, ByteBuffer.wrap(new byte[8]));
    check(activeCapture.trigger(1_000_000).accepted(), "test reset has an active capture");

    try {
      ContinuousCaptureContinuityException.resetAndThrow(
          appendFailure, mapping.diagnosticSnapshot(3), activeCapture::resetContinuity);
      throw new AssertionError("expected structured continuity failure");
    } catch (ContinuousCaptureContinuityException structured) {
      check(structured.getCause() == appendFailure, "append failure remains primary cause");
      check(structured.getSuppressed().length == 1, "reset failure is retained");
      check(
          structured.getSuppressed()[0] instanceof RetentionException resetFailure
              && resetFailure.failure()
                  == EncodedAccessUnitRetention.Failure.RESET_DURING_CAPTURE,
          "suppressed reset failure remains typed");
      check(
          structured.getMessage().equals(appendFailure.getMessage()),
          "reset failure cannot replace ordinary error text");
    }
  }

  private static void noncontinuityFailureHasNoSyntheticEvidence() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new EncodedAccessUnitRetention.Limits(
                8, 8, 100_000_000, 8, 1, 0, 0, 0, 20_000_000, 20_000_000));
    RetentionException capacityFailure;
    try {
      retention.append(0, 1_000, 1_000_000, 0, true, ByteBuffer.wrap(new byte[9]));
      throw new AssertionError("expected capacity failure");
    } catch (RetentionException expected) {
      capacityFailure = expected;
    }
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    check(
        ContinuousCaptureContinuityDiagnostic.from(
                capacityFailure, mapping.diagnosticSnapshot(0))
            .isEmpty(),
        "capacity failure has no continuity report");
  }

  private static RetentionException sensorGapFailure() throws Exception {
    EncodedAccessUnitRetention retention =
        new EncodedAccessUnitRetention(
            new EncodedAccessUnitRetention.Limits(
                256,
                8,
                100_000_000,
                8,
                1,
                0,
                0,
                0,
                20_000_000,
                20_000_000));
    retention.append(2, 30_167, 25_167_000, 0, true, ByteBuffer.wrap(new byte[8]));
    try {
      retention.append(3, 34_334, 45_168_000, 0, false, ByteBuffer.wrap(new byte[8]));
      throw new AssertionError("expected sensor timestamp gap");
    } catch (RetentionException expected) {
      return expected;
    }
  }

  private static void check(boolean condition, String label) {
    if (!condition) {
      throw new AssertionError(label);
    }
  }
}
