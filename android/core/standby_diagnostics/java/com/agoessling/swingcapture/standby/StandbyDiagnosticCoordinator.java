package com.agoessling.swingcapture.standby;

import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ContinuousAudioImpactDetector;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.function.Supplier;

/**
 * Pure event coordinator for continuous standby audio and immutable missed-event evidence.
 *
 * <p>The coordinator owns a fixed 60-second, 48 kHz mono PCM16 ring, impact detection, and the
 * AudioRecord-frame-to-BOOTTIME model. A detected impact or explicit operator tag registers one
 * bounded event immediately. Its supplied preview snapshot is frozen at registration; its audio
 * snapshot becomes available only after the exact two-second post-roll has arrived.
 *
 * <p>All methods are synchronized so the AudioRecord read loop and an operator-control thread may
 * safely share one instance. Callbacks and filesystem work deliberately live outside this class.
 */
public final class StandbyDiagnosticCoordinator {
  public static final int SAMPLE_RATE_HZ = ImpactDetector.SAMPLE_RATE_HZ;
  public static final int RETENTION_SECONDS = 60;
  public static final int RETENTION_FRAMES = SAMPLE_RATE_HZ * RETENTION_SECONDS;
  public static final int DEFAULT_PRE_ROLL_FRAMES = 10 * SAMPLE_RATE_HZ;
  public static final int DEFAULT_POST_ROLL_FRAMES = 2 * SAMPLE_RATE_HZ;
  public static final int MAXIMUM_PENDING_EVENTS = 4;

  public enum EventKind {
    DETECTED_IMPACT("detected_impact"),
    OPERATOR_TAG("operator_tag");

    private final String wireName;

    EventKind(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }
  }

  public enum AudioClockStatus {
    VALIDATED("validated"),
    UNVALIDATED("audio_clock_unvalidated");

    private final String wireName;

    AudioClockStatus(String wireName) {
      this.wireName = wireName;
    }

    public String wireName() {
      return wireName;
    }
  }

  public enum DropReason {
    PENDING_LIMIT,
    AUDIO_DISCONTINUITY,
    MARKER_EVICTED;

    public String wireName() {
      return name().toLowerCase(Locale.ROOT);
    }
  }

  public record Config(int preRollFrames, int postRollFrames, int maximumPendingEvents) {
    public Config {
      if (preRollFrames < 0
          || postRollFrames <= 0
          || (long) preRollFrames + postRollFrames > RETENTION_FRAMES
          || maximumPendingEvents <= 0
          || maximumPendingEvents > MAXIMUM_PENDING_EVENTS) {
        throw new IllegalArgumentException("standby diagnostic configuration is invalid");
      }
    }

    public static Config defaults() {
      return new Config(
          DEFAULT_PRE_ROLL_FRAMES, DEFAULT_POST_ROLL_FRAMES, MAXIMUM_PENDING_EVENTS);
    }
  }

  /** Immutable registration retained until audio post-roll is complete or explicitly dropped. */
  public record EventMarker(
      long sequence,
      EventKind kind,
      long markerFramePosition,
      OptionalLong operatorReceivedBoottimeNanos,
      Optional<ContinuousAudioImpactDetector.TimedImpact> detectedImpact,
      Optional<AudioTimestampMapper.Estimate> markerTime,
      PreviewEvidenceRing.Snapshot previewSnapshot) {
    public EventMarker {
      if (sequence <= 0 || markerFramePosition < 0) {
        throw new IllegalArgumentException("standby diagnostic event identity is invalid");
      }
      Objects.requireNonNull(kind, "kind");
      Objects.requireNonNull(operatorReceivedBoottimeNanos, "operatorReceivedBoottimeNanos");
      Objects.requireNonNull(detectedImpact, "detectedImpact");
      Objects.requireNonNull(markerTime, "markerTime");
      Objects.requireNonNull(previewSnapshot, "previewSnapshot");
      if (kind == EventKind.DETECTED_IMPACT) {
        if (operatorReceivedBoottimeNanos.isPresent()
            || detectedImpact.isEmpty()
            || detectedImpact.orElseThrow().impact().strikeFramePosition()
                != markerFramePosition) {
          throw new IllegalArgumentException("detected-impact event fields are inconsistent");
        }
      } else if (operatorReceivedBoottimeNanos.isEmpty()
          || operatorReceivedBoottimeNanos.orElseThrow() <= 0
          || detectedImpact.isPresent()) {
        throw new IllegalArgumentException("operator-tag event fields are inconsistent");
      }
      if (markerTime.isPresent()
          && markerTime.orElseThrow().framePosition() != markerFramePosition) {
        throw new IllegalArgumentException("event marker time maps the wrong audio frame");
      }
    }

    public AudioClockStatus audioClockStatus() {
      return markerTime.isPresent() ? AudioClockStatus.VALIDATED : AudioClockStatus.UNVALIDATED;
    }
  }

