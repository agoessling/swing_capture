package com.agoessling.swingcapture;

import java.util.List;
import java.util.Objects;

/** Deterministic metadata for one bounded normal-rate field recording. */
public final class FieldRecordingManifest {
  public static final int SCHEMA_VERSION = 1;
  public static final int WIDTH = 1280;
  public static final int HEIGHT = 720;
  public static final int NOMINAL_FRAMES_PER_SECOND = 30;
  public static final int AUDIO_SAMPLE_RATE_HZ = 48_000;
  public static final int MAXIMUM_CAMERA_FRAMES = 36_100;
  public static final int MAXIMUM_ENCODED_SAMPLES = 36_100;
  public static final int MAXIMUM_AUDIO_TIMESTAMPS = 12_100;

  /** One midpoint-sampled transform from CLOCK_MONOTONIC into CLOCK_BOOTTIME. */
  public record ClockAnchor(
      long monotonicBeforeNs, long boottimeNs, long monotonicAfterNs) {
    public ClockAnchor {
      // Validate the raw bracket and all arithmetic before it becomes durable evidence.
      WarmCaptureTransitionTiming.midpointClockOffsetNanos(
          monotonicBeforeNs, boottimeNs, monotonicAfterNs);
    }

    public long boottimeMinusMonotonicNs() {
      return WarmCaptureTransitionTiming.midpointClockOffsetNanos(
          monotonicBeforeNs, boottimeNs, monotonicAfterNs);
    }

    public long bracketNs() {
      return Math.subtractExact(monotonicAfterNs, monotonicBeforeNs);
    }

    /** Maximum midpoint error, conservatively rounded up for an odd-width bracket. */
    public long uncertaintyNs() {
      long bracket = bracketNs();
      return bracket / 2 + bracket % 2;
    }
  }

  /** Exact Camera2 result timestamp. REALTIME camera timestamps use the boottime clock. */
  public record CameraFrame(long frameNumber, long sensorTimestampNs) {
    public CameraFrame {
      if (frameNumber < 0 || sensorTimestampNs <= 0) {
        throw new IllegalArgumentException("camera frame timing is invalid");
      }
    }
  }

  /** Exact encoder output timestamp plus the normalized timestamp written to the MP4. */
  public record EncodedSample(
      int sampleIndex, long sourcePtsUs, long mediaTimeUs, int flags, int bytes) {
    public EncodedSample {
      if (sampleIndex < 0 || sourcePtsUs < 0 || mediaTimeUs < 0 || flags < 0 || bytes <= 0) {
        throw new IllegalArgumentException("encoded sample timing is invalid");
      }
    }
  }

  /** Exact AudioRecord timestamp and the corresponding WAV position observed after one read. */
  public record AudioTimestampObservation(
      long wavEndFramePosition,
      long audioRecordFramePosition,
      long boottimeNs,
      long uncertaintyNs) {
    public AudioTimestampObservation {
      if (wavEndFramePosition < 0
          || audioRecordFramePosition < 0
          || boottimeNs <= 0
          || uncertaintyNs < 0) {
        throw new IllegalArgumentException("audio timestamp observation is invalid");
      }
    }
  }

  /** Complete immutable input to the canonical manifest encoder. */
  public record Data(
      String recordingId,
      String sharedRecordingId,
      String nodeId,
      String role,
      String createdAtUtc,
      long startedElapsedRealtimeNs,
      long stoppedElapsedRealtimeNs,
      int orientationDegrees,
      String stopReason,
      String videoCodec,
      long videoBytes,
      long firstVideoPtsUs,
      long lastVideoPtsUs,
      int audioSource,
      long audioFrames,
      long audioBytes,
      int cameraTimestampSource,
      ClockAnchor clockAnchor,
      List<CameraFrame> cameraFrames,
      List<EncodedSample> encodedSamples,
      List<AudioTimestampObservation> audioTimestamps) {
    public Data {
      requireText(recordingId, "recordingId");
      requireText(sharedRecordingId, "sharedRecordingId");
      requireText(nodeId, "nodeId");
      requireText(role, "role");
      requireText(createdAtUtc, "createdAtUtc");
      requireText(stopReason, "stopReason");
      requireText(videoCodec, "videoCodec");
      if (startedElapsedRealtimeNs <= 0
          || stoppedElapsedRealtimeNs < startedElapsedRealtimeNs
          || (orientationDegrees != 0
              && orientationDegrees != 90
              && orientationDegrees != 180
              && orientationDegrees != 270)
          || videoBytes <= 0
          || firstVideoPtsUs < 0
          || lastVideoPtsUs < firstVideoPtsUs
          || audioSource < 0
          || audioFrames <= 0
          || audioBytes != StreamingPcm16WavFile.expectedFileBytes(audioFrames)) {
        throw new IllegalArgumentException("field recording scalar metadata is invalid");
      }
      cameraFrames = boundedCopy(cameraFrames, MAXIMUM_CAMERA_FRAMES, "camera frames");
      encodedSamples = boundedCopy(encodedSamples, MAXIMUM_ENCODED_SAMPLES, "encoded samples");
      audioTimestamps =
          boundedCopy(audioTimestamps, MAXIMUM_AUDIO_TIMESTAMPS, "audio timestamps");
      Objects.requireNonNull(clockAnchor, "clockAnchor");
      if (cameraFrames.isEmpty() || encodedSamples.isEmpty() || audioTimestamps.isEmpty()) {
        throw new IllegalArgumentException("field recording timing evidence cannot be empty");
      }
      requireIncreasingCameraFrames(cameraFrames);
      requireIncreasingEncodedSamples(encodedSamples, firstVideoPtsUs, lastVideoPtsUs);
      requireIncreasingAudioTimestamps(audioTimestamps, audioFrames);
    }

    public long durationUs() {
      return (stoppedElapsedRealtimeNs - startedElapsedRealtimeNs) / 1_000;
    }
  }

