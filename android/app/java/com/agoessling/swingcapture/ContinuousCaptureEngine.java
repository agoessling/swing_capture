package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ContinuousAudioImpactDetector;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.audio.Pcm16EvidenceRing;
import com.agoessling.swingcapture.diagnostics.DiagnosticAudioRing;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import com.agoessling.swingcapture.retention.EncodedAccessUnitRetention;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalInt;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;

/** Continuously encodes camera video, detects local impacts, and publishes leased ring snapshots. */
public final class ContinuousCaptureEngine {
  private static final String TAG = "ContinuousCapture";
  private static final int FRAMES_PER_SECOND = 240;
  private static final int AUDIO_SAMPLE_RATE_HZ = 48_000;
  private static final long START_TIMEOUT_NANOS = TimeUnit.SECONDS.toNanos(6);
  private static final long THREAD_STOP_TIMEOUT_MILLIS = 5_000;
  private static final long AUDIO_TIMESTAMP_BASE_UNCERTAINTY_NANOS = 250_000L;
  private static final long AUDIO_EVIDENCE_WAIT_NANOS = TimeUnit.MILLISECONDS.toNanos(750);
  private static final long PEER_LOCAL_CANDIDATE_MAX_AGE_NANOS =
      TimeUnit.MILLISECONDS.toNanos(250);
  private static final long PEER_CLOCK_MATCH_BASE_TOLERANCE_NANOS =
      TimeUnit.MILLISECONDS.toNanos(80);
  private static final long PEER_CLOCK_CANDIDATE_WAIT_NANOS =
      TimeUnit.MILLISECONDS.toNanos(150);
  private static final int PEER_AUDIO_CANDIDATE_CAPACITY = 32;
  private static final int DIAGNOSTIC_AUDIO_PRE_ROLL_FRAMES = 10 * AUDIO_SAMPLE_RATE_HZ;
  private static final int DIAGNOSTIC_AUDIO_POST_ROLL_FRAMES = 2 * AUDIO_SAMPLE_RATE_HZ;
  private static final long DIAGNOSTIC_AUDIO_WAIT_NANOS = TimeUnit.MILLISECONDS.toNanos(2_500);
  private static final int RETENTION_BYTES = 48 * 1024 * 1024;
  private static final int RETENTION_BLOCK_BYTES = 16 * 1024;
  private static final int RETENTION_ACCESS_UNITS = 1_200;
  private static final String DIAGNOSTIC_STATUS_NOT_AVAILABLE = "not_available";
  private static final String DIAGNOSTIC_STATUS_AVAILABLE = "available";
  private static final String DIAGNOSTIC_STATUS_PUBLICATION_FAILED = "publication_failed";
  private static final int MAXIMUM_LOGGED_FAILURE_TYPE_CHARACTERS = 160;

  /** Thread-safe callbacks; implementations must return promptly. */
  public interface Listener {
    void onReady();

    void onRingMetrics(long videoFrames, long audioFrames, long bytes, long durationUs);

    void onAudioMetrics(
        long audioFrames, float peakAmplitude, float noiseFloor, float threshold);

    void onTriggerAccepted(
        String sessionId,
        long triggerTimestampNanos,
        long timestampUncertaintyNanos,
        String source);

    void onPublishing(String sessionId);

    void onPublished(String sessionId);

    void onTriggerRejected(String source, String reason);

    void onFailure(Throwable failure);
  }

  /** Admission response used by the foreground-service control executor. */
  public static final class TriggerAttempt {
    private final boolean accepted;
    private final String sessionId;
    private final String diagnostic;

    private TriggerAttempt(boolean accepted, String sessionId, String diagnostic) {
      this.accepted = accepted;
      this.sessionId = sessionId;
      this.diagnostic = diagnostic;
    }

    public boolean accepted() {
      return accepted;
    }

    public String sessionId() {
      return sessionId;
    }

    public String diagnostic() {
      return diagnostic;
    }
  }

  private static final class TriggerEvidence {
    private final String source;
    private final long confirmationTimestampNanos;
    private final long timestampUncertaintyNanos;
    private final int sampleRateHz;
    private final long strikeFramePosition;
    private final float peakAmplitude;
    private final float noiseFloor;
    private final float threshold;
    private final PeerImpactMappingManifest peerImpactMapping;

    private TriggerEvidence(
        String source,
        long confirmationTimestampNanos,
        long timestampUncertaintyNanos,
        int sampleRateHz,
        long strikeFramePosition,
        float peakAmplitude,
        float noiseFloor,
        float threshold,
        PeerImpactMappingManifest peerImpactMapping) {
      this.source = source;
      this.confirmationTimestampNanos = confirmationTimestampNanos;
      this.timestampUncertaintyNanos = timestampUncertaintyNanos;
      this.sampleRateHz = sampleRateHz;
      this.strikeFramePosition = strikeFramePosition;
      this.peakAmplitude = peakAmplitude;
      this.noiseFloor = noiseFloor;
      this.threshold = threshold;
      this.peerImpactMapping = peerImpactMapping;
    }

    private static TriggerEvidence operator(
        String source, long timestampNanos, long audioFramePosition) {
      if (!source.equals("manual")
          && !source.equals("missed_shot")
          && !source.equals("pose_armed_no_impact")
          && !source.equals("peer_audio_arrival")) {
        throw new IllegalArgumentException("unsupported operator trigger source");
      }
      return new TriggerEvidence(
          source,
          timestampNanos,
          0,
          AUDIO_SAMPLE_RATE_HZ,
          audioFramePosition,
          Float.NaN,
          Float.NaN,
          Float.NaN,
          null);
    }
  }

  private record AudioTimestampAnchor(
      long framePosition, long boottimeNanos, long uncertaintyNanos) {}

  private static final class PendingCapture {
    private final String sessionId;
    private final TriggerEvidence evidence;
    private final long encoderToSensorOffsetNanos;
    private final long timestampOffsetSpanNanos;
    private final int timestampPairCount;

    private PendingCapture(
        String sessionId,
        TriggerEvidence evidence,
        long encoderToSensorOffsetNanos,
        long timestampOffsetSpanNanos,
        int timestampPairCount) {
      this.sessionId = sessionId;
      this.evidence = evidence;
      this.encoderToSensorOffsetNanos = encoderToSensorOffsetNanos;
      this.timestampOffsetSpanNanos = timestampOffsetSpanNanos;
      this.timestampPairCount = timestampPairCount;
    }
  }

  private final Context context;
  private final CaptureConfigurationSnapshot captureConfiguration;
  private final CaptureProfile profile;
  private final String sharedSessionId;
  private final long armRequestedElapsedRealtimeNanos;
  private final AutomaticTriggerReadinessGate automaticTriggerReadinessGate;
  private final Listener listener;
  private final WarmCameraLease warmCameraLease;
  private final PreviewEvidenceRing.Snapshot poseDiagnosticSnapshot;
  private final PeerArmStatusTracker peerArmStatus;
  private final EncodedAccessUnitRetention retention;
  private final StreamingTimestampCalibrator timestampCalibrator;
  private final long expectedEncoderToSensorOffsetNanos;
  private final ContinuousAudioImpactDetector impactDetector;
  private final Pcm16EvidenceRing pcmEvidenceRing;
  private final DiagnosticAudioRing diagnosticAudioRing =
      new DiagnosticAudioRing(
          DiagnosticAudioRing.recommendedCapacityFrames(
              AUDIO_SAMPLE_RATE_HZ, DiagnosticAudioRing.RECOMMENDED_RETENTION_SECONDS));
  private final Object pcmEvidenceMonitor = new Object();
  private final Object peerAudioCandidateMonitor = new Object();
  private final Object triggerAdmissionMonitor = new Object();
  private final Object startupTimingMonitor = new Object();
  private final ArrayDeque<ContinuousAudioImpactDetector.TimedImpact> peerAudioCandidates =
      new ArrayDeque<>(PEER_AUDIO_CANDIDATE_CAPACITY);
  private final Map<Long, PendingCapture> pendingCaptures = new ConcurrentHashMap<>();
  private final ExecutorService publisher = Executors.newSingleThreadExecutor();
  private final AtomicBoolean stopping = new AtomicBoolean();
  private final AtomicBoolean failed = new AtomicBoolean();
  private final AtomicBoolean automaticTriggersEnabled = new AtomicBoolean();
  private final AtomicLong audioFrames = new AtomicLong();
  private final AtomicLong encodedFrames = new AtomicLong();
  private final AtomicLong firstCameraFrameElapsedRealtimeNanos = new AtomicLong();
  private final AtomicLong firstUsableEncodedFrameElapsedRealtimeNanos = new AtomicLong();
  private final AtomicLong startupContinuityResetCount = new AtomicLong();
  private final AtomicLong maximumStartupContinuityGapNanos = new AtomicLong();
  private final AtomicLong rejectedAudioTimestamps = new AtomicLong();
  private final AtomicReference<AudioTimestampAnchor> latestAudioTimestamp =
      new AtomicReference<>();
  private final AtomicReference<ContinuousAudioImpactDetector.TimedImpact>
      latestValidatedAudioImpact = new AtomicReference<>();
  private volatile long firstRetainedSensorTimestampNanos;
  private volatile long lastRetainedSensorTimestampNanos;
  private volatile MediaFormat encoderOutputFormat;
  private volatile String cameraId = "";
  private volatile int cameraTimestampSource = -1;
  private volatile long engineStartedElapsedRealtimeNanos;
  private volatile long fullPreRollReadyElapsedRealtimeNanos;
  private volatile int audioSource = -1;
  private MediaCodec encoder;
  private Surface encoderInputSurface;
  private Thread encoderThread;
  private AudioRecord audioRecord;
  private Thread audioThread;
  private HandlerThread cameraThread;
  private CameraDevice cameraDevice;
  private CameraConstrainedHighSpeedCaptureSession cameraSession;
  private final CountDownLatch firstCameraFrame = new CountDownLatch(1);
  private final CountDownLatch audioStarted = new CountDownLatch(1);