  /** Complete detached evidence, safe to hand to an asynchronous session publisher. */
  public record FrozenEvidence(EventMarker event, DiagnosticAudioRing.Snapshot audioSnapshot) {
    public FrozenEvidence {
      Objects.requireNonNull(event, "event");
      Objects.requireNonNull(audioSnapshot, "audioSnapshot");
      if (event.markerFramePosition() < audioSnapshot.firstFramePosition()
          || event.markerFramePosition() >= audioSnapshot.endFramePosition()) {
        throw new IllegalArgumentException("frozen audio does not contain its event marker");
      }
    }
  }

  public record DroppedEvent(EventMarker event, DropReason reason) {
    public DroppedEvent {
      Objects.requireNonNull(event, "event");
      Objects.requireNonNull(reason, "reason");
    }
  }

  public record StreamDiscontinuity(
      DiagnosticAudioRing.DiscontinuityKind kind,
      long expectedFirstFramePosition,
      long receivedFirstFramePosition) {
    public StreamDiscontinuity {
      Objects.requireNonNull(kind, "kind");
      if (expectedFirstFramePosition < 0 || receivedFirstFramePosition < 0) {
        throw new IllegalArgumentException("audio discontinuity positions must be nonnegative");
      }
    }
  }

  /** All externally relevant effects of one successful nonempty PCM append. */
  public record AppendResult(
      ImpactDetector.ProcessResult detectorMetrics,
      List<EventMarker> registeredEvents,
      List<FrozenEvidence> frozenEvidence,
      List<DroppedEvent> droppedEvents,
      Optional<StreamDiscontinuity> discontinuity) {
    public AppendResult {
      Objects.requireNonNull(detectorMetrics, "detectorMetrics");
      registeredEvents = List.copyOf(Objects.requireNonNull(registeredEvents, "registeredEvents"));
      frozenEvidence = List.copyOf(Objects.requireNonNull(frozenEvidence, "frozenEvidence"));
      droppedEvents = List.copyOf(Objects.requireNonNull(droppedEvents, "droppedEvents"));
      Objects.requireNonNull(discontinuity, "discontinuity");
    }
  }

  /** Registration and any coincident state changes caused by one operator button receipt. */
  public record OperatorTagResult(
      EventMarker event,
      boolean accepted,
      List<FrozenEvidence> frozenEvidence,
      List<DroppedEvent> droppedEvents) {
    public OperatorTagResult {
      Objects.requireNonNull(event, "event");
      frozenEvidence = List.copyOf(Objects.requireNonNull(frozenEvidence, "frozenEvidence"));
      droppedEvents = List.copyOf(Objects.requireNonNull(droppedEvents, "droppedEvents"));
      if (accepted == droppedEvents.stream().anyMatch(drop -> drop.event().equals(event))) {
        throw new IllegalArgumentException("operator tag acceptance disagrees with drop result");
      }
    }
  }

  private final Config config;
  private final ContinuousAudioImpactDetector detector;
  private final ArrayList<EventMarker> pending = new ArrayList<>();
  private DiagnosticAudioRing audioRing = new DiagnosticAudioRing(RETENTION_FRAMES);
  private long nextSequence = 1;

  public StandbyDiagnosticCoordinator() {
    this(
        Config.defaults(),
        ImpactDetector.Config.defaults(),
        AudioTimestampMapper.Config.defaults());
  }