  private FieldRecordingManifest() {}

  /** Encodes a compact, byte-for-byte deterministic JSON document with a trailing newline. */
  public static String toCanonicalJson(Data data) {
    Objects.requireNonNull(data, "data");
    StringBuilder json =
        new StringBuilder(
            1_024
                + data.cameraFrames().size() * 88
                + data.encodedSamples().size() * 104
                + data.audioTimestamps().size() * 128);
    json.append("{\"schema_version\":1");
    appendString(json, "session_kind", "field_recording");
    appendString(json, "recording_id", data.recordingId());
    appendString(json, "shared_recording_id", data.sharedRecordingId());
    appendString(json, "node_id", data.nodeId());
    appendString(json, "role", data.role());
    appendString(json, "created_at_utc", data.createdAtUtc());
    appendDecimalString(
        json, "started_elapsed_realtime_ns", data.startedElapsedRealtimeNs());
    appendDecimalString(
        json, "stopped_elapsed_realtime_ns", data.stoppedElapsedRealtimeNs());
    appendDecimalString(json, "duration_us", data.durationUs());
    json.append(",\"orientation_degrees\":").append(data.orientationDegrees());
    appendString(json, "stop_reason", data.stopReason());
    json.append(",\"video\":{");
    appendFirstString(json, "path", "video.mp4");
    appendString(json, "mime_type", "video/mp4");
    json.append(",\"width\":").append(WIDTH);
    json.append(",\"height\":").append(HEIGHT);
    json.append(",\"nominal_fps\":").append(NOMINAL_FRAMES_PER_SECOND);
    appendString(json, "codec", data.videoCodec());
    appendDecimalString(json, "bytes", data.videoBytes());
    appendDecimalString(json, "first_pts_us", data.firstVideoPtsUs());
    appendDecimalString(json, "last_pts_us", data.lastVideoPtsUs());
    json.append("},\"audio\":{");
    appendFirstString(json, "path", "audio.wav");
    appendString(json, "mime_type", "audio/wav");
    json.append(",\"sample_rate_hz\":").append(AUDIO_SAMPLE_RATE_HZ);
    json.append(",\"channel_count\":1");
    appendString(json, "encoding", "pcm_s16le");
    json.append(",\"audio_source\":").append(data.audioSource());
    appendDecimalString(json, "frames", data.audioFrames());
    appendDecimalString(json, "bytes", data.audioBytes());
    appendDecimalString(json, "first_frame_position", 0);
    appendDecimalString(json, "end_frame_position", data.audioFrames());
    json.append("},\"timing\":{");
    appendFirstString(json, "clock", "CLOCK_BOOTTIME");
    appendString(json, "encoder_pts_clock", "CLOCK_MONOTONIC");
    json.append(",\"camera_timestamp_source\":").append(data.cameraTimestampSource());
    ClockAnchor anchor = data.clockAnchor();
    json.append(",\"clock_anchor\":{");
    appendFirstString(json, "source_clock", "CLOCK_MONOTONIC");
    appendString(json, "target_clock", "CLOCK_BOOTTIME");
    appendDecimalString(
        json, "boottime_minus_monotonic_ns", anchor.boottimeMinusMonotonicNs());
    appendDecimalString(json, "monotonic_before_ns", anchor.monotonicBeforeNs());
    appendDecimalString(json, "boottime_ns", anchor.boottimeNs());
    appendDecimalString(json, "monotonic_after_ns", anchor.monotonicAfterNs());
    appendDecimalString(json, "bracket_ns", anchor.bracketNs());
    appendDecimalString(json, "uncertainty_ns", anchor.uncertaintyNs());
    json.append('}');
    json.append(",\"camera_frames\":[");
    for (int index = 0; index < data.cameraFrames().size(); ++index) {
      if (index != 0) {
        json.append(',');
      }
      CameraFrame frame = data.cameraFrames().get(index);
      json.append("{\"frame_number\":\"").append(frame.frameNumber()).append('"');
      appendDecimalString(json, "sensor_timestamp_ns", frame.sensorTimestampNs());
      json.append('}');
    }
    json.append("],\"encoded_samples\":[");
    for (int index = 0; index < data.encodedSamples().size(); ++index) {
      if (index != 0) {
        json.append(',');
      }
      EncodedSample sample = data.encodedSamples().get(index);
      json.append("{\"sample_index\":").append(sample.sampleIndex());
      appendDecimalString(json, "source_pts_us", sample.sourcePtsUs());
      appendDecimalString(json, "media_time_us", sample.mediaTimeUs());
      json.append(",\"flags\":").append(sample.flags());
      json.append(",\"bytes\":").append(sample.bytes()).append('}');
    }
    json.append("],\"audio_timestamp_observations\":[");
    for (int index = 0; index < data.audioTimestamps().size(); ++index) {
      if (index != 0) {
        json.append(',');
      }
      AudioTimestampObservation timestamp = data.audioTimestamps().get(index);
      json.append("{\"wav_end_frame_position\":\"")
          .append(timestamp.wavEndFramePosition())
          .append('"');
      appendDecimalString(
          json, "audio_record_frame_position", timestamp.audioRecordFramePosition());
      appendDecimalString(json, "boottime_ns", timestamp.boottimeNs());
      appendDecimalString(json, "uncertainty_ns", timestamp.uncertaintyNs());
      json.append('}');
    }
    return json.append("]}}\n").toString();
  }