  public ContinuousCaptureEngine(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      String sharedSessionId,
      boolean audioHilMode,
      boolean automaticAudioTriggersRequested,
      Listener listener) {
    this(
        context,
        captureConfiguration,
        sharedSessionId,
        audioHilMode,
        automaticAudioTriggersRequested,
        null,
        null,
        null,
        false,
        SystemClock.elapsedRealtimeNanos(),
        listener);
  }

  /** Creates a direct-start engine tied to the service's immutable arm-request milestone. */
  public ContinuousCaptureEngine(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      String sharedSessionId,
      boolean audioHilMode,
      boolean automaticAudioTriggersRequested,
      long armRequestedElapsedRealtimeNanos,
      Listener listener) {
    this(
        context,
        captureConfiguration,
        sharedSessionId,
        audioHilMode,
        automaticAudioTriggersRequested,
        null,
        null,
        null,
        false,
        armRequestedElapsedRealtimeNanos,
        listener);
  }

  public ContinuousCaptureEngine(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      String sharedSessionId,
      boolean audioHilMode,
      boolean automaticAudioTriggersRequested,
      WarmCameraLease warmCameraLease,
      Listener listener) {
    this(
        context,
        captureConfiguration,
        sharedSessionId,
        audioHilMode,
        automaticAudioTriggersRequested,
        warmCameraLease,
        null,
        null,
        false,
        SystemClock.elapsedRealtimeNanos(),
        listener);
  }

  /**
   * Creates an engine with optional immutable pose-preview evidence for best-effort publication.
   * Existing constructors retain their behavior by supplying no pose diagnostic snapshot.
   */
  public ContinuousCaptureEngine(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      String sharedSessionId,
      boolean audioHilMode,
      boolean automaticAudioTriggersRequested,
      WarmCameraLease warmCameraLease,
      PreviewEvidenceRing.Snapshot poseDiagnosticSnapshot,
      PeerArmStatusTracker peerArmStatus,
      boolean peerReadinessRequired,
      Listener listener) {
    this(
        context,
        captureConfiguration,
        sharedSessionId,
        audioHilMode,
        automaticAudioTriggersRequested,
        warmCameraLease,
        poseDiagnosticSnapshot,
        peerArmStatus,
        peerReadinessRequired,
        SystemClock.elapsedRealtimeNanos(),
        listener);
  }

  /**
   * Creates an engine with an immutable service-observed arm-request milestone.
   *
   * <p>The explicit milestone prevents a later monitoring restart from relabeling a retained
   * session while its publisher thread is still completing.
   */
  public ContinuousCaptureEngine(
      Context context,
      CaptureConfigurationSnapshot captureConfiguration,
      String sharedSessionId,
      boolean audioHilMode,
      boolean automaticAudioTriggersRequested,
      WarmCameraLease warmCameraLease,
      PreviewEvidenceRing.Snapshot poseDiagnosticSnapshot,
      PeerArmStatusTracker peerArmStatus,
      boolean peerReadinessRequired,
      long armRequestedElapsedRealtimeNanos,
      Listener listener) {
    if (armRequestedElapsedRealtimeNanos < 0) {
      throw new IllegalArgumentException("arm request timestamp cannot be negative");
    }
    this.context = context.getApplicationContext();
    this.captureConfiguration = captureConfiguration;
    this.profile = captureConfiguration.profile();
    this.sharedSessionId = sharedSessionId;
    this.armRequestedElapsedRealtimeNanos = armRequestedElapsedRealtimeNanos;
    this.automaticTriggerReadinessGate =
        new AutomaticTriggerReadinessGate(
            automaticAudioTriggersRequested, peerReadinessRequired);
    this.warmCameraLease = warmCameraLease;
    if (warmCameraLease == null) {
      expectedEncoderToSensorOffsetNanos = 0;
      timestampCalibrator = new StreamingTimestampCalibrator();
    } else {
      long monotonicBeforeNanos = System.nanoTime();
      long boottimeNanos = SystemClock.elapsedRealtimeNanos();
      long monotonicAfterNanos = System.nanoTime();
      expectedEncoderToSensorOffsetNanos =
          WarmCaptureTransitionTiming.midpointClockOffsetNanos(
              monotonicBeforeNanos, boottimeNanos, monotonicAfterNanos);
      timestampCalibrator =
          new StreamingTimestampCalibrator(expectedEncoderToSensorOffsetNanos);
    }
    this.poseDiagnosticSnapshot = poseDiagnosticSnapshot;
    this.peerArmStatus = peerArmStatus;
    this.listener = listener;
    this.impactDetector =
        new ContinuousAudioImpactDetector(
            DeviceAudioDetectorPolicy.forDevice(Build.MANUFACTURER, Build.MODEL),
            AudioTimestampMapper.Config.defaults());
    this.pcmEvidenceRing = AudioEvidencePolicy.ringForCapture(audioHilMode);
    retention =
        new EncodedAccessUnitRetention(
            new EncodedAccessUnitRetention.Limits(
                RETENTION_BYTES,
                RETENTION_BLOCK_BYTES,
                TimeUnit.SECONDS.toNanos(4),
                RETENTION_ACCESS_UNITS,
                2,
                EncodedAccessUnitRetention.SWING_PRE_ROLL_NS,
                EncodedAccessUnitRetention.SWING_POST_ROLL_NS,
                TimeUnit.MILLISECONDS.toNanos(750),
                TimeUnit.MILLISECONDS.toNanos(20),
                TimeUnit.MILLISECONDS.toNanos(250)));
  }

  /** Starts hardware and waits until the ring contains a triggerable pre-roll and preceding IDR. */
  public void start() throws Exception {
    engineStartedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
    requirePermissions();
    captureConfiguration.requireAssignedRole();
    try {
      startEncoder();
      startAudio();
      startCamera();
      long deadline = SystemClock.elapsedRealtimeNanos() + START_TIMEOUT_NANOS;
      while (!triggerReady() && SystemClock.elapsedRealtimeNanos() < deadline) {
        throwIfFailed();
        Thread.sleep(10);
      }
      if (!triggerReady()) {
        String ordinalShift;
        try {
          ordinalShift = Long.toString(timestampCalibrator.cameraToEncoderOrdinalShift());
        } catch (IllegalStateException unavailable) {
          ordinalShift = "unavailable";
        }
        throw new IllegalStateException(
            "Timed out filling a continuous encoded pre-roll (timestamp_evidence="
                + timestampCalibrator.evidenceCount()
                + ", camera_to_encoder_ordinal_shift="
                + ordinalShift
                + ", timestamp_samples="
                + timestampCalibrator.diagnosticSnapshot(0)
                + ")");
      }
      fullPreRollReadyElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
      boolean automaticTriggersReady = automaticTriggerReadinessGate.markLocalReady();
      listener.onReady();
      synchronized (triggerAdmissionMonitor) {
        automaticTriggersEnabled.set(
            automaticTriggersReady
                && pendingCaptures.isEmpty()
                && !stopping.get()
                && !failed.get());
      }
    } catch (Throwable failure) {
      stop();
      if (failure instanceof Exception exception) {
        throw exception;
      }
      throw new IllegalStateException("Unable to start continuous capture", failure);
    }
  }

  public TriggerAttempt triggerManual(String requestedSessionId) {
    return triggerOperator(requestedSessionId, "manual");
  }

  public TriggerAttempt triggerMissedShot(String requestedSessionId) {
    return triggerOperator(requestedSessionId, "missed_shot");
  }

  /** Releases a paired leader after peer readiness or a bounded degraded-mode resolution. */
  void resolvePeerArmForAutomaticTriggers(
      String expectedSharedSessionId,
      AutomaticTriggerReadinessGate.PeerArmResolution resolution) {
    if (!sharedSessionId.equals(expectedSharedSessionId)) {
      return;
    }
    boolean automaticTriggersReady = automaticTriggerReadinessGate.resolvePeerArm(resolution);
    synchronized (triggerAdmissionMonitor) {
      if (automaticTriggersReady
          && pendingCaptures.isEmpty()
          && !stopping.get()
          && !failed.get()) {
        automaticTriggersEnabled.set(true);
      }
    }
  }

  boolean activeEvidenceTimeoutAllowed() {
    return automaticTriggerReadinessGate.activeEvidenceTimeoutAllowed();
  }

