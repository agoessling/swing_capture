package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Objects;

/**
 * Bounded atomic publication of exact pose-preview inputs and their inference/controller trace.
 *
 * <p>The two files are staged and fsynced inside one temporary directory, then that complete
 * directory is renamed into the temporary session. A caller can therefore treat any returned
 * {@link ManifestMetadata} as a complete unit and catch publication failures without affecting the
 * primary capture. The caller should pass {@code AndroidDirectorySync::synchronize} on Android.
 */
public final class PoseDiagnosticFiles {
  public static final int SCHEMA_VERSION = 1;
  public static final String DIRECTORY_NAME = "pose_diagnostics";
  public static final String FRAMES_FILE_NAME = "preview_frames.mjpeg";
  public static final String TRACE_FILE_NAME = "pose_trace.ndjson";
  public static final String FRAMES_RELATIVE_PATH = DIRECTORY_NAME + "/" + FRAMES_FILE_NAME;
  public static final String TRACE_RELATIVE_PATH = DIRECTORY_NAME + "/" + TRACE_FILE_NAME;
  public static final String FRAME_CONTENT_TYPE = "image/jpeg";
  public static final String TRACE_CONTENT_TYPE = "application/x-ndjson";
  public static final int MAXIMUM_ENTRY_COUNT =
      PreviewEvidenceRing.RECOMMENDED_MAXIMUM_ENTRY_COUNT;
  public static final long MAXIMUM_FRAME_BYTES =
      PreviewEvidenceRing.RECOMMENDED_MAXIMUM_COMPRESSED_BYTES;
  public static final int MAXIMUM_TRACE_BYTES = 1024 * 1024;

  private static final String STAGING_DIRECTORY_NAME = DIRECTORY_NAME + ".tmp";

  @FunctionalInterface
  public interface DirectorySync {
    void synchronize(File directory) throws IOException;
  }

  /** Byte index for one exact JPEG in the concatenated preview stream. */
  public record FrameMetadata(
      int sequenceIndex,
      long timestampBoottimeNanos,
      long byteOffset,
      int byteLength,
      String contentType) {
    public FrameMetadata {
      if (sequenceIndex < 0
          || timestampBoottimeNanos < 0
          || byteOffset < 0
          || byteLength < 4
          || !FRAME_CONTENT_TYPE.equals(contentType)) {
        throw new IllegalArgumentException("pose preview frame metadata is invalid");
      }
    }
  }

