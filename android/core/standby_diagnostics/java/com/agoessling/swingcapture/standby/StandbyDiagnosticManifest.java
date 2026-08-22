package com.agoessling.swingcapture.standby;

import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ContinuousAudioImpactDetector;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.Optional;
import java.util.OptionalLong;

/** Strict schema-v1 value contract for one non-reviewable standby diagnostic-only session. */
public record StandbyDiagnosticManifest(
    String sessionId,
    long createdAtEpochMillis,
    String sourceNodeId,
    EventMetadata event,
    AudioArtifact audio,
    PublicationStatus previewStatus,
    Optional<PreviewArtifact> preview,
    String incidentRelativePath,
    int incidentBytes) {
  public static final int SCHEMA_VERSION = 1;
  public static final String SESSION_KIND = "standby_diagnostic";
  public static final String INCIDENT_FILE_NAME = "diagnostic_incident.json";
  public static final String AUDIO_FILE_NAME = "diagnostic_audio.wav";
  public static final String AUDIO_CONTENT_TYPE = "audio/wav";
  public static final String PREVIEW_FRAMES_PATH = "pose_diagnostics/preview_frames.mjpeg";
  public static final String PREVIEW_TRACE_PATH = "pose_diagnostics/pose_trace.ndjson";
  public static final String PREVIEW_FRAME_CONTENT_TYPE = "image/jpeg";
  public static final String PREVIEW_TRACE_CONTENT_TYPE = "application/x-ndjson";
  public static final int WAV_HEADER_BYTES = 44;
  public static final int MAXIMUM_SERIALIZED_BYTES = 128 * 1024;

  public enum PublicationStatus {
    AVAILABLE("available"),
    NOT_AVAILABLE("not_available"),
    PUBLICATION_FAILED("publication_failed");

    private final String wireName;

    PublicationStatus(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }
  }

  public record DetectorEvidence(
      long strikeFramePosition,
      long confirmationFramePosition,
      float peakAmplitude,
      float noiseFloor,
      float threshold,
      Optional<AudioTimestampMapper.Estimate> strikeTime,
      Optional<AudioTimestampMapper.Estimate> confirmationTime) {
    public DetectorEvidence {
      if (strikeFramePosition < 0 || confirmationFramePosition < strikeFramePosition) {
        throw new IllegalArgumentException("standby detector frame positions are invalid");
      }
      requireUnitInterval(peakAmplitude, "peakAmplitude");
      requireUnitInterval(noiseFloor, "noiseFloor");
      requireUnitInterval(threshold, "threshold");
      Objects.requireNonNull(strikeTime, "strikeTime");
      Objects.requireNonNull(confirmationTime, "confirmationTime");
      if (strikeTime.isPresent()
          && strikeTime.orElseThrow().framePosition() != strikeFramePosition) {
        throw new IllegalArgumentException("detector strike time maps the wrong frame");
      }
      if (confirmationTime.isPresent()
          && confirmationTime.orElseThrow().framePosition() != confirmationFramePosition) {
        throw new IllegalArgumentException("detector confirmation time maps the wrong frame");
      }
    }

    private static DetectorEvidence from(
        ContinuousAudioImpactDetector.TimedImpact timedImpact) {
      ImpactDetector.Impact impact = timedImpact.impact();
      return new DetectorEvidence(
          impact.strikeFramePosition(),
          impact.confirmationFramePosition(),
          impact.peakAmplitude(),
          impact.noiseFloorAtDetection(),
          impact.thresholdAtDetection(),
          timedImpact.strikeTime(),
          timedImpact.confirmationTime());
    }
  }

  public record EventMetadata(
      long sequence,
      StandbyDiagnosticCoordinator.EventKind kind,
      long markerFramePosition,
      StandbyDiagnosticCoordinator.AudioClockStatus audioClockStatus,
      Optional<AudioTimestampMapper.Estimate> markerTime,
      OptionalLong operatorReceivedBoottimeNanos,
      Optional<DetectorEvidence> detector) {
    public EventMetadata {
      if (sequence <= 0 || markerFramePosition < 0) {
        throw new IllegalArgumentException("standby event metadata identity is invalid");
      }
      Objects.requireNonNull(kind, "kind");
      Objects.requireNonNull(audioClockStatus, "audioClockStatus");
      Objects.requireNonNull(markerTime, "markerTime");
      Objects.requireNonNull(operatorReceivedBoottimeNanos, "operatorReceivedBoottimeNanos");
      Objects.requireNonNull(detector, "detector");
      if ((audioClockStatus == StandbyDiagnosticCoordinator.AudioClockStatus.VALIDATED)
          != markerTime.isPresent()) {
        throw new IllegalArgumentException("audio clock status disagrees with marker time");
      }
      if (markerTime.isPresent()
          && markerTime.orElseThrow().framePosition() != markerFramePosition) {
        throw new IllegalArgumentException("event marker estimate maps the wrong frame");
      }
      if (kind == StandbyDiagnosticCoordinator.EventKind.DETECTED_IMPACT) {
        if (detector.isEmpty()
            || operatorReceivedBoottimeNanos.isPresent()
            || detector.orElseThrow().strikeFramePosition() != markerFramePosition) {
          throw new IllegalArgumentException("detected event metadata is inconsistent");
        }
      } else if (detector.isPresent()
          || operatorReceivedBoottimeNanos.isEmpty()
          || operatorReceivedBoottimeNanos.orElseThrow() <= 0) {
        throw new IllegalArgumentException("operator event metadata is inconsistent");
      }
    }

    public static EventMetadata from(StandbyDiagnosticCoordinator.EventMarker event) {
      Objects.requireNonNull(event, "event");
      return new EventMetadata(
          event.sequence(),
          event.kind(),
          event.markerFramePosition(),
          event.audioClockStatus(),
          event.markerTime(),
          event.operatorReceivedBoottimeNanos(),
          event.detectedImpact().map(DetectorEvidence::from));
    }
  }

  public record AudioArtifact(
      String relativePath,
      String contentType,
      long bytes,
      int sampleRateHz,
      long firstFramePosition,
      long endFramePosition,
      long markerFramePosition,
      int sampleCount,
      int markerSampleIndex) {
    public AudioArtifact {
      if (!AUDIO_FILE_NAME.equals(relativePath)
          || !AUDIO_CONTENT_TYPE.equals(contentType)
          || sampleRateHz != StandbyDiagnosticCoordinator.SAMPLE_RATE_HZ
          || firstFramePosition < 0
          || endFramePosition <= firstFramePosition
          || markerFramePosition < firstFramePosition
          || markerFramePosition >= endFramePosition
          || sampleCount != endFramePosition - firstFramePosition
          || markerSampleIndex != markerFramePosition - firstFramePosition
          || bytes != expectedWavBytes(sampleCount)) {
        throw new IllegalArgumentException("standby diagnostic audio artifact is invalid");
      }
    }

    public static long expectedWavBytes(int sampleCount) {
      if (sampleCount <= 0) {
        throw new IllegalArgumentException("standby WAV sample count must be positive");
      }
      return Math.addExact(WAV_HEADER_BYTES, Math.multiplyExact((long) sampleCount, 2));
    }
  }

  public record PreviewArtifact(
      String framesRelativePath,
      String framesContentType,
      long framesBytes,
      String traceRelativePath,
      String traceContentType,
      int traceBytes,
      long firstTimestampInclusive,
      long endTimestampExclusive,
      int observationCount,
      int jpegFrameCount) {
    public PreviewArtifact {
      if (!PREVIEW_FRAMES_PATH.equals(framesRelativePath)
          || !PREVIEW_FRAME_CONTENT_TYPE.equals(framesContentType)
          || framesBytes < 0
          || framesBytes > 32L * 1024 * 1024
          || !PREVIEW_TRACE_PATH.equals(traceRelativePath)
          || !PREVIEW_TRACE_CONTENT_TYPE.equals(traceContentType)
          || traceBytes <= 0
          || traceBytes > 1024 * 1024
          || firstTimestampInclusive < 0
          || endTimestampExclusive <= firstTimestampInclusive
          || observationCount <= 0
          || observationCount > 300
          || jpegFrameCount < 0
          || jpegFrameCount > observationCount
          || ((framesBytes == 0) != (jpegFrameCount == 0))) {
        throw new IllegalArgumentException("standby preview artifact is invalid");
      }
    }
  }

  public StandbyDiagnosticManifest {
    requireIdentifier(sessionId, "sessionId");
    if (createdAtEpochMillis <= 0) {
      throw new IllegalArgumentException("createdAtEpochMillis must be positive");
    }
    requireIdentifier(sourceNodeId, "sourceNodeId");
    Objects.requireNonNull(event, "event");
    Objects.requireNonNull(audio, "audio");
    Objects.requireNonNull(previewStatus, "previewStatus");
    Objects.requireNonNull(preview, "preview");
    if ((previewStatus == PublicationStatus.AVAILABLE) != preview.isPresent()) {
      throw new IllegalArgumentException("preview status disagrees with preview artifact");
    }
    if (!INCIDENT_FILE_NAME.equals(incidentRelativePath)
        || incidentBytes <= 0
        || incidentBytes > DiagnosticIncident.MAXIMUM_SERIALIZED_BYTES
        || audio.markerFramePosition() != event.markerFramePosition()) {
      throw new IllegalArgumentException("standby incident or marker metadata is inconsistent");
    }
  }

  /** Creates the canonical initial incident paired with this diagnostic-only event. */
  public static DiagnosticIncident incidentFor(
      StandbyDiagnosticCoordinator.EventMarker event,
      String sessionId,
      String sourceNodeId,
      long createdAtEpochMillis) {
    Objects.requireNonNull(event, "event");
    DiagnosticIncident.IncidentClassification classification;
    DiagnosticIncident.UserFeedback feedback;
    List<DiagnosticIncident.TimingMark> timingMarks;
    if (event.kind() == StandbyDiagnosticCoordinator.EventKind.DETECTED_IMPACT) {
      classification = DiagnosticIncident.IncidentClassification.IMPACT_WHILE_NOT_ARMED;
      feedback = DiagnosticIncident.UserFeedback.unreviewed();
      timingMarks =
          List.of(
              new DiagnosticIncident.TimingMark(
                  DiagnosticIncident.TimingMarkKind.AUDIO_IMPACT_TRANSIENT,
                  "standby_audio",
                  0));
    } else {
      classification = DiagnosticIncident.IncidentClassification.USER_REPORTED;
      feedback =
          new DiagnosticIncident.UserFeedback(
              DiagnosticIncident.FeedbackClassification.MISSED_SHOT, "");
      timingMarks = List.of();
    }
    return new DiagnosticIncident(
        sessionId,
        classification,
        createdAtEpochMillis,
        sourceNodeId,
        feedback,
        timingMarks);
  }

  /** Canonical compact UTF-8 JSON with stable field ordering and JS-safe 64-bit decimals. */
  public String toCanonicalJson() {
    StringBuilder json = new StringBuilder(2_048);
    json.append("{\"schema_version\":").append(SCHEMA_VERSION);
    appendString(json, "session_kind", SESSION_KIND);
    appendString(json, "session_id", sessionId);
    appendDecimalString(json, "created_at_epoch_ms", createdAtEpochMillis);
    appendString(json, "source_node_id", sourceNodeId);
    json.append(",\"event\":{\"sequence\":\"").append(event.sequence()).append('"');
    appendString(json, "kind", event.kind().wireName());
    appendDecimalString(json, "marker_audio_frame_position", event.markerFramePosition());
    appendString(json, "audio_clock_status", event.audioClockStatus().wireName());
    appendEstimateFields(json, "marker", event.markerTime());
    appendOptionalDecimalString(
        json, "operator_received_boottime_ns", event.operatorReceivedBoottimeNanos());
    json.append(",\"detector\":");
    if (event.detector().isEmpty()) {
      json.append("null");
    } else {
      DetectorEvidence detectorEvidence = event.detector().orElseThrow();
      json.append("{\"strike_frame_position\":\"")
          .append(detectorEvidence.strikeFramePosition())
          .append('"');
      appendDecimalString(
          json, "confirmation_frame_position", detectorEvidence.confirmationFramePosition());
      appendString(json, "peak_amplitude", canonicalFloat(detectorEvidence.peakAmplitude()));
      appendString(json, "noise_floor", canonicalFloat(detectorEvidence.noiseFloor()));
      appendString(json, "threshold", canonicalFloat(detectorEvidence.threshold()));
      appendEstimateFields(json, "strike", detectorEvidence.strikeTime());
      appendEstimateFields(json, "confirmation", detectorEvidence.confirmationTime());
      json.append('}');
    }
    json.append("},\"evidence\":{\"audio\":{");
    appendFirstString(json, "path", audio.relativePath());
    appendString(json, "content_type", audio.contentType());
    appendDecimalString(json, "bytes", audio.bytes());
    json.append(",\"sample_rate_hz\":").append(audio.sampleRateHz());
    appendDecimalString(json, "first_frame_position", audio.firstFramePosition());
    appendDecimalString(json, "end_frame_position", audio.endFramePosition());
    appendDecimalString(json, "marker_frame_position", audio.markerFramePosition());
    json.append(",\"sample_count\":").append(audio.sampleCount());
    json.append(",\"marker_sample_index\":").append(audio.markerSampleIndex()).append('}');
    appendString(json, "preview_status", previewStatus.wireName());
    json.append(",\"preview\":");
    if (preview.isEmpty()) {
      json.append("null");
    } else {
      PreviewArtifact artifact = preview.orElseThrow();
      json.append('{');
      appendFirstString(json, "frames_path", artifact.framesRelativePath());
      appendString(json, "frames_content_type", artifact.framesContentType());
      appendDecimalString(json, "frames_bytes", artifact.framesBytes());
      appendString(json, "trace_path", artifact.traceRelativePath());
      appendString(json, "trace_content_type", artifact.traceContentType());
      json.append(",\"trace_bytes\":").append(artifact.traceBytes());
      appendDecimalString(
          json, "first_timestamp_boottime_ns", artifact.firstTimestampInclusive());
      appendDecimalString(
          json,
          "end_timestamp_boottime_ns_exclusive",
          artifact.endTimestampExclusive());
      json.append(",\"observation_count\":").append(artifact.observationCount());
      json.append(",\"jpeg_frame_count\":").append(artifact.jpegFrameCount());
      json.append(",\"frame_count\":").append(artifact.jpegFrameCount()).append('}');
    }
    json.append("},\"incident\":{");
    appendFirstString(json, "path", incidentRelativePath);
    json.append(",\"bytes\":").append(incidentBytes).append("}}");
    String result = json.toString();
    if (result.getBytes(StandardCharsets.UTF_8).length > MAXIMUM_SERIALIZED_BYTES) {
      throw new IllegalStateException("standby diagnostic manifest exceeds its bound");
    }
    return result;
  }

  private static void appendEstimateFields(
      StringBuilder json, String prefix, Optional<AudioTimestampMapper.Estimate> estimate) {
    if (estimate.isEmpty()) {
      appendNull(json, prefix + "_boottime_ns");
      appendNull(json, prefix + "_uncertainty_ns");
      return;
    }
    AudioTimestampMapper.Estimate value = estimate.orElseThrow();
    appendDecimalString(json, prefix + "_boottime_ns", value.boottimeNanos());
    appendDecimalString(json, prefix + "_uncertainty_ns", value.uncertaintyNanos());
  }

  private static void appendOptionalDecimalString(
      StringBuilder json, String name, OptionalLong value) {
    if (value.isPresent()) {
      appendDecimalString(json, name, value.orElseThrow());
    } else {
      appendNull(json, name);
    }
  }

  private static void appendNull(StringBuilder json, String name) {
    json.append(',');
    appendEscaped(json, name);
    json.append(":null");
  }

  private static void appendDecimalString(StringBuilder json, String name, long value) {
    appendString(json, name, Long.toString(value));
  }

  private static void appendFirstString(StringBuilder json, String name, String value) {
    appendEscaped(json, name);
    json.append(':');
    appendEscaped(json, value);
  }

  private static void appendString(StringBuilder json, String name, String value) {
    json.append(',');
    appendEscaped(json, name);
    json.append(':');
    appendEscaped(json, value);
  }

  private static void appendEscaped(StringBuilder json, String value) {
    json.append('"');
    for (int index = 0; index < value.length(); ++index) {
      char character = value.charAt(index);
      switch (character) {
        case '"' -> json.append("\\\"");
        case '\\' -> json.append("\\\\");
        case '\b' -> json.append("\\b");
        case '\f' -> json.append("\\f");
        case '\n' -> json.append("\\n");
        case '\r' -> json.append("\\r");
        case '\t' -> json.append("\\t");
        default -> {
          if (character < 0x20) {
            json.append(String.format(Locale.ROOT, "\\u%04x", (int) character));
          } else {
            json.append(character);
          }
        }
      }
    }
    json.append('"');
  }

  private static String canonicalFloat(float value) {
    return value == 0.0f ? "0.0" : Float.toString(value);
  }

  private static void requireUnitInterval(float value, String name) {
    if (!Float.isFinite(value) || value < 0.0f || value > 1.0f) {
      throw new IllegalArgumentException(name + " must be finite and in [0, 1]");
    }
  }

  private static void requireIdentifier(String value, String name) {
    Objects.requireNonNull(value, name);
    if (!value.matches("[A-Za-z0-9._-]+")
        || value.getBytes(StandardCharsets.UTF_8).length
            > DiagnosticIncident.MAXIMUM_IDENTIFIER_BYTES) {
      throw new IllegalArgumentException(name + " is invalid");
    }
  }
}