  /** Retains bounded evidence when pose armed high-speed capture but no impact followed. */
  public TriggerAttempt triggerPoseArmedNoImpact(String requestedSessionId) {
    return triggerOperator(requestedSessionId, "pose_armed_no_impact");
  }

  /** Freezes a shadow ring while retaining the original request and its effective policy result. */
  public TriggerAttempt triggerPeerImpact(
      PosePeerArmClient.ImpactTrigger originalPeerTrigger,
      PosePeerArmClient.ImpactTrigger effectivePeerTrigger) {
    String requestedSessionId = effectivePeerTrigger.sharedSessionId();
    if (!sharedSessionId.equals(requestedSessionId)) {
      return rejected("peer_audio", "peer impact session does not match the active arm");
    }
    long requestArrivalNanos = SystemClock.elapsedRealtimeNanos();
    if (effectivePeerTrigger.hasClockMapping()) {
      ContinuousAudioImpactDetector.TimedImpact matched =
          awaitClockMatchedCandidate(
              effectivePeerTrigger.mappedPeerTriggerElapsedRealtimeNanos(),
              effectivePeerTrigger.mappingUncertaintyNanos(),
              requestArrivalNanos);
      if (matched != null) {
        return triggerPeerLocalCandidate(
            requestedSessionId,
            matched,
            "peer_audio_clock_candidate",
            originalPeerTrigger,
            effectivePeerTrigger,
            requestArrivalNanos);
      }
      // A valid mapping with no nearby local evidence is safer than selecting an unrelated
      // precursor. Retain using the audited arrival timestamp instead.
      return triggerPeerArrival(
          requestedSessionId,
          requestArrivalNanos,
          originalPeerTrigger,
          effectivePeerTrigger);
    }

    ContinuousAudioImpactDetector.TimedImpact local = latestValidatedAudioImpact.get();
    if (local != null) {
      AudioTimestampMapper.Estimate strike = local.strikeTime().orElseThrow();
      if (PeerAudioImpactSelection.isFreshLocalCandidate(
          requestArrivalNanos,
          strike.boottimeNanos(),
          PEER_LOCAL_CANDIDATE_MAX_AGE_NANOS)) {
        return triggerPeerLocalCandidate(
            requestedSessionId,
            local,
            "peer_audio_local_candidate",
            originalPeerTrigger,
            effectivePeerTrigger,
            requestArrivalNanos);
      }
    }
    return triggerPeerArrival(
        requestedSessionId, requestArrivalNanos, originalPeerTrigger, effectivePeerTrigger);
  }

  private TriggerAttempt triggerPeerLocalCandidate(
      String requestedSessionId,
      ContinuousAudioImpactDetector.TimedImpact local,
      String source,
      PosePeerArmClient.ImpactTrigger originalPeerTrigger,
      PosePeerArmClient.ImpactTrigger effectivePeerTrigger,
      long requestArrivalNanos) {
    AudioTimestampMapper.Estimate strike = local.strikeTime().orElseThrow();
    AudioTimestampMapper.Estimate confirmation = local.confirmationTime().orElseThrow();
    TriggerEvidence evidence =
        new TriggerEvidence(
            source,
            confirmation.boottimeNanos(),
            strike.uncertaintyNanos(),
            AUDIO_SAMPLE_RATE_HZ,
            local.impact().strikeFramePosition(),
            local.impact().peakAmplitude(),
            local.impact().noiseFloorAtDetection(),
            local.impact().thresholdAtDetection(),
            PeerImpactMappingManifest.create(
                originalPeerTrigger,
                effectivePeerTrigger,
                source,
                requestArrivalNanos,
                strike.boottimeNanos(),
                strike.uncertaintyNanos()));
    return trigger(requestedSessionId, strike.boottimeNanos(), evidence);
  }

  private TriggerAttempt triggerPeerArrival(
      String requestedSessionId,
      long requestArrivalNanos,
      PosePeerArmClient.ImpactTrigger originalPeerTrigger,
      PosePeerArmClient.ImpactTrigger effectivePeerTrigger) {
    long audioFramePosition = -1;
    AudioTimestampAnchor anchor = latestAudioTimestamp.get();
    if (anchor != null) {
      audioFramePosition =
          AudioFrameMarker.estimate(
                  requestArrivalNanos,
                  anchor.framePosition(),
                  anchor.boottimeNanos(),
                  anchor.uncertaintyNanos(),
                  AUDIO_SAMPLE_RATE_HZ)
              .map(AudioFrameMarker.Marker::framePosition)
              .orElse(-1L);
    }
    TriggerEvidence evidence =
        new TriggerEvidence(
            "peer_audio_arrival",
            requestArrivalNanos,
            0,
            AUDIO_SAMPLE_RATE_HZ,
            audioFramePosition,
            Float.NaN,
            Float.NaN,
            Float.NaN,
            PeerImpactMappingManifest.create(
                originalPeerTrigger,
                effectivePeerTrigger,
                "peer_audio_arrival",
                requestArrivalNanos,
                requestArrivalNanos,
                0));
    return trigger(requestedSessionId, requestArrivalNanos, evidence);
  }

  private ContinuousAudioImpactDetector.TimedImpact awaitClockMatchedCandidate(
      long mappedLeaderStrikeNanos,
      long clockUncertaintyNanos,
      long requestArrivalNanos) {
    long deadlineNanos = Math.addExact(requestArrivalNanos, PEER_CLOCK_CANDIDATE_WAIT_NANOS);
    return MonitorCandidateWait.until(
        peerAudioCandidateMonitor,
        SystemClock::elapsedRealtimeNanos,
        deadlineNanos,
        () -> stopping.get() || failed.get(),
        () -> closestClockMatchedCandidate(mappedLeaderStrikeNanos, clockUncertaintyNanos));
  }

  /** Called only while {@link #peerAudioCandidateMonitor} is held. */
  private ContinuousAudioImpactDetector.TimedImpact closestClockMatchedCandidate(
      long mappedLeaderStrikeNanos, long clockUncertaintyNanos) {
    ArrayList<ContinuousAudioImpactDetector.TimedImpact> impacts =
        new ArrayList<>(peerAudioCandidates);
    ArrayList<PeerAudioImpactSelection.Candidate> candidates = new ArrayList<>(impacts.size());
    for (ContinuousAudioImpactDetector.TimedImpact impact : impacts) {
      AudioTimestampMapper.Estimate strike = impact.strikeTime().orElseThrow();
      candidates.add(
          new PeerAudioImpactSelection.Candidate(
              strike.boottimeNanos(), strike.uncertaintyNanos()));
    }
    OptionalInt selected =
        PeerAudioImpactSelection.closestCandidateIndex(
            candidates,
            mappedLeaderStrikeNanos,
            PEER_CLOCK_MATCH_BASE_TOLERANCE_NANOS,
            clockUncertaintyNanos);
    return selected.isPresent() ? impacts.get(selected.orElseThrow()) : null;
  }

  private TriggerAttempt triggerOperator(String requestedSessionId, String source) {
    long now = SystemClock.elapsedRealtimeNanos();
    long audioFramePosition = -1;
    AudioTimestampAnchor anchor = latestAudioTimestamp.get();
    if (anchor != null) {
      audioFramePosition =
          AudioFrameMarker.estimate(
                  now,
                  anchor.framePosition(),
                  anchor.boottimeNanos(),
                  anchor.uncertaintyNanos(),
                  AUDIO_SAMPLE_RATE_HZ)
              .map(AudioFrameMarker.Marker::framePosition)
              .orElse(-1L);
    }
    return trigger(
        requestedSessionId, now, TriggerEvidence.operator(source, now, audioFramePosition));
  }

  public Optional<CaptureStartupTiming> startupTiming() {
    return startupTiming(armRequestedElapsedRealtimeNanos);
  }

  /** Legacy compatibility overload; new callers should use the engine-owned milestone. */
  public Optional<CaptureStartupTiming> startupTiming(long armRequestedElapsedRealtimeNanos) {
    long engineStarted = engineStartedElapsedRealtimeNanos;
    long firstCameraFrame = firstCameraFrameElapsedRealtimeNanos.get();
    long firstUsableEncodedFrame = firstUsableEncodedFrameElapsedRealtimeNanos.get();
    long fullPreRollReady = fullPreRollReadyElapsedRealtimeNanos;
    if (engineStarted == 0
        || firstCameraFrame == 0
        || firstUsableEncodedFrame == 0
        || fullPreRollReady == 0) {
      return Optional.empty();
    }
    return Optional.of(
        new CaptureStartupTiming(
            armRequestedElapsedRealtimeNanos,
            engineStarted,
            firstCameraFrame,
            firstUsableEncodedFrame,
            fullPreRollReady));
  }

  public Optional<CaptureStartupTimingManifest> startupTimingManifest() {
    Optional<CaptureStartupTiming> startupTiming = startupTiming();
    if (startupTiming.isEmpty()) {
      return Optional.empty();
    }
    synchronized (startupTimingMonitor) {
      return Optional.of(
          new CaptureStartupTimingManifest(
              startupTiming.orElseThrow(),
              startupContinuityResetCount.get(),
              maximumStartupContinuityGapNanos.get()));
    }
  }

  public long startupContinuityResetCount() {
    return startupContinuityResetCount.get();
  }

