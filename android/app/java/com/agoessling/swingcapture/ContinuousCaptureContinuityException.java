package com.agoessling.swingcapture;

import com.agoessling.swingcapture.StreamingTimestampCalibrator.DiagnosticSnapshot;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.RetentionException;

/** Preserves the retention error text while carrying app-side timestamp correlation evidence. */
public final class ContinuousCaptureContinuityException extends Exception {
  @FunctionalInterface
  interface ContinuityReset {
    void reset() throws RetentionException;
  }

  private final ContinuousCaptureContinuityDiagnostic diagnostic;

  public ContinuousCaptureContinuityException(
      RetentionException cause, DiagnosticSnapshot timestampCorrelation) {
    super(cause.getMessage(), cause);
    diagnostic =
        ContinuousCaptureContinuityDiagnostic.from(cause, timestampCorrelation)
            .orElseThrow(
                () ->
                    new IllegalArgumentException(
                        "retention failure does not contain continuity diagnostics"));
  }

  public ContinuousCaptureContinuityDiagnostic diagnostic() {
    return diagnostic;
  }

  /** Always rethrows the original append failure, retaining any reset failure as suppressed. */
  static void resetAndThrow(
      RetentionException appendFailure,
      DiagnosticSnapshot timestampCorrelation,
      ContinuityReset reset)
      throws RetentionException, ContinuousCaptureContinuityException {
    ContinuousCaptureContinuityException structuredFailure =
        timestampCorrelation == null
            ? null
            : new ContinuousCaptureContinuityException(appendFailure, timestampCorrelation);
    try {
      reset.reset();
    } catch (RetentionException resetFailure) {
      if (structuredFailure != null) {
        structuredFailure.addSuppressed(resetFailure);
        throw structuredFailure;
      }
      appendFailure.addSuppressed(resetFailure);
      throw appendFailure;
    }
    if (structuredFailure != null) {
      throw structuredFailure;
    }
    throw appendFailure;
  }
}
