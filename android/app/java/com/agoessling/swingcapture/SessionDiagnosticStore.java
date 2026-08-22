package com.agoessling.swingcapture;

import com.agoessling.swingcapture.diagnostics.DiagnosticFeedbackRequest;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/** Atomic app-private persistence for one canonical diagnostic incident per session. */
public final class SessionDiagnosticStore {
  public static final String FILE_NAME = "diagnostic_incident.json";

  @FunctionalInterface
  public interface DirectorySync {
    void synchronize(File directory) throws IOException;
  }

  /** Inclusive bounds for user timing labels expressed relative to local detected impact. */
  public record LocalTimingBounds(long minimumOffsetMicros, long maximumOffsetMicros) {
    public LocalTimingBounds {
      if (minimumOffsetMicros > 0
          || maximumOffsetMicros < 0
          || minimumOffsetMicros > maximumOffsetMicros) {
        throw new IllegalArgumentException("local timing bounds must be ordered and contain zero");
      }
    }

    public boolean contains(long offsetMicros) {
      return offsetMicros >= minimumOffsetMicros && offsetMicros <= maximumOffsetMicros;
    }
  }

  /** Optional independent bounds for the preview/video and diagnostic-audio label domains. */
  public record LocalTimingPolicy(
      Optional<LocalTimingBounds> desiredHighSpeedStart,
      Optional<LocalTimingBounds> visualImpact,
      Optional<LocalTimingBounds> audioImpact) {
    public LocalTimingPolicy {
      Objects.requireNonNull(desiredHighSpeedStart, "desiredHighSpeedStart");
      Objects.requireNonNull(visualImpact, "visualImpact");
      Objects.requireNonNull(audioImpact, "audioImpact");
    }

    public static LocalTimingPolicy unbounded() {
      return new LocalTimingPolicy(Optional.empty(), Optional.empty(), Optional.empty());
    }

    private Optional<LocalTimingBounds> boundsFor(DiagnosticIncident.TimingMark mark) {
      return switch (mark.kind()) {
        case HIGH_SPEED_SHOULD_START -> desiredHighSpeedStart;
        case VISUAL_BALL_IMPACT -> visualImpact;
        case AUDIO_IMPACT_TRANSIENT -> audioImpact;
      };
    }
  }

  private final DirectorySync directorySync;

  public SessionDiagnosticStore(DirectorySync directorySync) {
    this.directorySync = Objects.requireNonNull(directorySync, "directorySync");
  }

  public DiagnosticIncident initialize(
      File sessionDirectory,
      String sessionId,
      String sourceNodeId,
      String triggerSource,
      long createdAtEpochMillis)
      throws IOException {
    DiagnosticIncident incident =
        new DiagnosticIncident(
            sessionId,
            initialClassification(triggerSource),
            createdAtEpochMillis,
            sourceNodeId,
            DiagnosticIncident.UserFeedback.unreviewed(),
            List.of());
    File target = checkedTarget(sessionDirectory, sessionId, true);
    if (target.exists()) {
      throw new IOException("Diagnostic incident already exists");
    }
    writeAtomic(target, incident.toCanonicalJson());
    return incident;
  }

  public DiagnosticIncident applyFeedback(
      File sessionDirectory, String sessionId, byte[] requestBody) throws IOException {
    return applyFeedback(sessionDirectory, sessionId, requestBody, LocalTimingPolicy.unbounded());
  }

  /** Applies feedback only when every local timing label lies inside the supplied bounds. */
  public synchronized DiagnosticIncident applyFeedback(
      File sessionDirectory,
      String sessionId,
      byte[] requestBody,
      LocalTimingPolicy localTimingPolicy)
      throws IOException {
    Objects.requireNonNull(requestBody, "requestBody");
    Objects.requireNonNull(localTimingPolicy, "localTimingPolicy");
    File target = checkedTarget(sessionDirectory, sessionId, false);
    DiagnosticIncident existing = read(target);
    DiagnosticFeedbackRequest request =
        DiagnosticFeedbackRequest.parseJson(decodeStrictFeedback(requestBody));
    for (DiagnosticIncident.TimingMark mark : request.timingMarks()) {
      if (localTimingPolicy
          .boundsFor(mark)
          .filter(bounds -> !bounds.contains(mark.offsetMicros()))
          .isPresent()) {
        throw new IllegalArgumentException("feedback timing mark lies outside local evidence");
      }
    }
    DiagnosticIncident updated = request.applyTo(existing);
    writeAtomic(target, updated.toCanonicalJson());
    return updated;
  }

