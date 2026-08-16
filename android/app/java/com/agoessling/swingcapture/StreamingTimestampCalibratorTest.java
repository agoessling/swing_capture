package com.agoessling.swingcapture;

/** Host tests for asynchronous streaming timestamp correlation. */
public final class StreamingTimestampCalibratorTest {
  private StreamingTimestampCalibratorTest() {}

  public static void main(String[] arguments) {
    correlatesEitherArrivalOrder();
    rejectsDiscontinuity();
    startupOutlierDoesNotDelayCoherentCluster();
    isolatedOutlierDoesNotInvalidateEstablishedMapping();
    validSamplesResetEstablishedOutlierRun();
    sustainedOutliersFailInsteadOfLeavingStaleMapping();
    rollingEvidenceDoesNotGrow();
    diagnosticSnapshotPreservesExactAsynchronousEvidence();
    diagnosticSnapshotIsBoundedAcrossRingWrap();
  }

  private static void correlatesEitherArrivalOrder() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    long offset = 196_000_000_000L;
    mapping.observeCamera(0, offset + 1_000_000L);
    mapping.observeEncoder(0, 1_000);
    mapping.observeEncoder(1, 5_167);
    mapping.observeCamera(1, offset + 5_167_500L);
    assert mapping.valid();
    assert mapping.evidenceCount() == 2;
    assert mapping.offsetSpanNanos() == 500;
    assert mapping.sensorTimestampNanos(9_333) == offset + 9_333_500L;
  }

  private static void rejectsDiscontinuity() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    mapping.observeEncoder(0, 1_000);
    try {
      mapping.observeEncoder(2, 9_333);
      throw new AssertionError("ordinal gap was accepted");
    } catch (IllegalStateException expected) {
      // Expected.
    }
  }

  private static void startupOutlierDoesNotDelayCoherentCluster() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    long coherentOffset = 80_000_000L;
    mapping.observeEncoder(0, 1_000);
    mapping.observeCamera(0, 5_001_000L);
    assert !mapping.valid();

    mapping.observeEncoder(1, 5_167);
    mapping.observeCamera(1, coherentOffset + 5_167_000L);
    assert !mapping.valid();
    assert mapping.evidenceCount() == 1;

    mapping.observeEncoder(2, 9_334);
    mapping.observeCamera(2, coherentOffset + 9_334_300L);
    assert mapping.valid();
    assert mapping.evidenceCount() == 2;
    assert mapping.offsetSpanNanos() == 300;
  }

  private static void isolatedOutlierDoesNotInvalidateEstablishedMapping() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    long coherentOffset = 80_000_000L;
    mapping.observeEncoder(0, 1_000);
    mapping.observeCamera(0, coherentOffset + 1_000_000L);
    mapping.observeEncoder(1, 5_167);
    mapping.observeCamera(1, coherentOffset + 5_167_300L);
    assert mapping.valid();

    mapping.observeEncoder(2, 9_334);
    mapping.observeCamera(2, coherentOffset + 13_501_000L);
    assert mapping.valid();
    assert mapping.evidenceCount() == 2;
    assert mapping.offsetSpanNanos() == 300;

    mapping.observeEncoder(3, 13_501);
    mapping.observeCamera(3, coherentOffset + 13_501_500L);
    assert mapping.valid();
    assert mapping.evidenceCount() == 3;
    assert mapping.offsetSpanNanos() == 500;
  }

  private static void validSamplesResetEstablishedOutlierRun() {
    StreamingTimestampCalibrator mapping = establishedMapping();
    long coherentOffset = 80_000_000L;
    for (int ordinal = 2; ordinal < 8; ++ordinal) {
      long ptsUs = 1_000L + ordinal * 4_167L;
      mapping.observeEncoder(ordinal, ptsUs);
      long mismatch = ordinal % 2 == 0 ? 2_000_000L : 0L;
      mapping.observeCamera(ordinal, coherentOffset + ptsUs * 1_000L + mismatch);
      assert mapping.valid();
    }
    assert mapping.evidenceCount() == 5;
  }

  private static void sustainedOutliersFailInsteadOfLeavingStaleMapping() {
    StreamingTimestampCalibrator mapping = establishedMapping();
    long coherentOffset = 80_000_000L;
    for (int ordinal = 2; ordinal <= 4; ++ordinal) {
      long ptsUs = 1_000L + ordinal * 4_167L;
      mapping.observeEncoder(ordinal, ptsUs);
      try {
        mapping.observeCamera(ordinal, coherentOffset + ptsUs * 1_000L + 2_000_000L);
        if (ordinal == 4) {
          throw new AssertionError("sustained timestamp-correlation shift was ignored");
        }
      } catch (IllegalStateException expected) {
        assert ordinal == 4;
        assert expected.getMessage().contains("3 consecutive outliers");
      }
    }
  }

  private static StreamingTimestampCalibrator establishedMapping() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    long coherentOffset = 80_000_000L;
    mapping.observeEncoder(0, 1_000);
    mapping.observeCamera(0, coherentOffset + 1_000_000L);
    mapping.observeEncoder(1, 5_167);
    mapping.observeCamera(1, coherentOffset + 5_167_300L);
    assert mapping.valid();
    return mapping;
  }

  private static void rollingEvidenceDoesNotGrow() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    for (int ordinal = 0; ordinal < 300; ++ordinal) {
      long ptsUs = 1_000L + ordinal * 4_167L;
      mapping.observeEncoder(ordinal, ptsUs);
      mapping.observeCamera(ordinal, 50_000L + ptsUs * 1_000L);
    }
    assert mapping.evidenceCount() == 64;
    assert mapping.valid();
  }

  private static void diagnosticSnapshotPreservesExactAsynchronousEvidence() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    for (int ordinal = 0; ordinal <= 4; ++ordinal) {
      mapping.observeEncoder(ordinal, 1_000 + ordinal * 4_167L);
    }
    mapping.observeCamera(0, 10_001_000);
    mapping.observeCamera(1, 14_168_000);
    mapping.observeCamera(3, 22_502_000);
    mapping.observeCamera(9, 47_504_000);

    StreamingTimestampCalibrator.DiagnosticSnapshot snapshot = mapping.diagnosticSnapshot(2);
    assert snapshot.requestedEncoderOrdinal() == 2;
    assert snapshot.nearestEncoderSamples().size() == 5;
    assert snapshot.nearestEncoderSamples().get(2).ordinal() == 2;
    assert snapshot.nearestEncoderSamples().get(2).presentationTimeUs() == 9_334;
    assert snapshot.nearestCameraSamples().size() == 4;
    assert snapshot.nearestCameraSamples().get(1).frameNumber() == 1;
    assert snapshot.nearestCameraSamples().get(2).frameNumber() == 3;
    assert snapshot.latestEncoderSample().ordinal() == 4;
    assert snapshot.latestEncoderSample().presentationTimeUs() == 17_668;
    assert snapshot.latestCameraSample().frameNumber() == 9;
    assert snapshot.latestCameraSample().sensorTimestampNs() == 47_504_000;
  }

  private static void diagnosticSnapshotIsBoundedAcrossRingWrap() {
    StreamingTimestampCalibrator mapping = new StreamingTimestampCalibrator();
    for (int ordinal = 0; ordinal < 300; ++ordinal) {
      long ptsUs = 1_000L + ordinal * 4_167L;
      mapping.observeEncoder(ordinal, ptsUs);
      mapping.observeCamera(ordinal, 50_000L + ptsUs * 1_000L);
    }

    StreamingTimestampCalibrator.DiagnosticSnapshot snapshot = mapping.diagnosticSnapshot(294);
    assert snapshot.nearestEncoderSamples().size()
        == StreamingTimestampCalibrator.DIAGNOSTIC_SAMPLE_CAPACITY;
    assert snapshot.nearestCameraSamples().size()
        == StreamingTimestampCalibrator.DIAGNOSTIC_SAMPLE_CAPACITY;
    assert snapshot.nearestEncoderSamples().get(0).ordinal() == 291;
    assert snapshot.nearestEncoderSamples().get(5).ordinal() == 296;
    assert snapshot.nearestCameraSamples().get(0).frameNumber() == 291;
    assert snapshot.nearestCameraSamples().get(5).frameNumber() == 296;
    assert snapshot.latestEncoderSample().ordinal() == 299;
    assert snapshot.latestCameraSample().frameNumber() == 299;
  }
}