  /** Manifest-ready immutable description of a successfully published evidence unit. */
  public record ManifestMetadata(
      int schemaVersion,
      String framesRelativePath,
      String framesContentType,
      long framesBytes,
      String traceRelativePath,
      String traceContentType,
      int traceBytes,
      long firstTimestampInclusive,
      long endTimestampExclusive,
      int observationCount,
      List<FrameMetadata> frames) {
    public ManifestMetadata {
      if (schemaVersion != SCHEMA_VERSION
          || !FRAMES_RELATIVE_PATH.equals(framesRelativePath)
          || !FRAME_CONTENT_TYPE.equals(framesContentType)
          || !TRACE_RELATIVE_PATH.equals(traceRelativePath)
          || !TRACE_CONTENT_TYPE.equals(traceContentType)
          || framesBytes < 0
          || framesBytes > MAXIMUM_FRAME_BYTES
          || traceBytes <= 0
          || traceBytes > MAXIMUM_TRACE_BYTES
          || firstTimestampInclusive < 0
          || endTimestampExclusive <= firstTimestampInclusive
          || observationCount <= 0
          || observationCount > MAXIMUM_ENTRY_COUNT) {
        throw new IllegalArgumentException("pose diagnostic manifest metadata is invalid");
      }
      frames = List.copyOf(Objects.requireNonNull(frames, "frames"));
      if (frames.size() > observationCount) {
        throw new IllegalArgumentException("pose diagnostic frame count is invalid");
      }
      long expectedOffset = 0;
      long previousTimestamp = -1;
      for (int index = 0; index < frames.size(); ++index) {
        FrameMetadata frame = Objects.requireNonNull(frames.get(index), "frame metadata");
        if ((index > 0 && frame.sequenceIndex() <= frames.get(index - 1).sequenceIndex())
            || frame.sequenceIndex() >= observationCount
            || frame.byteOffset() != expectedOffset
            || frame.timestampBoottimeNanos() < firstTimestampInclusive
            || frame.timestampBoottimeNanos() >= endTimestampExclusive
            || frame.timestampBoottimeNanos() <= previousTimestamp
            || expectedOffset > MAXIMUM_FRAME_BYTES - frame.byteLength()) {
          throw new IllegalArgumentException("pose diagnostic frame index is inconsistent");
        }
        expectedOffset += frame.byteLength();
        previousTimestamp = frame.timestampBoottimeNanos();
      }
      if (expectedOffset != framesBytes) {
        throw new IllegalArgumentException("pose diagnostic frame bytes are inconsistent");
      }
    }

    public int frameCount() {
      return frames.size();
    }

    /** Canonical compact JSON object suitable for insertion into a session manifest. */
    public String toCanonicalJson() {
      StringBuilder json = new StringBuilder(512 + frames.size() * 96);
      json.append("{\"schema_version\":").append(schemaVersion);
      appendString(json, "frames_path", framesRelativePath);
      appendString(json, "frames_content_type", framesContentType);
      appendDecimalString(json, "frames_bytes", framesBytes);
      appendString(json, "trace_path", traceRelativePath);
      appendString(json, "trace_content_type", traceContentType);
      json.append(",\"trace_bytes\":").append(traceBytes);
      appendDecimalString(json, "first_timestamp_boottime_ns", firstTimestampInclusive);
      appendDecimalString(json, "end_timestamp_boottime_ns_exclusive", endTimestampExclusive);
      json.append(",\"observation_count\":").append(observationCount);
      json.append(",\"jpeg_frame_count\":").append(frames.size());
      json.append(",\"frame_count\":").append(frames.size()).append(",\"frame_index\":[");
      for (int index = 0; index < frames.size(); ++index) {
        if (index != 0) {
          json.append(',');
        }
        FrameMetadata frame = frames.get(index);
        json.append("{\"sequence_index\":").append(frame.sequenceIndex());
        appendDecimalString(json, "timestamp_boottime_ns", frame.timestampBoottimeNanos());
        appendDecimalString(json, "byte_offset", frame.byteOffset());
        json.append(",\"byte_length\":").append(frame.byteLength());
        appendString(json, "content_type", frame.contentType());
        json.append('}');
      }
      return json.append("]}").toString();
    }
  }

  private record PreparedEvidence(
      List<FrameMetadata> frames, byte[] traceBytes, long compressedBytes) {}

  private final DirectorySync directorySync;

  public PoseDiagnosticFiles(DirectorySync directorySync) {
    this.directorySync = Objects.requireNonNull(directorySync, "directorySync");
  }

  /** Publishes one nonempty detached snapshot into an existing {@code session-id.tmp} directory. */
  public ManifestMetadata publish(
      File temporarySessionDirectory, PreviewEvidenceRing.Snapshot snapshot) throws IOException {
    Objects.requireNonNull(snapshot, "snapshot");
    File session = validateTemporarySessionDirectory(temporarySessionDirectory);
    PreparedEvidence prepared = prepare(snapshot);
    ManifestMetadata metadata =
        new ManifestMetadata(
            SCHEMA_VERSION,
            FRAMES_RELATIVE_PATH,
            FRAME_CONTENT_TYPE,
            prepared.compressedBytes(),
            TRACE_RELATIVE_PATH,
            TRACE_CONTENT_TYPE,
            prepared.traceBytes().length,
            snapshot.firstTimestampInclusive(),
            snapshot.endTimestampExclusive(),
            snapshot.entryCount(),
            prepared.frames());

    File staging = checkedChild(session, STAGING_DIRECTORY_NAME);
    File published = checkedChild(session, DIRECTORY_NAME);
    if (staging.exists() || published.exists()) {
      throw new IOException("Pose diagnostic publication path already exists");
    }
    if (!staging.mkdir()) {
      throw new IOException("Unable to create pose diagnostic staging directory");
    }

    try {
      File stagedFrames = checkedChild(staging, FRAMES_FILE_NAME);
      writeFrames(snapshot, stagedFrames, prepared.compressedBytes());
      File stagedTrace = checkedChild(staging, TRACE_FILE_NAME);
      writeBytes(stagedTrace, prepared.traceBytes());
      if (stagedFrames.length() != metadata.framesBytes()
          || stagedTrace.length() != metadata.traceBytes()) {
        throw new IOException("Pose diagnostic file sizes disagree with metadata");
      }
      directorySync.synchronize(staging);
      atomicMove(staging, published);
      directorySync.synchronize(session);
      return metadata;
    } catch (IOException | RuntimeException | Error failure) {
      if (staging.exists()) {
        try {
          cleanupStaging(staging);
        } catch (IOException cleanupFailure) {
          failure.addSuppressed(cleanupFailure);
        }
      }
      throw failure;
    }
  }