  public long maximumStartupContinuityGapNanos() {
    return maximumStartupContinuityGapNanos.get();
  }

  /** Stops automatic impacts from racing the explicit terminal trigger of a soak HIL run. */
  void disableAutomaticTriggersForHil() {
    synchronized (triggerAdmissionMonitor) {
      automaticTriggersEnabled.set(false);
    }
  }

  public void stop() {
    synchronized (triggerAdmissionMonitor) {
      if (!stopping.compareAndSet(false, true)) {
        automaticTriggersEnabled.set(false);
        return;
      }
      automaticTriggersEnabled.set(false);
    }
    synchronized (peerAudioCandidateMonitor) {
      peerAudioCandidateMonitor.notifyAll();
    }
    stopCamera();
    stopAudio();
    stopEncoder();
    publisher.shutdown();
    try {
      if (!publisher.awaitTermination(THREAD_STOP_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS)) {
        fail(new IllegalStateException("Session publisher did not stop"));
      }
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      fail(interrupted);
    }
  }

  private TriggerAttempt trigger(
      String requestedSessionId, long triggerTimestampNanos, TriggerEvidence evidence) {
    return trigger(requestedSessionId, triggerTimestampNanos, evidence, false);
  }

  private TriggerAttempt trigger(
      String requestedSessionId,
      long triggerTimestampNanos,
      TriggerEvidence evidence,
      boolean automaticTrigger) {
    String sessionId;
    String rejection = null;
    synchronized (triggerAdmissionMonitor) {
      if (automaticTrigger && !automaticTriggersEnabled.get()) {
        return new TriggerAttempt(false, "", "automatic trigger gate is closed");
      }
      sessionId =
          requestedSessionId == null || requestedSessionId.isBlank()
              ? newSessionId()
              : requestedSessionId;
      if (!triggerReady()) {
        rejection = "the encoded pre-roll is not ready";
      } else {
        EncodedAccessUnitRetention.TriggerResult result = retention.trigger(triggerTimestampNanos);
        if (!result.accepted()) {
          rejection =
              result.status().name().toLowerCase(java.util.Locale.ROOT)
                  + " (trigger="
                  + triggerTimestampNanos
                  + ", first="
                  + firstRetainedSensorTimestampNanos
                  + ", last="
                  + lastRetainedSensorTimestampNanos
                  + ", offset="
                  + timestampCalibrator.medianOffsetNanos()
                  + ")";
        } else {
          // A physical arm cycle owns one swing and one shared coordination ID. Reject any later
          // automatic impact while post-roll/publication is in flight; the service stops after a
          // normal publication and the coordinator must arm both nodes again with a fresh shared
          // ID.
          automaticTriggersEnabled.set(false);
          pendingCaptures.put(
              result.captureId(),
              new PendingCapture(
                  sessionId,
                  evidence,
                  timestampCalibrator.medianOffsetNanos(),
                  timestampCalibrator.offsetSpanNanos(),
                  timestampCalibrator.evidenceCount()));
        }
      }
    }
    if (rejection != null) {
      return rejected(evidence.source, rejection);
    }
    listener.onTriggerAccepted(
        sessionId,
        triggerTimestampNanos,
        evidence.timestampUncertaintyNanos,
        evidence.source);
    return new TriggerAttempt(true, sessionId, "accepted");
  }

  private TriggerAttempt rejected(String source, String diagnostic) {
    listener.onTriggerRejected(source, diagnostic);
    return new TriggerAttempt(false, "", diagnostic);
  }

  private void handleAudioImpact(ContinuousAudioImpactDetector.TimedImpact timedImpact) {
    if (!timedImpact.hasValidatedTiming()) {
      return;
    }
    latestValidatedAudioImpact.set(timedImpact);
    synchronized (peerAudioCandidateMonitor) {
      if (peerAudioCandidates.size() == PEER_AUDIO_CANDIDATE_CAPACITY) {
        peerAudioCandidates.removeFirst();
      }
      peerAudioCandidates.addLast(timedImpact);
      peerAudioCandidateMonitor.notifyAll();
    }
    AudioTimestampMapper.Estimate strike = timedImpact.strikeTime().orElseThrow();
    AudioTimestampMapper.Estimate confirmation = timedImpact.confirmationTime().orElseThrow();
    TriggerEvidence evidence =
        new TriggerEvidence(
            "local_audio",
            confirmation.boottimeNanos(),
            strike.uncertaintyNanos(),
            AUDIO_SAMPLE_RATE_HZ,
            timedImpact.impact().strikeFramePosition(),
            timedImpact.impact().peakAmplitude(),
            timedImpact.impact().noiseFloorAtDetection(),
            timedImpact.impact().thresholdAtDetection(),
            null);
    trigger(null, strike.boottimeNanos(), evidence, true);
  }

  private boolean triggerReady() {
    long first = firstRetainedSensorTimestampNanos;
    long last = lastRetainedSensorTimestampNanos;
    return !stopping.get()
        && !failed.get()
        && (automaticTriggersEnabled.get()
            || (timestampCalibrator.valid()
            && first > 0
            && last - first >= TimeUnit.MILLISECONDS.toNanos(2_450)));
  }

  private void startEncoder() throws Exception {
    String codecName = selectEncoder();
    encoder = MediaCodec.createByCodecName(codecName);
    MediaFormat format =
        MediaFormat.createVideoFormat(
            MediaFormat.MIMETYPE_VIDEO_AVC, profile.width(), profile.height());
    format.setInteger(
        MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
    format.setInteger(MediaFormat.KEY_BIT_RATE, profile.bitrateBitsPerSecond());
    format.setInteger(MediaFormat.KEY_FRAME_RATE, FRAMES_PER_SECOND);
    format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
    format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
    format.setInteger(MediaFormat.KEY_PRIORITY, 0);
    format.setFloat(MediaFormat.KEY_OPERATING_RATE, FRAMES_PER_SECOND);
    encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
    encoderInputSurface = encoder.createInputSurface();
    encoder.start();
    encoderThread = new Thread(this::drainEncoder, "continuous-video-encoder");
    encoderThread.start();
  }

  private void drainEncoder() {
    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
    long ordinal = 0;
    try {
      while (!stopping.get()) {
        int index = encoder.dequeueOutputBuffer(info, 20_000);
        if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
          encoderOutputFormat = encoder.getOutputFormat();
        } else if (index >= 0) {
          try {
            if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
              ByteBuffer output = encoder.getOutputBuffer(index);
              if (output == null) {
                throw new IllegalStateException("Encoder returned a null output buffer");
              }
              timestampCalibrator.observeEncoder(ordinal, info.presentationTimeUs);
              if (timestampCalibrator.valid()) {
                long sensorTimestampNanos =
                    timestampCalibrator.sensorTimestampNanos(info.presentationTimeUs);
                ByteBuffer accessUnit = output.duplicate();
                accessUnit.position(info.offset);
                accessUnit.limit(info.offset + info.size);
                try {
                  retention.append(
                      ordinal,
                      info.presentationTimeUs,
                      sensorTimestampNanos,
                      info.flags,
                      (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0,
                      accessUnit);
                  firstUsableEncodedFrameElapsedRealtimeNanos.compareAndSet(
                      0, SystemClock.elapsedRealtimeNanos());
                } catch (EncodedAccessUnitRetention.RetentionException discontinuity) {
                  StreamingTimestampCalibrator.DiagnosticSnapshot timestampDiagnostic = null;
                  if (discontinuity.continuityDiagnostic().isPresent()) {
                    long failingOrdinal =
                        discontinuity.continuityDiagnostic().orElseThrow().current().ordinal();
                    try {
                      timestampDiagnostic =
                          timestampCalibrator.diagnosticSnapshot(failingOrdinal);
                    } catch (RuntimeException diagnosticFailure) {
                      discontinuity.addSuppressed(diagnosticFailure);
                    }
                  }
                  boolean retentionCaptureActive =
                      retention.captureState()
                          != EncodedAccessUnitRetention.CaptureState.IDLE;
                  CaptureStartupContinuityPolicy.Action action =
                      CaptureStartupContinuityPolicy.action(
                          discontinuity.failure()
                              == EncodedAccessUnitRetention.Failure.SENSOR_TIMESTAMP_GAP,
                          fullPreRollReadyElapsedRealtimeNanos != 0, retentionCaptureActive);
                  if (action
                      == CaptureStartupContinuityPolicy.Action.RESET_WARMUP_AND_CONTINUE) {
                    long gapNanos =
                        discontinuity
                            .continuityDiagnostic()
                            .map(
                                EncodedAccessUnitRetention.ContinuityDiagnostic
                                    ::sensorTimestampGapNs)
                            .orElse(0L);
                    synchronized (startupTimingMonitor) {
                      maximumStartupContinuityGapNanos.accumulateAndGet(gapNanos, Math::max);
                      startupContinuityResetCount.incrementAndGet();
                    }
                    Log.w(TAG, "Discarding discontinuous encoder warmup: " + discontinuity);
                    retention.resetContinuity();
                    firstRetainedSensorTimestampNanos = 0;
                    lastRetainedSensorTimestampNanos = 0;
                    firstUsableEncodedFrameElapsedRealtimeNanos.set(0);
                    ++ordinal;
                    continue;
                  }
                  ContinuousCaptureContinuityException.resetAndThrow(
                      discontinuity, timestampDiagnostic, retention::resetContinuity);
                }
                if (firstRetainedSensorTimestampNanos == 0) {
                  firstRetainedSensorTimestampNanos = sensorTimestampNanos;
                }
                lastRetainedSensorTimestampNanos = sensorTimestampNanos;
                long videoCount = encodedFrames.incrementAndGet();
                listener.onRingMetrics(
                    videoCount,
                    audioFrames.get(),
                    retention.retainedBytes(),
                    (retention.newestRetainedSensorTimestampNs()
                            - retention.oldestRetainedSensorTimestampNs())
                        / 1_000);
                scheduleCompletedSnapshot();
              }
              ++ordinal;
            }
          } finally {
            encoder.releaseOutputBuffer(index, false);
          }
        }
      }
    } catch (Throwable failure) {
      fail(failure);
    }
  }

