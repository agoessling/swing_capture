package com.agoessling.swingcapture;

import com.agoessling.swingcapture.StreamingTimestampCalibrator.CameraSample;
import com.agoessling.swingcapture.StreamingTimestampCalibrator.DiagnosticSnapshot;
import com.agoessling.swingcapture.StreamingTimestampCalibrator.EncoderSample;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.AccessUnitTiming;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.ContinuityDiagnostic;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention.RetentionException;
import java.util.Objects;
import java.util.Optional;

/** Structured retention and Camera2/encoder evidence for one rejected access-unit boundary. */
public record ContinuousCaptureContinuityDiagnostic(
    EncodedAccessUnitRetention.Failure failure,
    long captureId,
    AccessUnitTiming previousAccessUnit,
    AccessUnitTiming currentAccessUnit,
    long maximumMappedSensorTimestampGapNs,
    DiagnosticSnapshot timestampCorrelation) {
  public ContinuousCaptureContinuityDiagnostic {
    Objects.requireNonNull(failure, "failure");
    Objects.requireNonNull(previousAccessUnit, "previousAccessUnit");
    Objects.requireNonNull(currentAccessUnit, "currentAccessUnit");
    Objects.requireNonNull(timestampCorrelation, "timestampCorrelation");
    if (captureId < 0 || maximumMappedSensorTimestampGapNs <= 0) {
      throw new IllegalArgumentException("capture ID and maximum timestamp gap are invalid");
    }
    if (timestampCorrelation.requestedEncoderOrdinal() != currentAccessUnit.ordinal()) {
      throw new IllegalArgumentException("timestamp snapshot targets a different access unit");
    }
  }

  public static Optional<ContinuousCaptureContinuityDiagnostic> from(
      RetentionException failure, DiagnosticSnapshot timestampCorrelation) {
    Objects.requireNonNull(failure, "failure");
    Objects.requireNonNull(timestampCorrelation, "timestampCorrelation");
    Optional<ContinuityDiagnostic> continuity = failure.continuityDiagnostic();
    if (continuity.isEmpty()) {
      return Optional.empty();
    }
    ContinuityDiagnostic diagnostic = continuity.orElseThrow();
    return Optional.of(
        new ContinuousCaptureContinuityDiagnostic(
            failure.failure(),
            failure.captureId(),
            diagnostic.previous(),
            diagnostic.current(),
            diagnostic.maximumSensorTimestampGapNs(),
            timestampCorrelation));
  }

  public long ordinalDelta() {
    return currentAccessUnit.ordinal() - previousAccessUnit.ordinal();
  }

  public long presentationTimestampGapUs() {
    return currentAccessUnit.presentationTimeUs() - previousAccessUnit.presentationTimeUs();
  }

  public long mappedSensorTimestampGapNs() {
    return currentAccessUnit.sensorTimestampNs() - previousAccessUnit.sensorTimestampNs();
  }

  /** Canonical JSON with 64-bit timing values encoded as decimal strings. */
  public String toJson() {
    StringBuilder json = new StringBuilder(2_048);
    json.append('{');
    appendString(json, "failure", failure.name());
    appendString(json, "capture_id", Long.toString(captureId));
    appendAccessUnit(json, "previous_access_unit", previousAccessUnit);
    appendAccessUnit(json, "current_access_unit", currentAccessUnit);
    appendString(json, "ordinal_delta", Long.toString(ordinalDelta()));
    appendString(
        json,
        "presentation_timestamp_gap_us",
        Long.toString(presentationTimestampGapUs()));
    appendString(
        json,
        "mapped_sensor_timestamp_gap_ns",
        Long.toString(mappedSensorTimestampGapNs()));
    appendString(
        json,
        "maximum_mapped_sensor_timestamp_gap_ns",
        Long.toString(maximumMappedSensorTimestampGapNs));
    appendName(json, "timestamp_correlation");
    appendTimestampCorrelation(json, timestampCorrelation);
    json.append('}');
    return json.toString();
  }

  private static void appendAccessUnit(
      StringBuilder json, String name, AccessUnitTiming accessUnit) {
    appendName(json, name);
    json.append('{');
    appendString(json, "ordinal", Long.toString(accessUnit.ordinal()));
    appendString(
        json, "presentation_time_us", Long.toString(accessUnit.presentationTimeUs()));
    appendString(
        json, "mapped_sensor_timestamp_ns", Long.toString(accessUnit.sensorTimestampNs()));
    json.append('}');
  }

  private static void appendTimestampCorrelation(
      StringBuilder json, DiagnosticSnapshot correlation) {
    json.append('{');
    appendString(
        json,
        "requested_encoder_ordinal",
        Long.toString(correlation.requestedEncoderOrdinal()));
    appendName(json, "nearest_encoder_samples");
    json.append('[');
    for (int index = 0; index < correlation.nearestEncoderSamples().size(); ++index) {
      if (index > 0) {
        json.append(',');
      }
      appendEncoderSample(json, correlation.nearestEncoderSamples().get(index));
    }
    json.append(']');
    appendName(json, "nearest_camera2_samples");
    json.append('[');
    for (int index = 0; index < correlation.nearestCameraSamples().size(); ++index) {
      if (index > 0) {
        json.append(',');
      }
      appendCameraSample(json, correlation.nearestCameraSamples().get(index));
    }
    json.append(']');
    appendName(json, "latest_encoder_sample");
    if (correlation.latestEncoderSample() == null) {
      json.append("null");
    } else {
      appendEncoderSample(json, correlation.latestEncoderSample());
    }
    appendName(json, "latest_camera2_sample");
    if (correlation.latestCameraSample() == null) {
      json.append("null");
    } else {
      appendCameraSample(json, correlation.latestCameraSample());
    }
    json.append('}');
  }

  private static void appendEncoderSample(StringBuilder json, EncoderSample sample) {
    json.append('{');
    appendString(json, "ordinal", Long.toString(sample.ordinal()));
    appendString(json, "presentation_time_us", Long.toString(sample.presentationTimeUs()));
    json.append('}');
  }

  private static void appendCameraSample(StringBuilder json, CameraSample sample) {
    json.append('{');
    appendString(json, "frame_number", Long.toString(sample.frameNumber()));
    appendString(json, "sensor_timestamp_ns", Long.toString(sample.sensorTimestampNs()));
    json.append('}');
  }

  private static void appendString(StringBuilder json, String name, String value) {
    appendName(json, name);
    json.append('"').append(value).append('"');
  }

  private static void appendName(StringBuilder json, String name) {
    char last = json.charAt(json.length() - 1);
    if (last != '{' && last != '[') {
      json.append(',');
    }
    json.append('"').append(name).append("\":");
  }
}