  private static PreparedEvidence prepare(PreviewEvidenceRing.Snapshot snapshot) {
    int count = snapshot.entryCount();
    if (count <= 0 || count > MAXIMUM_ENTRY_COUNT) {
      throw new IllegalArgumentException("pose diagnostic snapshot count is outside its bound");
    }
    if (snapshot.firstTimestampInclusive() < 0
        || snapshot.endTimestampExclusive() <= snapshot.firstTimestampInclusive()
        || snapshot.compressedBytes() < 0
        || snapshot.compressedBytes() > MAXIMUM_FRAME_BYTES) {
      throw new IllegalArgumentException("pose diagnostic snapshot bounds are invalid");
    }

    List<FrameMetadata> frames = new ArrayList<>(count);
    long offset = 0;
    long previousTimestamp = -1;
    for (int index = 0; index < count; ++index) {
      PreviewEvidence evidence = Objects.requireNonNull(snapshot.entryAt(index), "evidence");
      long timestamp = evidence.timestampBoottimeNanos();
      byte[] frame = evidence.copyCompressedFrame();
      if (frame.length != evidence.compressedFrameBytes()
          || timestamp < snapshot.firstTimestampInclusive()
          || timestamp >= snapshot.endTimestampExclusive()
          || timestamp <= previousTimestamp) {
        throw new IllegalArgumentException("pose diagnostic snapshot entries are inconsistent");
      }
      if (evidence.hasCompressedFrame()) {
        requireJpeg(frame, index);
        if (offset > MAXIMUM_FRAME_BYTES - frame.length) {
          throw new IllegalArgumentException("pose diagnostic frame bytes exceed their bound");
        }
        frames.add(
            new FrameMetadata(index, timestamp, offset, frame.length, FRAME_CONTENT_TYPE));
        offset += frame.length;
      }
      previousTimestamp = timestamp;
    }
    if (offset != snapshot.compressedBytes()) {
      throw new IllegalArgumentException("pose diagnostic snapshot byte count is inconsistent");
    }

    byte[] trace = buildTrace(snapshot, frames);
    if (trace.length == 0 || trace.length > MAXIMUM_TRACE_BYTES) {
      throw new IllegalArgumentException("pose diagnostic trace exceeds its configured bound");
    }
    return new PreparedEvidence(List.copyOf(frames), trace, offset);
  }

  private static byte[] buildTrace(
      PreviewEvidenceRing.Snapshot snapshot, List<FrameMetadata> frames) {
    StringBuilder trace = new StringBuilder(snapshot.entryCount() * 512);
    int frameIndex = 0;
    for (int index = 0; index < snapshot.entryCount(); ++index) {
      PreviewEvidence evidence = snapshot.entryAt(index);
      FrameMetadata frame =
          frameIndex < frames.size() && frames.get(frameIndex).sequenceIndex() == index
              ? frames.get(frameIndex++)
              : null;
      trace.append("{\"schema_version\":").append(SCHEMA_VERSION);
      trace.append(",\"sequence_index\":").append(index);
      appendDecimalString(trace, "timestamp_boottime_ns", evidence.timestampBoottimeNanos());
      trace.append(",\"frame_available\":").append(frame != null);
      if (frame == null) {
        appendNull(trace, "frame_content_type");
        appendNull(trace, "frame_byte_offset");
        trace.append(",\"frame_byte_length\":0");
      } else {
        appendString(trace, "frame_content_type", FRAME_CONTENT_TYPE);
        appendDecimalString(trace, "frame_byte_offset", frame.byteOffset());
        trace.append(",\"frame_byte_length\":").append(frame.byteLength());
      }
      appendString(trace, "model_id", evidence.modelId());
      trace.append(",\"image_rotation_degrees\":").append(evidence.imageRotationDegrees());
      appendDecimalString(trace, "inference_duration_ns", evidence.inferenceDurationNanos());
      appendDouble(trace, "person_confidence", evidence.personConfidence());
      appendDouble(trace, "address_confidence", evidence.addressConfidence());
      appendDouble(trace, "motion_magnitude", evidence.motionMagnitude());
      trace
          .append(",\"hitting_region_occupied\":")
          .append(evidence.hittingRegionOccupied());
      appendString(
          trace,
          "controller_state",
          evidence.controllerState().name().toLowerCase(Locale.ROOT));
      appendString(trace, "decision_reason", evidence.decisionReason());
      trace.append("}\n");
    }
    return trace.toString().getBytes(StandardCharsets.UTF_8);
  }