  public DiagnosticIncident read(File sessionDirectory, String sessionId) throws IOException {
    return read(checkedTarget(sessionDirectory, sessionId, false));
  }

  private static DiagnosticIncident read(File target) throws IOException {
    if (!target.isFile() || Files.isSymbolicLink(target.toPath())) {
      throw new IOException("Diagnostic incident does not exist");
    }
    byte[] contents;
    try (FileInputStream input = new FileInputStream(target)) {
      contents = input.readNBytes(DiagnosticIncident.MAXIMUM_SERIALIZED_BYTES + 1);
      if (input.read() >= 0 || contents.length > DiagnosticIncident.MAXIMUM_SERIALIZED_BYTES) {
        throw new IOException("Diagnostic incident exceeds its configured bound");
      }
    }
    try {
      return DiagnosticIncident.fromCanonicalJson(decodeStrictUtf8(contents));
    } catch (IllegalArgumentException | CharacterCodingException malformed) {
      throw new IOException("Diagnostic incident is malformed", malformed);
    }
  }

  private static String decodeStrictFeedback(byte[] contents) {
    try {
      return decodeStrictUtf8(contents);
    } catch (CharacterCodingException malformed) {
      throw new IllegalArgumentException("diagnostic feedback is not valid UTF-8", malformed);
    }
  }

  private static String decodeStrictUtf8(byte[] contents) throws CharacterCodingException {
    CharsetDecoder decoder =
        StandardCharsets.UTF_8
            .newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT);
    return decoder.decode(ByteBuffer.wrap(contents)).toString();
  }

  private static DiagnosticIncident.IncidentClassification initialClassification(
      String triggerSource) {
    return switch (Objects.requireNonNull(triggerSource, "triggerSource")) {
      case "local_audio", "manual" ->
          DiagnosticIncident.IncidentClassification.SUCCESSFUL_CAPTURE;
      case "missed_shot" ->
          DiagnosticIncident.IncidentClassification.USER_REPORTED;
      case "pose_armed_no_impact" ->
          DiagnosticIncident.IncidentClassification.POSE_ARMED_NO_IMPACT;
      default -> throw new IllegalArgumentException("Unsupported diagnostic trigger source");
    };
  }

  private static File checkedTarget(
      File sessionDirectory, String sessionId, boolean allowTemporaryDirectory)
      throws IOException {
    Objects.requireNonNull(sessionDirectory, "sessionDirectory");
    Objects.requireNonNull(sessionId, "sessionId");
    if (!sessionId.matches("[A-Za-z0-9._-]+")
        || !sessionDirectory.isDirectory()
        || Files.isSymbolicLink(sessionDirectory.toPath())
        || (!sessionDirectory.getName().equals(sessionId)
            && !(allowTemporaryDirectory
                && sessionDirectory.getName().equals(sessionId + ".tmp")))) {
      throw new IOException("Diagnostic session directory is invalid");
    }
    File target = new File(sessionDirectory, FILE_NAME);
    if (!target.getCanonicalFile().getParentFile().equals(sessionDirectory.getCanonicalFile())) {
      throw new IOException("Diagnostic incident path escapes its session");
    }
    return target;
  }

  private void writeAtomic(File target, String canonicalJson) throws IOException {
    byte[] contents = canonicalJson.getBytes(StandardCharsets.UTF_8);
    if (contents.length > DiagnosticIncident.MAXIMUM_SERIALIZED_BYTES) {
      throw new IOException("Diagnostic incident exceeds its configured bound");
    }
    File temporary = new File(target.getParentFile(), target.getName() + ".tmp");
    if (temporary.exists() && !temporary.isFile()) {
      throw new IOException("Diagnostic incident temporary path is not a file");
    }
    Files.deleteIfExists(temporary.toPath());
    try {
      try (FileOutputStream output = new FileOutputStream(temporary)) {
        output.write(contents);
        output.getFD().sync();
      }
      try {
        Files.move(
            temporary.toPath(),
            target.toPath(),
            StandardCopyOption.ATOMIC_MOVE,
            StandardCopyOption.REPLACE_EXISTING);
      } catch (AtomicMoveNotSupportedException unsupported) {
        Files.move(
            temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
      }
      directorySync.synchronize(target.getParentFile());
    } finally {
      Files.deleteIfExists(temporary.toPath());
    }
  }
}