  private static void requireText(String value, String label) {
    Objects.requireNonNull(value, label);
    if (value.isBlank() || value.length() > 256) {
      throw new IllegalArgumentException(label + " must be nonblank and bounded");
    }
  }

  private static <T> List<T> boundedCopy(List<T> input, int maximum, String label) {
    List<T> copy = List.copyOf(Objects.requireNonNull(input, label));
    if (copy.size() > maximum) {
      throw new IllegalArgumentException(label + " exceed their configured bound");
    }
    for (T item : copy) {
      Objects.requireNonNull(item, label + " entry");
    }
    return copy;
  }

  private static void requireIncreasingCameraFrames(List<CameraFrame> frames) {
    for (int index = 1; index < frames.size(); ++index) {
      CameraFrame prior = frames.get(index - 1);
      CameraFrame current = frames.get(index);
      if (current.frameNumber() <= prior.frameNumber()
          || current.sensorTimestampNs() <= prior.sensorTimestampNs()) {
        throw new IllegalArgumentException("camera timing evidence is not increasing");
      }
    }
  }

  private static void requireIncreasingEncodedSamples(
      List<EncodedSample> samples, long firstPtsUs, long lastPtsUs) {
    if (samples.get(0).sourcePtsUs() != firstPtsUs
        || samples.get(samples.size() - 1).sourcePtsUs() != lastPtsUs) {
      throw new IllegalArgumentException("video bounds disagree with encoded timing evidence");
    }
    for (int index = 0; index < samples.size(); ++index) {
      EncodedSample current = samples.get(index);
      if (current.sampleIndex() != index
          || (index > 0
              && (current.sourcePtsUs() <= samples.get(index - 1).sourcePtsUs()
                  || current.mediaTimeUs() <= samples.get(index - 1).mediaTimeUs()))) {
        throw new IllegalArgumentException("encoded timing evidence is not increasing");
      }
    }
  }

  private static void requireIncreasingAudioTimestamps(
      List<AudioTimestampObservation> timestamps, long audioFrames) {
    for (int index = 0; index < timestamps.size(); ++index) {
      AudioTimestampObservation current = timestamps.get(index);
      if (current.wavEndFramePosition() > audioFrames
          || (index > 0
              && (current.wavEndFramePosition()
                      <= timestamps.get(index - 1).wavEndFramePosition()
                  || current.audioRecordFramePosition()
                      <= timestamps.get(index - 1).audioRecordFramePosition()
                  || current.boottimeNs() <= timestamps.get(index - 1).boottimeNs()))) {
        throw new IllegalArgumentException("audio timing evidence is not increasing");
      }
    }
  }

  private static void appendFirstString(StringBuilder json, String name, String value) {
    json.append('"');
    appendEscaped(json, name);
    json.append("\":\"");
    appendEscaped(json, value);
    json.append('"');
  }

  private static void appendString(StringBuilder json, String name, String value) {
    json.append(',');
    appendFirstString(json, name, value);
  }

  private static void appendDecimalString(StringBuilder json, String name, long value) {
    json.append(",\"");
    appendEscaped(json, name);
    json.append("\":\"").append(value).append('"');
  }

  private static void appendEscaped(StringBuilder json, String value) {
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
            json.append(String.format(java.util.Locale.ROOT, "\\u%04x", (int) character));
          } else {
            json.append(character);
          }
        }
      }
    }
  }
}