  public StandbyDiagnosticCoordinator(
      Config config,
      ImpactDetector.Config detectorConfig,
      AudioTimestampMapper.Config timestampConfig) {
    this.config = Objects.requireNonNull(config, "config");
    detector =
        new ContinuousAudioImpactDetector(
            Objects.requireNonNull(detectorConfig, "detectorConfig"),
            Objects.requireNonNull(timestampConfig, "timestampConfig"));
  }

  /** Passes through one successful AudioRecord TIMEBASE_BOOTTIME timestamp observation. */
  public synchronized AudioTimestampMapper.ObservationResult observeAudioTimestamp(
      long framePosition, long boottimeNanos, long uncertaintyNanos) {
    return detector.observeAudioTimestamp(framePosition, boottimeNanos, uncertaintyNanos);
  }

  /**
   * Appends one exact AudioRecord block and detects impacts against the same absolute positions.
   *
   * <p>{@code previewSnapshot} is attached only to impacts registered from this block. Supplying a
   * detached ring snapshot keeps image encoding and retention outside the audio critical section.
   */
  public synchronized AppendResult appendPcm16(
      short[] pcm,
      int offset,
      int frameCount,
      long firstFramePosition,
      PreviewEvidenceRing.Snapshot previewSnapshot) {
    Objects.requireNonNull(previewSnapshot, "previewSnapshot");
    return appendPcm16(
        pcm, offset, frameCount, firstFramePosition, () -> previewSnapshot);
  }

  /**
   * Lazy variant used by the live AudioRecord owner; the supplier runs once only when this block
   * actually confirms one or more impacts.
   */
  public synchronized AppendResult appendPcm16(
      short[] pcm,
      int offset,
      int frameCount,
      long firstFramePosition,
      Supplier<PreviewEvidenceRing.Snapshot> previewSnapshotSupplier) {
    Objects.requireNonNull(previewSnapshotSupplier, "previewSnapshotSupplier");
    Objects.requireNonNull(pcm, "pcm");
    Objects.checkFromIndexSize(offset, frameCount, pcm.length);
    if (frameCount <= 0) {
      throw new IllegalArgumentException("standby audio append must be nonempty");
    }

    ArrayList<DroppedEvent> dropped = new ArrayList<>();
    Optional<StreamDiscontinuity> discontinuity = Optional.empty();
    try {
      audioRing.append(pcm, offset, frameCount, firstFramePosition);
    } catch (DiagnosticAudioRing.AppendDiscontinuityException gap) {
      discontinuity =
          Optional.of(
              new StreamDiscontinuity(
                  gap.kind(),
                  gap.expectedFirstFramePosition(),
                  gap.receivedFirstFramePosition()));
      for (EventMarker event : pending) {
        dropped.add(new DroppedEvent(event, DropReason.AUDIO_DISCONTINUITY));
      }
      pending.clear();
      audioRing = new DiagnosticAudioRing(RETENTION_FRAMES);
      detector.reset();
      audioRing.append(pcm, offset, frameCount, firstFramePosition);
    }

    ArrayList<ContinuousAudioImpactDetector.TimedImpact> impacts = new ArrayList<>();
    ImpactDetector.ProcessResult metrics =
        detector.processPcm16(
            pcm, offset, frameCount, firstFramePosition, impacts::add);
    ArrayList<EventMarker> registered = new ArrayList<>(impacts.size());
    PreviewEvidenceRing.Snapshot previewSnapshot =
        impacts.isEmpty()
            ? null
            : Objects.requireNonNull(previewSnapshotSupplier.get(), "preview snapshot");
    for (ContinuousAudioImpactDetector.TimedImpact impact : impacts) {
      EventMarker event = detectedImpactEvent(impact, previewSnapshot);
      if (register(event)) {
        registered.add(event);
      } else {
        dropped.add(new DroppedEvent(event, DropReason.PENDING_LIMIT));
      }
    }
    ArrayList<FrozenEvidence> frozen = new ArrayList<>();
    drainReady(frozen, dropped);
    return new AppendResult(metrics, registered, frozen, dropped, discontinuity);
  }