  private void scheduleCompletedSnapshot() {
    EncodedAccessUnitRetention.Snapshot snapshot = retention.pollCompletedSnapshot();
    if (snapshot == null) {
      return;
    }
    PendingCapture pending = pendingCaptures.remove(snapshot.captureId());
    if (pending == null) {
      snapshot.close();
      fail(new IllegalStateException("Completed retention snapshot has no trigger metadata"));
      return;
    }
    try {
      publisher.execute(() -> publishSnapshot(snapshot, pending));
    } catch (Throwable rejected) {
      snapshot.close();
      fail(rejected);
    }
  }

  private void publishSnapshot(
      EncodedAccessUnitRetention.Snapshot snapshot, PendingCapture pending) {
    listener.onPublishing(pending.sessionId);
    try (snapshot) {
      writeSession(snapshot, pending);
      listener.onPublished(pending.sessionId);
    } catch (Throwable failure) {
      fail(failure);
    }
  }

  private void writeSession(
      EncodedAccessUnitRetention.Snapshot snapshot, PendingCapture pending) throws Exception {
    Pcm16EvidenceRing.Snapshot audioEvidence = awaitAudioEvidence(pending.evidence);
    DiagnosticAudioRing.Snapshot diagnosticAudioEvidence =
        awaitDiagnosticAudioEvidence(pending.evidence);
    MediaFormat format = encoderOutputFormat;
    if (format == null) {
      throw new IllegalStateException("Encoder output format is unavailable");
    }
    AvcCodecDescriptor codec = AvcMediaFormat.describe(format);
    File sessions = new File(context.getFilesDir(), "sessions");
    if (!sessions.isDirectory() && !sessions.mkdirs()) {
      throw new IllegalStateException("Unable to create sessions directory " + sessions);
    }
    File temporaryDirectory = new File(sessions, pending.sessionId + ".tmp");
    if (!temporaryDirectory.mkdir()) {
      throw new IllegalStateException("Unable to create temporary session " + pending.sessionId);
    }
    boolean publicationSucceeded = false;
    try {
      SessionStagingCleanup.markOwned(sessions, temporaryDirectory);
      AndroidDirectorySync.synchronize(temporaryDirectory);
    String mediaName = captureConfiguration.mediaFileName();
    File temporaryMedia = new File(temporaryDirectory, mediaName + ".tmp");
    File media = new File(temporaryDirectory, mediaName);
    muxSnapshot(snapshot, format, temporaryMedia);
    try (FileOutputStream sync = new FileOutputStream(temporaryMedia, true)) {
      sync.getFD().sync();
    }
    if (!temporaryMedia.renameTo(media)) {
      throw new IllegalStateException("Unable to publish retained media");
    }
    Pcm16WavFile.EvidenceMetadata audioMetadata = null;
    if (audioEvidence != null) {
      audioMetadata = Pcm16WavFile.metadata(audioEvidence);
      File temporaryAudio = new File(temporaryDirectory, Pcm16WavFile.FILE_NAME + ".tmp");
      File publishedAudio = new File(temporaryDirectory, Pcm16WavFile.FILE_NAME);
      try (FileOutputStream fileOutput = new FileOutputStream(temporaryAudio);
          BufferedOutputStream output = new BufferedOutputStream(fileOutput)) {
        Pcm16WavFile.write(audioEvidence, output);
        output.flush();
        fileOutput.getFD().sync();
      }
      if (temporaryAudio.length() != audioMetadata.bytes()) {
        throw new IllegalStateException("PCM evidence byte count disagrees with WAV metadata");
      }
      if (!temporaryAudio.renameTo(publishedAudio)) {
        throw new IllegalStateException("Unable to publish PCM evidence WAV");
      }
    }
    DiagnosticPcm16WavFile.EvidenceMetadata diagnosticAudioMetadata = null;
    String diagnosticAudioStatus = "not_available";
    if (diagnosticAudioEvidence != null) {
      File temporaryAudio =
          new File(temporaryDirectory, DiagnosticPcm16WavFile.FILE_NAME + ".tmp");
      File publishedAudio = new File(temporaryDirectory, DiagnosticPcm16WavFile.FILE_NAME);
      try {
        diagnosticAudioMetadata =
            DiagnosticPcm16WavFile.metadata(
                diagnosticAudioEvidence, pending.evidence.strikeFramePosition);
        try (FileOutputStream fileOutput = new FileOutputStream(temporaryAudio);
            BufferedOutputStream output = new BufferedOutputStream(fileOutput)) {
          DiagnosticPcm16WavFile.write(diagnosticAudioEvidence, output);
          output.flush();
          fileOutput.getFD().sync();
        }
        if (temporaryAudio.length() != diagnosticAudioMetadata.bytes()) {
          throw new IllegalStateException(
              "Diagnostic PCM byte count disagrees with WAV metadata");
        }
        if (!temporaryAudio.renameTo(publishedAudio)) {
          throw new IllegalStateException("Unable to publish diagnostic PCM WAV");
        }
        diagnosticAudioStatus = "available";
      } catch (Throwable diagnosticFailure) {
        diagnosticAudioMetadata = null;
        diagnosticAudioStatus = "publication_failed";
        Log.e(TAG, "Unable to publish auxiliary diagnostic PCM", diagnosticFailure);
        if (temporaryAudio.exists() && !temporaryAudio.delete()) {
          Log.w(TAG, "Unable to remove failed diagnostic PCM temporary file");
        }
      }
    }
    String diagnosticIncidentStatus = "available";
    try {
      new SessionDiagnosticStore(AndroidDirectorySync::synchronize)
          .initialize(
              temporaryDirectory,
              pending.sessionId,
              captureConfiguration.nodeId(),
              pending.evidence.source,
              System.currentTimeMillis());
    } catch (Throwable diagnosticFailure) {
      diagnosticIncidentStatus = "publication_failed";
      Log.e(TAG, "Unable to publish auxiliary diagnostic incident", diagnosticFailure);
    }
    JSONObject poseDiagnosticMetadata = null;
    String poseDiagnosticStatus = DIAGNOSTIC_STATUS_NOT_AVAILABLE;
    if (poseDiagnosticSnapshot != null) {
      try {
        PoseDiagnosticFiles.ManifestMetadata publishedPoseDiagnostics =
            new PoseDiagnosticFiles(AndroidDirectorySync::synchronize)
                .publish(temporaryDirectory, poseDiagnosticSnapshot);
        poseDiagnosticMetadata =
            new JSONObject(publishedPoseDiagnostics.toCanonicalJson());
        poseDiagnosticStatus = DIAGNOSTIC_STATUS_AVAILABLE;
      } catch (Throwable diagnosticFailure) {
        poseDiagnosticMetadata = null;
        poseDiagnosticStatus = DIAGNOSTIC_STATUS_PUBLICATION_FAILED;
        Log.e(
            TAG,
            "Unable to publish auxiliary pose diagnostics; status="
                + DIAGNOSTIC_STATUS_PUBLICATION_FAILED
                + "; failure_type="
                + boundedFailureType(diagnosticFailure));
      }
    }
    JSONObject manifest =
        sessionManifest(
            snapshot,
            pending,
            mediaName,
            media.length(),
            codec,
            audioMetadata,
            diagnosticAudioMetadata,
            diagnosticAudioStatus,
            diagnosticIncidentStatus,
            poseDiagnosticMetadata,
            poseDiagnosticStatus);
    File temporaryManifest = new File(temporaryDirectory, "manifest.json.tmp");
    File publishedManifest = new File(temporaryDirectory, "manifest.json");
    try (FileOutputStream output = new FileOutputStream(temporaryManifest)) {
      output.write((manifest.toString(2) + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
      output.getFD().sync();
    }
    if (!temporaryManifest.renameTo(publishedManifest)) {
      throw new IllegalStateException("Unable to publish retained manifest");
    }
    AndroidDirectorySync.synchronize(temporaryDirectory);
    File publishedDirectory = new File(sessions, pending.sessionId);
    if (!temporaryDirectory.renameTo(publishedDirectory)) {
      throw new IllegalStateException("Unable to atomically publish session " + pending.sessionId);
    }
    AndroidDirectorySync.synchronize(sessions);
      publicationSucceeded = true;
    } finally {
      if (!publicationSucceeded && temporaryDirectory.exists()) {
        if (!SessionStagingCleanup.cleanupFailedPublication(sessions, temporaryDirectory)) {
          if (!SessionStagingCleanup.cleanupAfterMarkFailure(sessions, temporaryDirectory)) {
            Log.e(TAG, "Refused unsafe failed session staging cleanup: " + temporaryDirectory);
          }
        }
      }
    }
  }

  private Pcm16EvidenceRing.Snapshot awaitAudioEvidence(TriggerEvidence evidence)
      throws InterruptedException {
    if (!AudioEvidencePolicy.requiresPublishedEvidence(pcmEvidenceRing, evidence.source)
        || evidence.strikeFramePosition < 0) {
      return null;
    }
    long deadline = SystemClock.elapsedRealtimeNanos() + AUDIO_EVIDENCE_WAIT_NANOS;
    synchronized (pcmEvidenceMonitor) {
      while (true) {
        Pcm16EvidenceRing.SnapshotAvailability availability =
            pcmEvidenceRing.snapshotAvailability(evidence.strikeFramePosition);
        if (availability == Pcm16EvidenceRing.SnapshotAvailability.AVAILABLE) {
          return pcmEvidenceRing.snapshot(evidence.strikeFramePosition);
        }
        if (availability == Pcm16EvidenceRing.SnapshotAvailability.PRE_ROLL_UNAVAILABLE) {
          return pcmEvidenceRing.snapshot(evidence.strikeFramePosition);
        }
        long remaining = deadline - SystemClock.elapsedRealtimeNanos();
        if (remaining <= 0) {
          return pcmEvidenceRing.snapshot(evidence.strikeFramePosition);
        }
        TimeUnit.NANOSECONDS.timedWait(pcmEvidenceMonitor, remaining);
      }
    }
  }

  private DiagnosticAudioRing.Snapshot awaitDiagnosticAudioEvidence(TriggerEvidence evidence)
      throws InterruptedException {
    if (evidence.strikeFramePosition < 0) {
      return null;
    }
    long deadline = SystemClock.elapsedRealtimeNanos() + DIAGNOSTIC_AUDIO_WAIT_NANOS;
    synchronized (pcmEvidenceMonitor) {
      while (true) {
        DiagnosticAudioWindow.Selection selection =
            DiagnosticAudioWindow.select(
                evidence.strikeFramePosition,
                diagnosticAudioRing.oldestRetainedFramePosition(),
                diagnosticAudioRing.endRetainedFramePosition(),
                DIAGNOSTIC_AUDIO_PRE_ROLL_FRAMES,
                DIAGNOSTIC_AUDIO_POST_ROLL_FRAMES);
        if (selection.state() == DiagnosticAudioWindow.State.AVAILABLE) {
          return diagnosticAudioRing.snapshot(
              selection.firstFramePosition(), selection.endFramePosition());
        }
        if (selection.state() == DiagnosticAudioWindow.State.MARKER_EVICTED) {
          return null;
        }
        long remaining = deadline - SystemClock.elapsedRealtimeNanos();
        if (remaining <= 0) {
          return null;
        }
        TimeUnit.NANOSECONDS.timedWait(pcmEvidenceMonitor, remaining);
      }
    }
  }

  private void muxSnapshot(
      EncodedAccessUnitRetention.Snapshot snapshot, MediaFormat format, File outputFile)
      throws Exception {
    int maximumPayloadBytes = 0;
    for (int index = 0; index < snapshot.accessUnitCount(); ++index) {
      maximumPayloadBytes =
          Math.max(maximumPayloadBytes, snapshot.metadata(index).payloadBytes());
    }
    ByteBuffer payload = ByteBuffer.allocate(maximumPayloadBytes);
    MediaMuxer muxer =
        new MediaMuxer(outputFile.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
    boolean started = false;
    try {
      int track = muxer.addTrack(format);
      muxer.start();
      started = true;
      long firstPtsUs = snapshot.metadata(0).presentationTimeUs();
      for (int index = 0; index < snapshot.accessUnitCount(); ++index) {
        EncodedAccessUnitRetention.AccessUnitMetadata metadata = snapshot.metadata(index);
        payload.clear();
        snapshot.copyPayload(index, payload);
        payload.flip();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        info.set(
            0,
            metadata.payloadBytes(),
            metadata.presentationTimeUs() - firstPtsUs,
            metadata.flags());
        muxer.writeSampleData(track, payload, info);
      }
      muxer.stop();
      started = false;
    } finally {
      if (started) {
        try {
          muxer.stop();
        } catch (Throwable ignored) {
          // Preserve the primary failure.
        }
      }
      muxer.release();
    }
  }

  private JSONObject sessionManifest(
      EncodedAccessUnitRetention.Snapshot snapshot,
      PendingCapture pending,
      String mediaName,
      long mediaBytes,
      AvcCodecDescriptor codec,
      Pcm16WavFile.EvidenceMetadata audioEvidence,
      DiagnosticPcm16WavFile.EvidenceMetadata diagnosticAudioEvidence,
      String diagnosticAudioStatus,
      String diagnosticIncidentStatus,
      JSONObject poseDiagnosticMetadata,
      String poseDiagnosticStatus)
      throws Exception {
    long triggerNs = snapshot.triggerSensorTimestampNs();
    long firstPtsUs = snapshot.metadata(0).presentationTimeUs();
    long lastPtsUs = snapshot.metadata(snapshot.accessUnitCount() - 1).presentationTimeUs();
    int impactFrameIndex = 0;
    long closestImpactNanos = Long.MAX_VALUE;
    JSONArray frames = new JSONArray();
    for (int index = 0; index < snapshot.accessUnitCount(); ++index) {
      EncodedAccessUnitRetention.AccessUnitMetadata sample = snapshot.metadata(index);
      long distance = Math.abs(sample.sensorTimestampNs() - triggerNs);
      if (distance < closestImpactNanos) {
        closestImpactNanos = distance;
        impactFrameIndex = index;
      }
      frames.put(
          new JSONObject()
              .put("frame_index", index)
              .put("frame_id", Long.toString(sample.ordinal()))
              .put("device_timestamp", Long.toString(sample.sensorTimestampNs()))
              .put("time_from_impact_us", (sample.sensorTimestampNs() - triggerNs) / 1_000)
              .put("media_time_us", sample.presentationTimeUs() - firstPtsUs));
    }
    SingleViewManifestTiming manifestTiming =
        SingleViewManifestTiming.fromAbsoluteDeltaUs(closestImpactNanos / 1_000);
    JSONObject trigger =
        new JSONObject()
            .put("source", pending.evidence.source)
            .put("host_monotonic_time_ns", Long.toString(triggerNs))
            .put(
                "confirmation_host_monotonic_time_ns",
                Long.toString(pending.evidence.confirmationTimestampNanos))
            .put(
                "sample_rate_hz",
                pending.evidence.sampleRateHz == 0
                    ? JSONObject.NULL
                    : pending.evidence.sampleRateHz)
            .put(
                "peak_amplitude",
                Float.isNaN(pending.evidence.peakAmplitude)
                    ? JSONObject.NULL
                    : pending.evidence.peakAmplitude)
            .put(
                "noise_floor",
                Float.isNaN(pending.evidence.noiseFloor)
                    ? JSONObject.NULL
                    : pending.evidence.noiseFloor)
            .put(
                "threshold",
                Float.isNaN(pending.evidence.threshold)
                    ? JSONObject.NULL
                    : pending.evidence.threshold);
    JSONObject track =
        new JSONObject()
            .put("role", captureConfiguration.role().wireName())
            .put("camera_serial", captureConfiguration.nodeId())
            .put(
                "source",
                new JSONObject()
                    .put("pixel_format", "camera2_private")
                    .put("width", profile.width())
                    .put("height", profile.height()))
            .put(
                "encoded",
                new JSONObject().put("width", profile.width()).put("height", profile.height()))
            .put("frame_count", snapshot.accessUnitCount())
            .put("nominal_fps", FRAMES_PER_SECOND)
            .put("impact_frame_index", impactFrameIndex)
            .put(
                "media",
                new JSONObject()
                    .put("path", mediaName)
                    .put("mime_type", "video/mp4")
                    .put("codec", codec.rfc6381Codec())
                    .put("all_frames_keyframes", false)
                    .put("encoded_bytes", mediaBytes))
            .put("frames", frames);
    JSONObject androidCapture =
        new JSONObject()
            .put("node_id", captureConfiguration.nodeId())
            .put(
                "shared_session_id",
                sharedSessionId == null ? JSONObject.NULL : sharedSessionId)
            .put("camera_id", cameraId)
            .put("camera_timestamp_source", cameraTimestampSource)
            .put("audio_source", audioSource)
            .put("avc_profile_idc", codec.profileIdc())
            .put("avc_profile_compatibility", codec.profileCompatibility())
            .put("avc_level_idc", codec.levelIdc())
            .put("timestamp_mapping", "streaming_camera2_frame_to_encoder_ordinal")
            .put(
                "camera_to_encoder_ordinal_shift",
                timestampCalibrator.cameraToEncoderOrdinalShift())
            .put(
                "encoder_to_sensor_offset_ns",
                Long.toString(pending.encoderToSensorOffsetNanos))
            .put(
                "expected_encoder_to_sensor_offset_ns",
                Long.toString(expectedEncoderToSensorOffsetNanos))
            .put("timestamp_offset_span_ns", pending.timestampOffsetSpanNanos)
            .put("timestamp_pair_count", pending.timestampPairCount)
            .put(
                SingleViewManifestTiming.LOCAL_RESIDUAL_FIELD,
                manifestTiming.localNearestFrameResidualUs())
            .put("trigger_timestamp_uncertainty_ns", pending.evidence.timestampUncertaintyNanos)
            .put("requested_pre_roll_us", 1_400_000)
            .put("requested_post_roll_us", 500_000)
            .put(
                "actual_pre_roll_us",
                (triggerNs - snapshot.metadata(0).sensorTimestampNs()) / 1_000)
            .put(
                "actual_post_roll_us",
                (snapshot.metadata(snapshot.accessUnitCount() - 1).sensorTimestampNs() - triggerNs)
                    / 1_000)
            .put("encoded_first_pts_us", firstPtsUs)
            .put("encoded_last_pts_us", lastPtsUs);
    if (pending.evidence.peerImpactMapping != null) {
      androidCapture.put(
          "peer_impact_mapping",
          new JSONObject(pending.evidence.peerImpactMapping.toCanonicalJson()));
    }
    CaptureStartupTimingManifest startupTiming =
        startupTimingManifest()
            .orElseThrow(
                () ->
                    new IllegalStateException(
                        "completed retained capture has incomplete startup timing"));
    androidCapture.put(
        CaptureStartupTimingManifest.MANIFEST_FIELD_NAME,
        new JSONObject(startupTiming.toCanonicalJson()));
    if (peerArmStatus != null) {
      PeerArmStatusTracker.Snapshot peerArm = peerArmStatus.snapshot();
      androidCapture.put(
          "peer_arm",
          new JSONObject()
              .put("state", peerArm.state().name().toLowerCase(java.util.Locale.ROOT))
              .put(
                  "shared_session_id",
                  peerArm.sharedSessionId().isEmpty()
                      ? JSONObject.NULL
                      : peerArm.sharedSessionId())
              .put("http_status", peerArm.httpStatus() == 0 ? JSONObject.NULL : peerArm.httpStatus())
              .put(
                  "failure_type",
                  peerArm.failureType().isEmpty() ? JSONObject.NULL : peerArm.failureType()));
    }
    if (audioEvidence != null) {
      androidCapture.put(
          "audio_evidence",
          new JSONObject()
              .put("path", audioEvidence.relativePath())
              .put("bytes", audioEvidence.bytes())
              .put("sample_rate_hz", audioEvidence.sampleRateHz())
              .put("first_frame_position", Long.toString(audioEvidence.firstFramePosition()))
              .put("last_frame_position", Long.toString(audioEvidence.lastFramePosition()))
              .put("strike_frame_position", Long.toString(audioEvidence.strikeFramePosition()))
              .put("sample_count", audioEvidence.sampleCount())
              .put("strike_sample_index", audioEvidence.strikeSampleIndex()));
    }
    JSONObject diagnosticEvidence =
        new JSONObject()
            .put("schema_version", 1)
            .put("audio", JSONObject.NULL)
            .put("audio_status", diagnosticAudioStatus)
            .put("incident_status", diagnosticIncidentStatus)
            .put(
                "preview",
                poseDiagnosticMetadata == null ? JSONObject.NULL : poseDiagnosticMetadata)
            .put("preview_status", poseDiagnosticStatus);
    if (diagnosticAudioEvidence != null) {
      diagnosticEvidence.put(
          "audio",
          new JSONObject()
              .put("path", diagnosticAudioEvidence.relativePath())
              .put("bytes", diagnosticAudioEvidence.bytes())
              .put("sample_rate_hz", diagnosticAudioEvidence.sampleRateHz())
              .put(
                  "first_frame_position",
                  Long.toString(diagnosticAudioEvidence.firstFramePosition()))
              .put(
                  "end_frame_position",
                  Long.toString(diagnosticAudioEvidence.endFramePosition()))
              .put(
                  "marker_frame_position",
                  Long.toString(diagnosticAudioEvidence.markerFramePosition()))
              .put("sample_count", diagnosticAudioEvidence.sampleCount())
              .put("marker_sample_index", diagnosticAudioEvidence.markerSampleIndex()));
    }
    androidCapture.put("diagnostic_evidence", diagnosticEvidence);
    return new JSONObject()
        .put("schema_version", 1)
        .put("session_id", pending.sessionId)
        .put("created_at_utc", Instant.now().toString())
        .put("trigger", trigger)
        .put(
            SingleViewManifestTiming.INTER_VIEW_SKEW_FIELD,
            manifestTiming.mappedNearestFrameSkewUs() == null
                ? JSONObject.NULL
                : manifestTiming.mappedNearestFrameSkewUs())
        .put("views", new JSONArray().put(track))
        .put("android_capture", androidCapture);
  }

  private void startAudio() throws Exception {
    int minimumBuffer =
        AudioRecord.getMinBufferSize(
            AUDIO_SAMPLE_RATE_HZ,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT);
    if (minimumBuffer <= 0) {
      throw new IllegalStateException("Invalid minimum AudioRecord buffer " + minimumBuffer);
    }
    int bufferBytes = Math.max(minimumBuffer * 4, AUDIO_SAMPLE_RATE_HZ / 5 * 2);
    AudioManager audioManager = context.getSystemService(AudioManager.class);
    int source =
        "barbet".equals(Build.DEVICE)
            ? MediaRecorder.AudioSource.VOICE_RECOGNITION
            : Boolean.parseBoolean(
                    audioManager.getProperty(
                        AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED))
                ? MediaRecorder.AudioSource.UNPROCESSED
                : MediaRecorder.AudioSource.VOICE_RECOGNITION;
    audioSource = source;
    audioRecord =
        new AudioRecord.Builder()
            .setAudioSource(source)
            .setAudioFormat(
                new AudioFormat.Builder()
                    .setSampleRate(AUDIO_SAMPLE_RATE_HZ)
                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build())
            .setBufferSizeInBytes(bufferBytes)
            .build();
    if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
      audioRecord.release();
      throw new IllegalStateException("AudioRecord did not initialize");
    }
    // AudioRecord keeps the larger buffer for scheduling resilience, but asking a blocking read
    // to fill that entire buffer delayed detector delivery by as much as 200 ms. Process bounded
    // chunks so the shadow phone can match its local acoustic candidate before the mapped peer
    // request falls back to network-arrival time.
    int readFrames = ContinuousAudioReadPolicy.framesPerRead(AUDIO_SAMPLE_RATE_HZ);
    audioThread = new Thread(() -> recordAudio(readFrames), "continuous-impact-audio");
    audioThread.start();
    if (!audioStarted.await(5, TimeUnit.SECONDS)) {
      throw new IllegalStateException("Timed out starting continuous AudioRecord");
    }
    throwIfFailed();
  }

  private void recordAudio(int readFrames) {
    short[] samples = new short[readFrames];
    AudioTimestamp timestamp = new AudioTimestamp();
    long firstFramePosition = 0;
    try {
      audioRecord.startRecording();
      if (audioRecord.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
        throw new IllegalStateException("AudioRecord is not recording");
      }
      audioStarted.countDown();
      while (!stopping.get()) {
        int frameCount = audioRecord.read(samples, 0, samples.length, AudioRecord.READ_BLOCKING);
        if (frameCount <= 0 && stopping.get()) {
          return;
        }
        if (frameCount < 0) {
          throw new IllegalStateException("AudioRecord read failed with " + frameCount);
        }
        long beforeTimestamp = SystemClock.elapsedRealtimeNanos();
        int timestampResult =
            audioRecord.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_BOOTTIME);
        long afterTimestamp = SystemClock.elapsedRealtimeNanos();
        if (timestampResult == AudioRecord.SUCCESS) {
          long uncertainty =
              AUDIO_TIMESTAMP_BASE_UNCERTAINTY_NANOS
                  + Math.max(0, afterTimestamp - beforeTimestamp) / 2;
          AudioTimestampMapper.ObservationResult observation =
              impactDetector.observeAudioTimestamp(
                  timestamp.framePosition, timestamp.nanoTime, uncertainty);
          if (observation.accepted()) {
            latestAudioTimestamp.set(
                new AudioTimestampAnchor(
                    timestamp.framePosition, timestamp.nanoTime, uncertainty));
          } else {
            long rejected = rejectedAudioTimestamps.incrementAndGet();
            if (rejected == 1 || rejected % 100 == 0) {
              Log.w(
                  TAG,
                  "Rejected AudioRecord timestamp #"
                      + rejected
                      + ": "
                      + observation.rejectionReason());
            }
          }
        }
        if (pcmEvidenceRing != null && frameCount > 0) {
          pcmEvidenceRing.append(samples, 0, frameCount, firstFramePosition);
        }
        if (frameCount > 0) {
          diagnosticAudioRing.append(samples, 0, frameCount, firstFramePosition);
          synchronized (pcmEvidenceMonitor) {
            pcmEvidenceMonitor.notifyAll();
          }
        }
        ImpactDetector.ProcessResult processResult =
            impactDetector.processPcm16(
                samples, 0, frameCount, firstFramePosition, this::handleAudioImpact);
        firstFramePosition += frameCount;
        audioFrames.set(firstFramePosition);
        listener.onAudioMetrics(
            firstFramePosition,
            processResult.blockPeakAmplitude(),
            processResult.noiseFloorAfterBlock(),
            processResult.thresholdAfterBlock());
      }
    } catch (Throwable failure) {
      audioStarted.countDown();
      fail(failure);
    }
  }

  private void startCamera() throws Exception {
    if (warmCameraLease != null) {
      cameraId = warmCameraLease.cameraId();
      cameraTimestampSource = warmCameraLease.timestampSource();
      warmCameraLease.transitionToHighSpeed(
          encoderInputSurface,
          new WarmCameraLease.HighSpeedListener() {
            @Override
            public void onCaptureStarted(long elapsedRealtimeNanos) {
              firstCameraFrameElapsedRealtimeNanos.compareAndSet(0, elapsedRealtimeNanos);
              firstCameraFrame.countDown();
            }

            @Override
            public void onCaptureCompleted(long frameNumber, long sensorTimestampNanos) {
              try {
                timestampCalibrator.observeCamera(frameNumber, sensorTimestampNanos);
              } catch (Throwable failure) {
                fail(failure);
              }
            }

            @Override
            public void onFailure(Throwable failure) {
              fail(failure);
            }
          });
      throwIfFailed();
      return;
    }
    CameraManager manager = context.getSystemService(CameraManager.class);
    CameraSelection selection = selectCamera(manager);
    cameraId = selection.cameraId;
    cameraTimestampSource = selection.timestampSource;
    if (cameraTimestampSource != CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME) {
      throw new IllegalStateException("Camera sensor timestamps are not in the REALTIME clock");
    }
    cameraThread = new HandlerThread("continuous-high-speed-camera");
    cameraThread.start();
    Handler handler = new Handler(cameraThread.getLooper());
    manager.openCamera(
        cameraId,
        new CameraDevice.StateCallback() {
          @Override
          public void onOpened(CameraDevice opened) {
            cameraDevice = opened;
            createCameraSession(opened, handler);
          }

          @Override
          public void onDisconnected(CameraDevice disconnected) {
            disconnected.close();
            fail(new IllegalStateException("Camera disconnected"));
          }

          @Override
          public void onError(CameraDevice errorDevice, int errorCode) {
            errorDevice.close();
            fail(new IllegalStateException("Camera error " + errorCode));
          }
        },
        handler);
    if (!firstCameraFrame.await(5, TimeUnit.SECONDS)) {
      throw new IllegalStateException("Timed out waiting for continuous high-speed camera");
    }
    throwIfFailed();
  }

  @SuppressWarnings("deprecation")
  private void createCameraSession(CameraDevice device, Handler handler) {
    try {
      CaptureRequest.Builder builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
      builder.addTarget(encoderInputSurface);
      builder.set(
          CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
          new Range<>(FRAMES_PER_SECOND, FRAMES_PER_SECOND));
      builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
      device.createConstrainedHighSpeedCaptureSession(
          Collections.singletonList(encoderInputSurface),
          new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession configured) {
              cameraSession = (CameraConstrainedHighSpeedCaptureSession) configured;
              try {
                List<CaptureRequest> burst =
                    cameraSession.createHighSpeedRequestList(builder.build());
                cameraSession.setRepeatingBurst(
                    burst,
                    new CameraCaptureSession.CaptureCallback() {
                      @Override
                      public void onCaptureStarted(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          long captureTimestamp,
                          long ignoredFrameNumber) {
                        firstCameraFrameElapsedRealtimeNanos.compareAndSet(
                            0, captureTimestamp);
                        firstCameraFrame.countDown();
                      }

                      @Override
                      public void onCaptureCompleted(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          TotalCaptureResult result) {
                        Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
                        if (timestamp == null) {
                          fail(new IllegalStateException("Camera2 result omitted SENSOR_TIMESTAMP"));
                          return;
                        }
                        try {
                          timestampCalibrator.observeCamera(result.getFrameNumber(), timestamp);
                        } catch (Throwable failure) {
                          fail(failure);
                        }
                      }

                      @Override
                      public void onCaptureFailed(
                          CameraCaptureSession ignoredSession,
                          CaptureRequest ignoredRequest,
                          android.hardware.camera2.CaptureFailure failure) {
                        fail(
                            new IllegalStateException(
                                "Capture request failed: " + failure.getReason()));
                      }
                    },
                    handler);
              } catch (Throwable failure) {
                fail(failure);
              }
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession ignored) {
              fail(new IllegalStateException("High-speed session configuration failed"));
            }
          },
          handler);
    } catch (Throwable failure) {
      fail(failure);
    }
  }

  private CameraSelection selectCamera(CameraManager manager) throws Exception {
    for (String candidate : manager.getCameraIdList()) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(candidate);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK) {
        continue;
      }
      StreamConfigurationMap streams =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      if (streams == null) {
        continue;
      }
      Size requested = new Size(profile.width(), profile.height());
      if (!Arrays.asList(streams.getHighSpeedVideoSizes()).contains(requested)) {
        continue;
      }
      for (Range<Integer> range : streams.getHighSpeedVideoFpsRangesFor(requested)) {
        if (range.getLower() == FRAMES_PER_SECOND && range.getUpper() == FRAMES_PER_SECOND) {
          Integer timestampSource =
              characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
          return new CameraSelection(candidate, timestampSource == null ? -1 : timestampSource);
        }
      }
    }
    throw new IllegalStateException(
        "No rear camera supports "
            + profile.width()
            + "x"
            + profile.height()
            + " at fixed 240 fps");
  }

  private String selectEncoder() {
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (!info.isEncoder() || !info.isHardwareAccelerated()) {
        continue;
      }
      for (String type : info.getSupportedTypes()) {
        if (!MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(type)) {
          continue;
        }
        try {
          if (info.getCapabilitiesForType(type)
              .getVideoCapabilities()
              .areSizeAndRateSupported(profile.width(), profile.height(), FRAMES_PER_SECOND)) {
            return info.getName();
          }
        } catch (IllegalArgumentException ignored) {
          // Try the next hardware codec.
        }
      }
    }
    throw new IllegalStateException("No hardware H.264 encoder supports the selected profile");
  }

  private void stopCamera() {
    if (warmCameraLease != null) {
      warmCameraLease.close();
      return;
    }
    if (cameraSession != null) {
      try {
        cameraSession.stopRepeating();
        cameraSession.abortCaptures();
      } catch (Throwable failure) {
        fail(failure);
      }
      cameraSession.close();
    }
    if (cameraDevice != null) {
      cameraDevice.close();
    }
    if (cameraThread != null) {
      cameraThread.quitSafely();
      join(cameraThread, "Camera thread");
    }
  }

  private void stopAudio() {
    if (audioRecord != null) {
      try {
        audioRecord.stop();
      } catch (Throwable failure) {
        fail(failure);
      }
    }
    join(audioThread, "Audio thread");
    if (audioRecord != null) {
      audioRecord.release();
    }
  }

  private void stopEncoder() {
    if (encoder != null) {
      try {
        encoder.signalEndOfInputStream();
      } catch (Throwable failure) {
        fail(failure);
      }
    }
    join(encoderThread, "Encoder thread");
    if (encoder != null) {
      try {
        encoder.stop();
      } catch (Throwable failure) {
        fail(failure);
      }
      encoder.release();
    }
    if (encoderInputSurface != null) {
      encoderInputSurface.release();
    }
  }

  private void join(Thread thread, String name) {
    if (thread == null || thread == Thread.currentThread()) {
      return;
    }
    try {
      thread.join(THREAD_STOP_TIMEOUT_MILLIS);
      if (thread.isAlive()) {
        fail(new IllegalStateException(name + " did not stop"));
      }
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      fail(interrupted);
    }
  }

  private void requirePermissions() {
    if (context.checkSelfPermission(Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        || context.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
      throw new IllegalStateException("Camera and microphone permissions are required");
    }
  }

  private void throwIfFailed() {
    if (failed.get()) {
      throw new IllegalStateException("Continuous capture failed; inspect service status");
    }
  }

  private void fail(Throwable failure) {
    if (!stopping.get() && failed.compareAndSet(false, true)) {
      listener.onFailure(failure);
    }
  }

  private static String boundedFailureType(Throwable failure) {
    String type = failure.getClass().getName();
    return type.length() <= MAXIMUM_LOGGED_FAILURE_TYPE_CHARACTERS
        ? type
        : type.substring(0, MAXIMUM_LOGGED_FAILURE_TYPE_CHARACTERS);
  }

  private static String newSessionId() {
    return "android-"
        + System.currentTimeMillis()
        + "-"
        + UUID.randomUUID().toString().substring(0, 8);
  }

  private static final class CameraSelection {
    private final String cameraId;
    private final int timestampSource;

    private CameraSelection(String cameraId, int timestampSource) {
      this.cameraId = cameraId;
      this.timestampSource = timestampSource;
    }
  }
}