  private static void writeFrames(
      PreviewEvidenceRing.Snapshot snapshot, File target, long expectedBytes) throws IOException {
    long written = 0;
    try (FileOutputStream fileOutput = new FileOutputStream(target);
        BufferedOutputStream output = new BufferedOutputStream(fileOutput)) {
      for (int index = 0; index < snapshot.entryCount(); ++index) {
        PreviewEvidence evidence = snapshot.entryAt(index);
        byte[] frame = evidence.copyCompressedFrame();
        if (evidence.hasCompressedFrame()) {
          requireJpeg(frame, index);
          output.write(frame);
          written = Math.addExact(written, frame.length);
        }
      }
      output.flush();
      fileOutput.getFD().sync();
    }
    if (written != expectedBytes || target.length() != expectedBytes) {
      throw new IOException("Pose preview stream byte count changed during publication");
    }
  }

  private static void writeBytes(File target, byte[] contents) throws IOException {
    try (FileOutputStream output = new FileOutputStream(target)) {
      output.write(contents);
      output.getFD().sync();
    }
  }

  private static File validateTemporarySessionDirectory(File directory) throws IOException {
    Objects.requireNonNull(directory, "temporarySessionDirectory");
    if (!directory.isDirectory()
        || Files.isSymbolicLink(directory.toPath())
        || !directory.getName().matches("[A-Za-z0-9._-]+\\.tmp")) {
      throw new IOException("Pose diagnostics require a safe temporary session directory");
    }
    return directory.getCanonicalFile();
  }

  private static File checkedChild(File parent, String name) throws IOException {
    if (!name.matches("[A-Za-z0-9._-]+")) {
      throw new IllegalArgumentException("Pose diagnostic filename is unsafe");
    }
    File child = new File(parent, name);
    if (!child.getCanonicalFile().getParentFile().equals(parent.getCanonicalFile())
        || Files.isSymbolicLink(child.toPath())) {
      throw new IOException("Pose diagnostic path escapes its parent");
    }
    return child;
  }

  private static void atomicMove(File staging, File published) throws IOException {
    try {
      Files.move(staging.toPath(), published.toPath(), StandardCopyOption.ATOMIC_MOVE);
    } catch (AtomicMoveNotSupportedException unsupported) {
      Files.move(staging.toPath(), published.toPath());
    }
  }

  private static void cleanupStaging(File staging) throws IOException {
    if (!staging.exists()) {
      return;
    }
    Files.deleteIfExists(new File(staging, FRAMES_FILE_NAME).toPath());
    Files.deleteIfExists(new File(staging, TRACE_FILE_NAME).toPath());
    Files.delete(staging.toPath());
  }

  private static void requireJpeg(byte[] frame, int index) {
    if (frame.length < 4
        || (frame[0] & 0xff) != 0xff
        || (frame[1] & 0xff) != 0xd8
        || (frame[frame.length - 2] & 0xff) != 0xff
        || (frame[frame.length - 1] & 0xff) != 0xd9) {
      throw new IllegalArgumentException("pose preview frame " + index + " is not a bounded JPEG");
    }
  }

  private static void appendDouble(StringBuilder json, String name, double value) {
    json.append(',');
    appendEscaped(json, name);
    json.append(':').append(value == 0.0 ? "0.0" : Double.toString(value));
  }

  private static void appendDecimalString(StringBuilder json, String name, long value) {
    appendString(json, name, Long.toString(value));
  }

  private static void appendString(StringBuilder json, String name, String value) {
    json.append(',');
    appendEscaped(json, name);
    json.append(':');
    appendEscaped(json, value);
  }

  private static void appendNull(StringBuilder json, String name) {
    json.append(',');
    appendEscaped(json, name);
    json.append(":null");
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
}