  /**
   * Tags the latest retained AudioRecord frame at operator-button receipt; it never guesses impact.
   */
  public synchronized OperatorTagResult tagOperator(
      long receivedBoottimeNanos, PreviewEvidenceRing.Snapshot previewSnapshot) {
    Objects.requireNonNull(previewSnapshot, "previewSnapshot");
    if (receivedBoottimeNanos <= 0) {
      throw new IllegalArgumentException("operator receipt BOOTTIME must be positive");
    }
    long retainedEnd = audioRing.endRetainedFramePosition();
    if (retainedEnd <= 0) {
      throw new IllegalStateException("operator tag requires retained standby audio");
    }

    ArrayList<FrozenEvidence> frozen = new ArrayList<>();
    ArrayList<DroppedEvent> dropped = new ArrayList<>();
    drainReady(frozen, dropped);
    long markerFramePosition = retainedEnd - 1;
    EventMarker event =
        new EventMarker(
            allocateSequence(),
            EventKind.OPERATOR_TAG,
            markerFramePosition,
            OptionalLong.of(receivedBoottimeNanos),
            Optional.empty(),
            detector.timestampMapper().estimateBoottime(markerFramePosition),
            previewSnapshot);
    boolean accepted = register(event);
    if (!accepted) {
      dropped.add(new DroppedEvent(event, DropReason.PENDING_LIMIT));
    }
    return new OperatorTagResult(event, accepted, frozen, dropped);
  }

  public synchronized int pendingEventCount() {
    return pending.size();
  }

  public synchronized int retainedAudioFrameCount() {
    return audioRing.retainedFrameCount();
  }

  public synchronized long retainedAudioFirstFramePosition() {
    return audioRing.oldestRetainedFramePosition();
  }

  public synchronized long retainedAudioEndFramePosition() {
    return audioRing.endRetainedFramePosition();
  }

  public Config config() {
    return config;
  }

  private EventMarker detectedImpactEvent(
      ContinuousAudioImpactDetector.TimedImpact impact,
      PreviewEvidenceRing.Snapshot previewSnapshot) {
    long marker = impact.impact().strikeFramePosition();
    return new EventMarker(
        allocateSequence(),
        EventKind.DETECTED_IMPACT,
        marker,
        OptionalLong.empty(),
        Optional.of(impact),
        impact.strikeTime(),
        previewSnapshot);
  }

  private long allocateSequence() {
    if (nextSequence == Long.MAX_VALUE) {
      throw new IllegalStateException("standby diagnostic event sequence exhausted");
    }
    return nextSequence++;
  }

  private boolean register(EventMarker event) {
    if (event.markerFramePosition() > Long.MAX_VALUE - config.postRollFrames()) {
      throw new IllegalArgumentException("event audio post-roll overflows frame position");
    }
    if (pending.size() >= config.maximumPendingEvents()) {
      return false;
    }
    pending.add(event);
    return true;
  }

  private void drainReady(
      List<FrozenEvidence> frozen, List<DroppedEvent> dropped) {
    long retainedFirst = audioRing.oldestRetainedFramePosition();
    long retainedEnd = audioRing.endRetainedFramePosition();
    if (retainedFirst < 0) {
      return;
    }
    Iterator<EventMarker> iterator = pending.iterator();
    while (iterator.hasNext()) {
      EventMarker event = iterator.next();
      if (event.markerFramePosition() < retainedFirst) {
        iterator.remove();
        dropped.add(new DroppedEvent(event, DropReason.MARKER_EVICTED));
        continue;
      }
      long desiredEnd = event.markerFramePosition() + config.postRollFrames();
      if (retainedEnd < desiredEnd) {
        continue;
      }
      long desiredFirst = Math.max(0, event.markerFramePosition() - (long) config.preRollFrames());
      long first = Math.max(desiredFirst, retainedFirst);
      DiagnosticAudioRing.Snapshot audio = audioRing.snapshot(first, desiredEnd);
      iterator.remove();
      frozen.add(new FrozenEvidence(event, audio));
    }
  }
}
