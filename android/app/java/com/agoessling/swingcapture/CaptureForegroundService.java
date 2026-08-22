package com.agoessling.swingcapture;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.os.BatteryManager;
import android.os.Debug;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;
import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import com.agoessling.swingcapture.standby.StandbyDiagnosticCoordinator;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Owns the camera/microphone capture loop and node API beyond the activity lifecycle. */
public final class CaptureForegroundService extends Service
    implements NodeHttpServer.CaptureControl, ContinuousCaptureEngine.Listener {
  public static final String ACTION_START = "com.agoessling.swingcapture.action.START";
  public static final String ACTION_ARM = "com.agoessling.swingcapture.action.ARM";
  public static final String ACTION_DISARM = "com.agoessling.swingcapture.action.DISARM";
  public static final String ACTION_TRIGGER = "com.agoessling.swingcapture.action.TRIGGER";
  public static final String ACTION_START_POSE_HIL =
      "com.agoessling.swingcapture.action.START_POSE_HIL";
  public static final String ACTION_ARM_CONTINUOUS_HIL =
      "com.agoessling.swingcapture.action.ARM_CONTINUOUS_HIL";
  public static final String ACTION_ARM_AUDIO_HIL =
      "com.agoessling.swingcapture.action.ARM_AUDIO_HIL";
  public static final String ACTION_ARM_SOAK_HIL =
      "com.agoessling.swingcapture.action.ARM_SOAK_HIL";
  public static final String ACTION_FINISH_SOAK_HIL =
      "com.agoessling.swingcapture.action.FINISH_SOAK_HIL";
  public static final String EXTRA_SHARED_SESSION_ID = "shared_session_id";
  private static final String TAG = "SwingCaptureService";
  private static final String CHANNEL_ID = "capture_node";
  private static final int NOTIFICATION_ID = 240;
  private static final long SOAK_TELEMETRY_INTERVAL_NANOS = 30_000_000_000L;
  private static final CaptureRuntime RUNTIME = new CaptureRuntime();
  private static final NodeCoordinationState COORDINATION = new NodeCoordinationState();
  private static volatile List<String> advertisedUrls = Collections.emptyList();

  private final ExecutorService controlExecutor = Executors.newSingleThreadExecutor();
  private final ExecutorService peerExecutor = Executors.newSingleThreadExecutor();
  private final ScheduledExecutorService poseTimeoutExecutor =
      Executors.newSingleThreadScheduledExecutor();
  private final ExecutorService standbyDiagnosticPublisherExecutor =
      new ThreadPoolExecutor(
          1,
          1,
          0L,
          TimeUnit.MILLISECONDS,
          new ArrayBlockingQueue<>(StandbyDiagnosticSessionRegistry.MAXIMUM_PENDING_SESSIONS),
          runnable -> new Thread(runnable, "standby-diagnostic-publisher"),
          new ThreadPoolExecutor.AbortPolicy());
  private final StandbyDiagnosticSessionRegistry standbyDiagnosticSessions =
      new StandbyDiagnosticSessionRegistry();
  private final StandbyDiagnosticTelemetry standbyDiagnosticTelemetry =
      new StandbyDiagnosticTelemetry();
  private final PoseTriggerControllerLease poseTriggerControllerLease =
      new PoseTriggerControllerLease();
  private final PeerArmStatusTracker peerArmStatus = new PeerArmStatusTracker();
  private NodeConfiguration configuration;
  private NodeHttpServer server;
  private volatile ContinuousCaptureEngine engine;
  private volatile PoseStandbyEngine poseStandbyEngine;
  private volatile StandbyAudioRecorder standbyAudioRecorder;
  private volatile CaptureConfigurationSnapshot activeCaptureConfiguration;
  private volatile PoseStationConfigurationSnapshot activePoseConfiguration;
  private volatile PoseStandbyMetrics.Snapshot lastPoseMetrics;
  private volatile PoseTriggerController.Decision lastPoseDecision;
  private volatile boolean poseTransitionRequested;
  private volatile long poseArmCancelledForStandbyDiagnostic;
  private volatile String poseTransitionLastPrecedence = "none";
  private volatile boolean standbyOperatorTagInProgress;
  private volatile boolean poseHighSpeedAttempt;
  private volatile PoseHighSpeedNoImpactDeadline.Deadline poseNoImpactDeadline;
  private volatile long poseThermalHardStopElapsedRealtimeNanos;
  private volatile ScheduledFuture<?> poseNoImpactTimeout;
  private volatile ScheduledFuture<?> poseThermalHardStopTimeout;
  private PowerManager.WakeLock wakeLock;
  private volatile boolean continuousHilRequested;
  private volatile boolean audioHilRequested;
  private volatile boolean soakHilRequested;
  private volatile boolean poseArmHilEnabled;
  private volatile boolean soakCompletionRequested;
  private volatile int soakIncidentalTriggerCount;
  private volatile String soakLastIncidentalSessionId = "";
  private volatile long lastSoakTelemetryElapsedRealtimeNanos;
  private volatile long armRequestedElapsedRealtimeNanos;
  private volatile boolean captureReady;
  private volatile float audioPeakAmplitude;
  private volatile float audioNoiseFloor;
  private volatile float audioThreshold;
  private volatile boolean standbyAudioReady;
  private volatile int standbyAudioSource = -1;
  private volatile long standbyAudioEndFramePosition;
  private volatile float standbyAudioPeakAmplitude;
  private volatile float standbyAudioNoiseFloor;
  private volatile float standbyAudioThreshold;

  public static CaptureRuntime.Snapshot snapshot() {
    return RUNTIME.snapshot();
  }

  public static List<String> advertisedUrls() {
    return advertisedUrls;
  }

  @Override
  public void onCreate() {
    super.onCreate();
    configuration = new NodeConfiguration(this);
    createNotificationChannel();
    startForeground(
        NOTIFICATION_ID,
        notification("Node service running; capture is not armed"),
        ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
            | ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
    PowerManager power = getSystemService(PowerManager.class);
    wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "swing_capture:capture_node");
    wakeLock.setReferenceCounted(false);
    cleanupStaleSessionStaging();
    startServer();
  }

  private void cleanupStaleSessionStaging() {
    File sessions = new File(getFilesDir(), "sessions");
    try {
      List<String> removed =
          SessionStagingCleanup.cleanupStale(
              sessions,
              System.currentTimeMillis(),
              SessionStagingCleanup.DEFAULT_STALE_AGE_MILLIS);
      if (!removed.isEmpty()) {
        Log.i(TAG, "Removed stale unpublished session staging: " + String.join(", ", removed));
      }
    } catch (IOException failure) {
      Log.e(TAG, "Unable to inspect stale unpublished session staging", failure);
    }
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    String action = intent == null ? ACTION_START : intent.getAction();
    if (ACTION_START_POSE_HIL.equals(action)) {
      poseArmHilEnabled = true;
    }
    if (ACTION_ARM.equals(action)
        || ACTION_ARM_CONTINUOUS_HIL.equals(action)
        || ACTION_ARM_AUDIO_HIL.equals(action)
        || ACTION_ARM_SOAK_HIL.equals(action)) {
      boolean previousContinuousHilRequested = continuousHilRequested;
      boolean previousAudioHilRequested = audioHilRequested;
      boolean previousSoakHilRequested = soakHilRequested;
      boolean previousCaptureReady = captureReady;
      try {
        continuousHilRequested =
            ACTION_ARM_CONTINUOUS_HIL.equals(action)
                || ACTION_ARM_AUDIO_HIL.equals(action)
                || ACTION_ARM_SOAK_HIL.equals(action);
        audioHilRequested =
            ACTION_ARM_AUDIO_HIL.equals(action) || ACTION_ARM_SOAK_HIL.equals(action);
        soakHilRequested = ACTION_ARM_SOAK_HIL.equals(action);
        soakCompletionRequested = false;
        soakIncidentalTriggerCount = 0;
        soakLastIncidentalSessionId = "";
        lastSoakTelemetryElapsedRealtimeNanos = 0L;
        captureReady = false;
        setArmed(
            true,
            intent == null ? null : intent.getStringExtra(EXTRA_SHARED_SESSION_ID));
      } catch (IllegalArgumentException | IllegalStateException rejected) {
        continuousHilRequested = previousContinuousHilRequested;
        audioHilRequested = previousAudioHilRequested;
        soakHilRequested = previousSoakHilRequested;
        captureReady = previousCaptureReady;
        Log.i(TAG, "Arm request rejected: " + rejected.getMessage());
      }
    } else if (ACTION_FINISH_SOAK_HIL.equals(action)) {
      try {
        finishSoakHil();
      } catch (IllegalStateException rejected) {
        onFailure(rejected);
      }
    } else if (ACTION_DISARM.equals(action)) {
      setArmed(false, null);
    } else if (ACTION_TRIGGER.equals(action)) {
      try {
        triggerManual();
      } catch (IllegalStateException rejected) {
        Log.i(TAG, "Manual trigger rejected: " + rejected.getMessage());
      }
    }
    return START_STICKY;
  }

  @Override
  public IBinder onBind(Intent intent) {
    return null;
  }

  @Override
  public void onDestroy() {
    stopCapture();
    if (server != null) {
      server.stop();
    }
    advertisedUrls = Collections.emptyList();
    drainStandbyDiagnosticPublisher();
    controlExecutor.shutdownNow();
    peerExecutor.shutdownNow();
    poseTimeoutExecutor.shutdownNow();
    releaseWakeLock();
    RUNTIME.stopped();
    super.onDestroy();
  }

  private void drainStandbyDiagnosticPublisher() {
    standbyDiagnosticPublisherExecutor.shutdown();
    boolean interrupted = false;
    try {
      if (!standbyDiagnosticPublisherExecutor.awaitTermination(5, TimeUnit.SECONDS)) {
        List<Runnable> discarded = standbyDiagnosticPublisherExecutor.shutdownNow();
        standbyDiagnosticTelemetry.recordDroppedEvents(
            discarded.size(), "publication:shutdown_timeout");
        Log.e(
            TAG,
            "Timed out draining standby diagnostic publisher; discarded "
                + discarded.size()
                + " queued publication(s)");
      }
    } catch (InterruptedException interruption) {
      interrupted = true;
      List<Runnable> discarded = standbyDiagnosticPublisherExecutor.shutdownNow();
      standbyDiagnosticTelemetry.recordDroppedEvents(
          discarded.size(), "publication:shutdown_interrupted");
      Log.e(TAG, "Interrupted while draining standby diagnostic publisher", interruption);
    }
    if (interrupted) {
      Thread.currentThread().interrupt();
    }
  }

  @Override
  public void setArmed(boolean armed, String sharedSessionId) {
    setArmed(armed, sharedSessionId, false);
  }

  private void setArmed(
      boolean armed, String sharedSessionId, boolean preserveCompletedTriggerReport) {
    if (!armed) {
      if (preserveCompletedTriggerReport) {
        throw new IllegalArgumentException("disarming cannot preserve a trigger report");
      }
      if (sharedSessionId != null) {
        throw new IllegalArgumentException("shared_session_id is valid only while arming");
      }
      controlExecutor.execute(this::stopCapture);
      return;
    }
    synchronized (this) {
      CaptureRuntime.Snapshot snapshot = RUNTIME.snapshot();
      if (snapshot.state() != CaptureRuntime.State.STOPPED
          && snapshot.state() != CaptureRuntime.State.ERROR) {
        throw new IllegalStateException("capture is already running");
      }
      if (activeCaptureConfiguration != null) {
        throw new IllegalStateException("the previous capture is still shutting down");
      }
      CaptureConfigurationSnapshot requestedConfiguration = configuration.captureSnapshot();
      requestedConfiguration.requireAssignedRole();
      PoseStationConfigurationSnapshot requestedPoseConfiguration =
          configuration.poseConfigurationSnapshot();
      if (preserveCompletedTriggerReport) {
        if (sharedSessionId != null) {
          throw new IllegalArgumentException(
              "pose monitoring restart cannot accept a shared session ID");
        }
        COORDINATION.resumeMonitoringAfterCapture();
      } else {
        COORDINATION.armed(sharedSessionId);
      }
      RUNTIME.starting();
      activeCaptureConfiguration = requestedConfiguration;
      activePoseConfiguration = requestedPoseConfiguration;
      poseTransitionRequested = false;
      poseHighSpeedAttempt = false;
      poseNoImpactDeadline = null;
      poseThermalHardStopElapsedRealtimeNanos = 0L;
      peerArmStatus.reset();
      armRequestedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
    }
    if (!continuousHilRequested
        && activePoseConfiguration.mode() != PoseNodeMode.DISABLED) {
      updateNotification("Starting 5 Hz pose standby…");
      controlExecutor.execute(this::startPoseStandby);
    } else {
      updateNotification("Starting camera, microphone, and encoded pre-roll…");
      controlExecutor.execute(this::startCapture);
    }
  }

  @Override
  public String triggerManual() {
    return triggerOperatorCapture(false).sessionId();
  }

  @Override
  public NodeHttpServer.CaptureControl.TriggeredSession triggerMissedShot() {
    return triggerOperatorCapture(true);
  }

  @Override
  public void triggerPoseArm(PosePeerArmClient.Candidate candidate) {
    if (candidate == null) {
      throw new IllegalArgumentException("pose-arm candidate is required");
    }
    PoseExternalArmLifecycle externalArmLifecycle;
    synchronized (this) {
      PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
      CaptureConfigurationSnapshot captureConfiguration = activeCaptureConfiguration;
      if (poseConfiguration == null
          || captureConfiguration == null
          || poseConfiguration.mode() != PoseNodeMode.SHADOW
          || poseStandbyEngine == null
          || RUNTIME.snapshot().state() != CaptureRuntime.State.ARMED) {
        throw new IllegalStateException("shadow pose standby is not ready");
      }
      if (captureConfiguration.nodeId().equals(candidate.leaderNodeId())) {
        throw new IllegalArgumentException("pose leader and peer must be distinct nodes");
      }
      if (poseTransitionRequested) {
        throw new IllegalStateException("pose high-speed transition is already in progress");
      }
      boolean diagnosticBusy =
          standbyOperatorTagInProgress
              || (standbyAudioRecorder != null
                  && standbyAudioRecorder.pendingEventCount() > 0);
      if (PoseTransitionPrecedence.resolve(false, diagnosticBusy)
          == PoseTransitionPrecedence.Outcome.DIAGNOSTIC_WINS) {
        ++poseArmCancelledForStandbyDiagnostic;
        poseTransitionLastPrecedence = "standby_diagnostic_before_peer_pose_arm";
        throw new IllegalStateException("standby diagnostic evidence is completing post-roll");
      }
      externalArmLifecycle = new PoseExternalArmLifecycle();
      poseTransitionRequested = true;
      poseTransitionLastPrecedence = "peer_pose_arm_claimed";
      peerArmStatus.inboundAccepted(candidate.sharedSessionId());
    }
    controlExecutor.execute(
        () -> beginPoseHighSpeed(candidate.sharedSessionId(), false, null, externalArmLifecycle));
  }

  @Override
  public boolean poseLeaderHilEnabled() {
    return poseArmHilEnabled;
  }

  @Override
  public String triggerPoseLeaderHil() {
    PoseHilEndpointAccess.requireEnabled(poseArmHilEnabled);
    PoseStationConfigurationSnapshot poseConfiguration;
    CaptureConfigurationSnapshot captureConfiguration;
    String sharedSessionId;
    long candidateTimestamp;
    PoseExternalArmLifecycle externalArmLifecycle;
    synchronized (this) {
      poseConfiguration = activePoseConfiguration;
      captureConfiguration = activeCaptureConfiguration;
      if (poseConfiguration == null
          || captureConfiguration == null
          || poseConfiguration.mode() != PoseNodeMode.LEADER
          || !poseConfiguration.hasPeer()
          || poseStandbyEngine == null
          || RUNTIME.snapshot().state() != CaptureRuntime.State.ARMED) {
        throw new IllegalStateException("leader pose standby with a peer is not ready");
      }
      if (poseTransitionRequested) {
        throw new IllegalStateException("pose high-speed transition is already in progress");
      }
      boolean diagnosticBusy =
          standbyOperatorTagInProgress
              || (standbyAudioRecorder != null && standbyAudioRecorder.pendingEventCount() > 0);
      if (PoseTransitionPrecedence.resolve(false, diagnosticBusy)
          == PoseTransitionPrecedence.Outcome.DIAGNOSTIC_WINS) {
        ++poseArmCancelledForStandbyDiagnostic;
        poseTransitionLastPrecedence = "standby_diagnostic_before_hil_leader_pose_arm";
        throw new IllegalStateException("standby diagnostic evidence is completing post-roll");
      }
      candidateTimestamp = SystemClock.elapsedRealtimeNanos();
      sharedSessionId = "pose-hil-" + UUID.randomUUID();
      externalArmLifecycle = new PoseExternalArmLifecycle();
      poseTransitionRequested = true;
      poseTransitionLastPrecedence = "hil_leader_pose_arm_claimed";
      peerArmStatus.pending(sharedSessionId);
    }
    PosePeerArmClient.Candidate candidate =
        new PosePeerArmClient.Candidate(
            sharedSessionId,
            captureConfiguration.nodeId(),
            candidateTimestamp,
            1.0,
            1.0);
    peerExecutor.execute(() -> requestPeerPoseArm(poseConfiguration, candidate));
    controlExecutor.execute(
        () -> beginPoseHighSpeed(sharedSessionId, true, null, externalArmLifecycle));
    return sharedSessionId;
  }

  @Override
  public JSONObject poseStatus() {
    PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
    if (poseConfiguration == null) {
      poseConfiguration = configuration.poseConfigurationSnapshot();
    }
    PoseStandbyEngine standby = poseStandbyEngine;
    PoseStandbyMetrics.Snapshot metrics = standby == null ? lastPoseMetrics : standby.metrics();
    PoseTriggerController.Decision decision = lastPoseDecision;
    StandbyDiagnosticTelemetry.Snapshot diagnosticTelemetry =
        standbyDiagnosticTelemetry.snapshot();
    JSONObject status = new JSONObject();
    try {
      status
          .put("mode", poseConfiguration.mode().wireName())
          .put("hil_pose_arm_enabled", poseArmHilEnabled)
          .put("configured_delegate", poseConfiguration.delegateWireName())
          .put("debug_evidence_enabled", poseConfiguration.debugEvidenceEnabled())
          .put("peer_configured", poseConfiguration.hasPeer())
          .put(
              "phase",
              standby != null
                  ? "monitoring"
                  : (poseHighSpeedAttempt ? "high_speed" : "idle"))
          .put("transition_requested", poseTransitionRequested)
          .put("transition_last_precedence", poseTransitionLastPrecedence)
          .put(
              "pose_arms_cancelled_for_standby_diagnostic",
              poseArmCancelledForStandbyDiagnostic)
          .put(
              "standby_diagnostic_events_cancelled_for_pose_arm",
              diagnosticTelemetry.eventsCancelledForPoseArm())
          .put(
              "active_evidence_stop_elapsed_realtime_ns",
              poseNoImpactDeadline == null
                  ? JSONObject.NULL
                  : Long.toString(poseNoImpactDeadline.activeEvidenceStopNs()))
          .put(
              "thermal_hard_stop_elapsed_realtime_ns",
              poseThermalHardStopElapsedRealtimeNanos == 0
                  ? JSONObject.NULL
                  : Long.toString(poseThermalHardStopElapsedRealtimeNanos));
      if (metrics == null) {
        status.put("metrics", JSONObject.NULL);
      } else {
        status.put(
            "metrics",
            new JSONObject()
                .put("delegate", metrics.delegate().name().toLowerCase(java.util.Locale.ROOT))
                .put("offered_images", metrics.offeredImages())
                .put("scheduled_images", metrics.scheduledImages())
                .put("dropped_images", metrics.droppedImages())
                .put("successful_inferences", metrics.successfulInferences())
                .put("failed_inferences", metrics.failedInferences())
                .put("mean_inference_duration_ms", metrics.meanInferenceDurationMs())
                .put("maximum_inference_duration_ns", metrics.maximumInferenceDurationNs())
                .put("retained_observation_rows", metrics.retainedObservationRows())
                .put("failed_observation_rows", metrics.failedObservationRows())
                .put("offered_evidence_frames", metrics.offeredEvidenceFrames())
                .put("dropped_evidence_frames", metrics.droppedEvidenceFrames())
                .put("encoded_evidence_frames", metrics.encodedEvidenceFrames())
                .put("failed_evidence_frames", metrics.failedEvidenceFrames())
                .put("arm_evidence_flush_present", metrics.armEvidenceFlushPresent())
                .put("arm_evidence_flush_failed", metrics.armEvidenceFlushFailed())
                .put("arm_evidence_flush_timed_out", metrics.armEvidenceFlushTimedOut()));
      }
      status.put(
          "last_decision",
          decision == null
              ? JSONObject.NULL
              : new JSONObject()
                  .put("state", decision.state().name().toLowerCase(java.util.Locale.ROOT))
                  .put("command", decision.command().name().toLowerCase(java.util.Locale.ROOT))
                  .put(
                      "reason",
                      decision.transitionReason().name().toLowerCase(java.util.Locale.ROOT)));
      PeerArmStatusTracker.Snapshot peerArm = peerArmStatus.snapshot();
      status.put(
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
      StandbyAudioRecorder standbyAudio = standbyAudioRecorder;
      status.put(
          "standby_audio",
          new JSONObject()
              .put("ready", standbyAudioReady && standbyAudio != null)
              .put("audio_source", standbyAudioSource < 0 ? JSONObject.NULL : standbyAudioSource)
              .put("end_frame_position", Long.toString(standbyAudioEndFramePosition))
              .put("peak_amplitude", standbyAudioPeakAmplitude)
              .put("noise_floor", standbyAudioNoiseFloor)
              .put("threshold", standbyAudioThreshold)
              .put("pending_events", standbyDiagnosticSessions.size())
              .put("detected_events", diagnosticTelemetry.detectedEvents())
              .put("operator_tags", diagnosticTelemetry.operatorTags())
              .put("published_sessions", diagnosticTelemetry.publishedSessions())
              .put("dropped_events", diagnosticTelemetry.droppedEvents())
              .put("timestamp_rejections", diagnosticTelemetry.timestampRejections())
              .put("discontinuities", diagnosticTelemetry.discontinuities())
              .put("last_error", diagnosticTelemetry.lastError()));
      return status;
    } catch (org.json.JSONException impossible) {
      throw new IllegalStateException("Unable to serialize pose status", impossible);
    }
  }

  private NodeHttpServer.CaptureControl.TriggeredSession triggerOperatorCapture(
      boolean missedShot) {
    ContinuousCaptureEngine current = engine;
    if (current == null
        && missedShot
        && RUNTIME.snapshot().state() == CaptureRuntime.State.ARMED) {
      StandbyAudioRecorder standbyAudio = standbyAudioRecorder;
      CaptureConfigurationSnapshot captureConfiguration = activeCaptureConfiguration;
      if (standbyAudio != null && captureConfiguration != null) {
        synchronized (this) {
          if (poseTransitionRequested) {
            throw new IllegalStateException(
                "pose high-speed transition is already in progress");
          }
          standbyOperatorTagInProgress = true;
        }
        try {
          StandbyDiagnosticCoordinator.OperatorTagResult tag = standbyAudio.tagOperator();
          if (!tag.accepted()) {
            throw new IllegalStateException("standby diagnostic event queue is full");
          }
          String sessionId = newSessionId();
          standbyDiagnosticSessions.register(
              tag.event().sequence(),
              new StandbyDiagnosticSessionRegistry.PendingSession(
                  sessionId,
                  captureConfiguration.nodeId(),
                  System.currentTimeMillis()));
          standbyDiagnosticTelemetry.recordOperatorTag();
          return new NodeHttpServer.CaptureControl.TriggeredSession(
              sessionId, "standby_diagnostic");
        } finally {
          standbyOperatorTagInProgress = false;
        }
      }
    }
    if (current == null || RUNTIME.snapshot().state() != CaptureRuntime.State.ARMED) {
      throw new IllegalStateException("capture is not armed with a full pre-roll");
    }
    String sessionId = newSessionId();
    ContinuousCaptureEngine.TriggerAttempt attempt =
        missedShot
            ? current.triggerMissedShot(sessionId)
            : current.triggerManual(sessionId);
    if (!attempt.accepted()) {
      throw new IllegalStateException(attempt.diagnostic());
    }
    return new NodeHttpServer.CaptureControl.TriggeredSession(sessionId, "capture");
  }

  @Override
  public void onReady() {
    audioPeakAmplitude = 0.0f;
    RUNTIME.armed();
    updateNotification("Armed: waiting for a local audio impact");
    if (poseHighSpeedAttempt) {
      schedulePoseNoImpactTimeout();
    }
    if (continuousHilRequested) {
      if (audioHilRequested) {
        lastSoakTelemetryElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
        writeContinuousHilArmedReport();
      } else {
        try {
          triggerManual();
        } catch (Throwable failure) {
          onFailure(failure);
        }
      }
    }
    captureReady = true;
  }

  @Override
  public void onRingMetrics(
      long videoFrames, long audioFrames, long bytes, long durationUs) {
    RUNTIME.updateRing(videoFrames, audioFrames, bytes, durationUs);
    if (soakHilRequested && captureReady) {
      long now = SystemClock.elapsedRealtimeNanos();
      if (now - lastSoakTelemetryElapsedRealtimeNanos >= SOAK_TELEMETRY_INTERVAL_NANOS) {
        lastSoakTelemetryElapsedRealtimeNanos = now;
        writeContinuousHilArmedReport();
      }
    }
  }

  @Override
  public void onAudioMetrics(
      long audioFrames, float peakAmplitude, float noiseFloor, float threshold) {
    audioNoiseFloor = noiseFloor;
    audioThreshold = threshold;
    if (audioHilRequested && captureReady) {
      audioPeakAmplitude = Math.max(audioPeakAmplitude, peakAmplitude);
      if (!soakHilRequested) {
        writeContinuousHilArmedReport();
      }
    }
  }

  @Override
  public void onTriggerAccepted(
      String sessionId,
      long triggerTimestampNanos,
      long timestampUncertaintyNanos,
      String source) {
    cancelPoseActiveEvidenceTimeout();
    CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
    RUNTIME.triggered(sessionId, triggerTimestampNanos);
    COORDINATION.triggered(
        captureConfiguration.role().wireName(),
        captureConfiguration.nodeId(),
        sessionId,
        triggerTimestampNanos,
        timestampUncertaintyNanos,
        source);
    updateNotification("Impact detected; collecting post-roll");
  }

  @Override
  public void onPublishing(String sessionId) {
    RUNTIME.publishing(sessionId);
    updateNotification("Publishing retained swing " + sessionId);
  }

  @Override
  public void onPublished(String sessionId) {
    cancelPoseNoImpactTimeout();
    boolean restartPoseStandby = poseHighSpeedAttempt && !continuousHilRequested;
    boolean operatorCapture = !continuousHilRequested && !restartPoseStandby;
    RUNTIME.published(sessionId);
    updateNotification("Armed: last published " + sessionId);
    if (continuousHilRequested) {
      if (soakHilRequested && !soakCompletionRequested) {
        ++soakIncidentalTriggerCount;
        soakLastIncidentalSessionId = sessionId;
        writeContinuousHilArmedReport();
      } else {
        continuousHilRequested = false;
        audioHilRequested = false;
        soakHilRequested = false;
        writeContinuousHilReport(sessionId, null);
      }
    }
    try {
      List<String> deleted = SessionStorage.enforceRetention(this, Collections.emptySet());
      if (!deleted.isEmpty()) {
        Log.i(TAG, "Expired sessions: " + String.join(", ", deleted));
      }
    } catch (Exception failure) {
      Log.e(TAG, "Unable to enforce session retention", failure);
    }
    if (operatorCapture) {
      // A shared session ID identifies exactly one physical swing. Stopping after publication
      // forces the browser coordinator to provision a fresh ID to both nodes before the next
      // shot instead of silently associating later clips with stale trigger evidence.
      controlExecutor.execute(this::stopCapture);
    } else if (restartPoseStandby) {
      try {
        lastPoseDecision =
            poseTriggerControllerLease.captureEnded(SystemClock::elapsedRealtimeNanos);
      } catch (RuntimeException invalidControllerState) {
        onFailure(invalidControllerState);
        return;
      }
      controlExecutor.execute(
          () -> {
            stopCaptureForPoseRestart();
            try {
              setArmed(true, null, true);
            } catch (RuntimeException restartFailure) {
              onFailure(restartFailure);
            }
          });
    }
  }

  @Override
  public void onTriggerRejected(String source, String reason) {
    Log.i(TAG, "Trigger from " + source + " rejected: " + reason);
  }

  @Override
  public void onFailure(Throwable failure) {
    Log.e(TAG, "Continuous capture failed", failure);
    ContinuousCaptureContinuityDiagnostic continuityDiagnostic =
        continuityDiagnostic(failure);
    if (continuityDiagnostic != null) {
      Log.e(TAG, "Structured continuity evidence: " + continuityDiagnostic.toJson());
    }
    RUNTIME.failed(failure);
    updateNotification("Capture error: " + String.valueOf(failure.getMessage()));
    if (continuousHilRequested) {
      continuousHilRequested = false;
      audioHilRequested = false;
      soakHilRequested = false;
      writeContinuousHilReport("", failure);
    }
    controlExecutor.execute(this::stopEngineOnly);
  }

  private void startCapture() {
    try {
      acquireWakeLock();
      CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
      ContinuousCaptureEngine created =
          new ContinuousCaptureEngine(
              this,
              captureConfiguration,
              COORDINATION.sharedSessionId(),
              audioHilRequested,
              !soakHilRequested,
              this);
      engine = created;
      created.start();
    } catch (Throwable failure) {
      releaseWakeLock();
      onFailure(failure);
    }
  }

  private void startPoseStandby() {
    try {
      acquireWakeLock();
      CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
      PoseStationConfigurationSnapshot poseConfiguration = requireActivePoseConfiguration();
      PoseStandbyEngine.Config standbyConfig =
          PoseStandbyEngine.Config.defaults(
              captureConfiguration.profile(),
              captureConfiguration.role(),
              poseConfiguration.delegatePolicy(),
              poseConfiguration.hittingRegion(),
              poseConfiguration.debugEvidenceEnabled());
      PoseTriggerController triggerController =
          poseTriggerControllerLease.acquire(standbyConfig.controllerConfig());
      PoseStandbyEngine created =
          PoseStandbyEngine.start(
              this,
              standbyConfig,
              triggerController,
              new PoseStandbyEngine.Listener() {
                @Override
                public void onReady(PoseStandbyEngine.ReadyStatus ready) {
                  lastPoseMetrics = createdPoseMetrics();
                  updateNotification(
                      "Pose camera ready: "
                          + ready.standbySize().getWidth()
                          + "x"
                          + ready.standbySize().getHeight()
                          + " at 5 Hz ("
                          + ready.inferenceDelegate().name()
                          + ")");
                }

                @Override
                public void onDecision(
                    PoseLandmarkObservationExtractor.Evaluation evaluation,
                    PoseTriggerController.Decision decision) {
                  lastPoseDecision = decision;
                  PoseStandbyEngine current = poseStandbyEngine;
                  if (current != null) {
                    lastPoseMetrics = current.metrics();
                  }
                  if (decision.command() == PoseTriggerController.Command.START_HIGH_SPEED
                      && poseConfiguration.mode() != PoseNodeMode.LEADER) {
                    if (current != null) {
                      current.cancelArmEvidence(evaluation.timestampNs());
                    }
                    return;
                  }
                  if (decision.command() == PoseTriggerController.Command.START_HIGH_SPEED
                      && poseConfiguration.mode() == PoseNodeMode.LEADER) {
                    boolean diagnosticWins = false;
                    synchronized (CaptureForegroundService.this) {
                      StandbyAudioRecorder currentAudio = standbyAudioRecorder;
                      if (poseTransitionRequested) {
                        if (current != null) {
                          current.cancelArmEvidence(evaluation.timestampNs());
                        }
                        return;
                      }
                      boolean diagnosticBusy =
                          standbyOperatorTagInProgress
                              || (currentAudio != null && currentAudio.pendingEventCount() > 0);
                      diagnosticWins =
                          PoseTransitionPrecedence.resolve(false, diagnosticBusy)
                              == PoseTransitionPrecedence.Outcome.DIAGNOSTIC_WINS;
                      if (diagnosticWins) {
                        ++poseArmCancelledForStandbyDiagnostic;
                        poseTransitionLastPrecedence =
                            "standby_diagnostic_before_local_pose_arm";
                      } else {
                        if (current != null) {
                          current.acceptArmEvidence(evaluation.timestampNs());
                        }
                        poseTransitionRequested = true;
                        poseTransitionLastPrecedence = "local_pose_arm_claimed";
                        poseThermalHardStopElapsedRealtimeNanos =
                            decision.thermalHardStopNs().orElse(0L);
                      }
                    }
                    if (diagnosticWins) {
                      if (current != null) {
                        current.cancelArmEvidence(evaluation.timestampNs());
                      }
                      long afterObservation =
                          evaluation.timestampNs() == Long.MAX_VALUE
                              ? Long.MAX_VALUE
                              : evaluation.timestampNs() + 1;
                      try {
                        lastPoseDecision =
                            poseTriggerControllerLease.captureEnded(
                                () ->
                                    Math.max(
                                        SystemClock.elapsedRealtimeNanos(), afterObservation));
                      } catch (RuntimeException invalidControllerState) {
                        CaptureForegroundService.this.onFailure(invalidControllerState);
                      }
                      return;
                    }
                    String sharedSessionId = "pose-" + UUID.randomUUID();
                    PosePeerArmClient.Candidate candidate =
                        new PosePeerArmClient.Candidate(
                            sharedSessionId,
                            captureConfiguration.nodeId(),
                            evaluation.timestampNs(),
                            evaluation.personConfidence(),
                            evaluation.addressConfidence());
                    if (poseConfiguration.hasPeer()) {
                      peerArmStatus.pending(sharedSessionId);
                      peerExecutor.execute(() -> requestPeerPoseArm(poseConfiguration, candidate));
                    } else {
                      peerArmStatus.reset();
                    }
                    controlExecutor.execute(
                        () -> beginPoseHighSpeed(sharedSessionId, true, decision, null));
                  }
                }

                @Override
                public void onFailure(Throwable failure) {
                  CaptureForegroundService.this.onFailure(failure);
                }
              });
      poseStandbyEngine = created;
      lastPoseMetrics = created.metrics();
      resetStandbyDiagnosticStatus();
      StandbyAudioRecorder createdAudio =
          StandbyAudioRecorder.start(
              this, created::previewEvidenceSnapshot, standbyAudioListener());
      standbyAudioRecorder = createdAudio;
      standbyAudioReady = true;
      RUNTIME.armed();
      updateNotification("Pose standby: 5 Hz inference and impact diagnostics armed");
    } catch (Throwable failure) {
      releaseWakeLock();
      onFailure(failure);
    }
  }

  private PoseStandbyMetrics.Snapshot createdPoseMetrics() {
    PoseStandbyEngine current = poseStandbyEngine;
    return current == null ? lastPoseMetrics : current.metrics();
  }

  private StandbyAudioRecorder.Listener standbyAudioListener() {
    return new StandbyAudioRecorder.Listener() {
      @Override
      public void onReady(int audioSource) {
        standbyAudioSource = audioSource;
      }

      @Override
      public void onAudioMetrics(
          long endFramePosition, ImpactDetector.ProcessResult detectorMetrics) {
        standbyAudioEndFramePosition = endFramePosition;
        standbyAudioPeakAmplitude = detectorMetrics.blockPeakAmplitude();
        standbyAudioNoiseFloor = detectorMetrics.noiseFloorAfterBlock();
        standbyAudioThreshold = detectorMetrics.thresholdAfterBlock();
      }

      @Override
      public void onDetectedImpact(StandbyDiagnosticCoordinator.EventMarker event) {
        registerDetectedStandbyDiagnostic(event);
      }

      @Override
      public void onEvidenceFrozen(StandbyDiagnosticCoordinator.FrozenEvidence evidence) {
        publishStandbyDiagnostic(evidence);
      }

      @Override
      public void onEventDropped(StandbyDiagnosticCoordinator.DroppedEvent event) {
        standbyDiagnosticSessions.discard(event.event().sequence());
        standbyDiagnosticTelemetry.addDroppedEvents(1);
        Log.e(
            TAG,
            "Standby diagnostic event "
                + event.event().sequence()
                + " dropped: "
                + event.reason());
      }

      @Override
      public void onAudioDiscontinuity(
          StandbyDiagnosticCoordinator.StreamDiscontinuity discontinuity) {
        standbyDiagnosticTelemetry.recordDiscontinuity();
        Log.e(
            TAG,
            "Standby audio discontinuity: "
                + discontinuity.kind()
                + " expected="
                + discontinuity.expectedFirstFramePosition()
                + " received="
                + discontinuity.receivedFirstFramePosition());
      }

      @Override
      public void onAudioTimestampRejected(
          AudioTimestampMapper.ObservationResult observation, long rejectionCount) {
        standbyDiagnosticTelemetry.recordTimestampRejections(rejectionCount);
        Log.i(TAG, "Standby audio timestamp rejected: " + observation.rejectionReason());
      }

      @Override
      public void onPreviewSnapshotFailure(Throwable failure) {
        standbyDiagnosticTelemetry.recordError(
            "preview_snapshot:" + failure.getClass().getSimpleName());
        Log.e(TAG, "Unable to snapshot standby preview evidence", failure);
      }

      @Override
      public void onFailure(Throwable failure) {
        standbyDiagnosticTelemetry.recordError(
            "audio_capture:" + failure.getClass().getSimpleName());
        CaptureForegroundService.this.onFailure(failure);
      }
    };
  }

  private void registerDetectedStandbyDiagnostic(
      StandbyDiagnosticCoordinator.EventMarker event) {
    try {
      CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
      synchronized (standbyDiagnosticSessions) {
        standbyDiagnosticSessions.register(
            event.sequence(),
            new StandbyDiagnosticSessionRegistry.PendingSession(
                newSessionId(),
                captureConfiguration.nodeId(),
                System.currentTimeMillis()));
      }
      standbyDiagnosticTelemetry.recordDetectedEvent();
      Log.i(
          TAG,
          "Impact detected while pose standby was not high-speed armed; retaining diagnostics");
    } catch (RuntimeException failure) {
      standbyDiagnosticTelemetry.recordDroppedEvents(
          1, "event_registration:" + failure.getClass().getSimpleName());
      Log.e(TAG, "Unable to register standby impact diagnostics", failure);
    }
  }

  private void publishStandbyDiagnostic(
      StandbyDiagnosticCoordinator.FrozenEvidence evidence) {
    StandbyDiagnosticSessionRegistry.PendingSession pending =
        standbyDiagnosticSessions.claim(evidence.event().sequence()).orElse(null);
    if (pending == null) {
      standbyDiagnosticTelemetry.recordDroppedEvents(1, "publication:unregistered_event");
      Log.e(TAG, "Frozen standby evidence has no registered session identity");
      return;
    }
    try {
      standbyDiagnosticPublisherExecutor.execute(
          () -> {
            try {
              new StandbyDiagnosticSessionPublisher(AndroidDirectorySync::synchronize)
                  .publish(
                      new File(getFilesDir(), "sessions"),
                      pending.sessionId(),
                      pending.sourceNodeId(),
                      pending.createdAtEpochMillis(),
                      evidence);
              standbyDiagnosticTelemetry.recordPublishedSession();
              List<String> deleted = SessionStorage.enforceRetention(this, Collections.emptySet());
              if (!deleted.isEmpty()) {
                Log.i(TAG, "Expired sessions: " + String.join(", ", deleted));
              }
            } catch (Throwable failure) {
              standbyDiagnosticTelemetry.recordDroppedEvents(
                  1, "publication:" + failure.getClass().getSimpleName());
              Log.e(TAG, "Unable to publish standby diagnostic session", failure);
            }
          });
    } catch (RejectedExecutionException rejected) {
      standbyDiagnosticTelemetry.recordDroppedEvents(1, "publication:queue_full");
      Log.e(TAG, "Standby diagnostic publisher queue is full", rejected);
    }
  }

  private void resetStandbyDiagnosticStatus() {
    standbyAudioReady = false;
    standbyAudioSource = -1;
    standbyAudioEndFramePosition = 0;
    standbyAudioPeakAmplitude = 0;
    standbyAudioNoiseFloor = 0;
    standbyAudioThreshold = 0;
    standbyDiagnosticTelemetry.clearLastError();
  }

  private void requestPeerPoseArm(
      PoseStationConfigurationSnapshot poseConfiguration,
      PosePeerArmClient.Candidate candidate) {
    try {
      PosePeerArmClient.Response response =
          new PosePeerArmClient(
                  poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
              .arm(candidate);
      if (!response.accepted()) {
        Log.e(TAG, "Peer rejected pose arm with HTTP " + response.statusCode());
      }
      peerArmStatus.response(candidate.sharedSessionId(), response.accepted(), response.statusCode());
    } catch (Throwable failure) {
      peerArmStatus.failed(candidate.sharedSessionId(), failure);
      Log.e(TAG, "Unable to arm pose peer; local capture is continuing", failure);
    }
  }

  private void beginPoseHighSpeed(
      String sharedSessionId,
      boolean leaderInitiated,
      PoseTriggerController.Decision decision,
      PoseExternalArmLifecycle externalArmLifecycle) {
    WarmCameraLease warmCamera = null;
    try {
      PoseStandbyEngine standby = poseStandbyEngine;
      if (standby == null) {
        throw new IllegalStateException("pose standby disappeared before high-speed transition");
      }
      StandbyAudioRecorder currentAudio = standbyAudioRecorder;
      int lateDiagnosticCount;
      synchronized (this) {
        boolean diagnosticBusy =
            standbyOperatorTagInProgress
                || (currentAudio != null && currentAudio.pendingEventCount() > 0);
        lateDiagnosticCount =
            Math.max(
                standbyDiagnosticSessions.size(),
                currentAudio == null ? 0 : currentAudio.pendingEventCount());
        if (PoseTransitionPrecedence.resolve(true, diagnosticBusy)
            == PoseTransitionPrecedence.Outcome.POSE_WINS_AND_CANCELS_DIAGNOSTIC) {
          poseTransitionLastPrecedence = "pose_arm_before_standby_diagnostic";
          standbyDiagnosticTelemetry.addEventsCancelledForPoseArm(
              Math.max(1, lateDiagnosticCount));
        }
      }
      if (lateDiagnosticCount > 0 || standbyOperatorTagInProgress) {
        // The pose transition already owns the camera. Waiting for the diagnostic's two-second
        // post-roll would miss the backswing, so truncate and audit the lower-priority evidence.
        closeStandbyAudioRecorder();
        currentAudio = null;
      }
      if ((decision == null) == (externalArmLifecycle == null)) {
        throw new IllegalStateException(
            "pose transition must have exactly one local or external controller lifecycle");
      }
      updateNotification(
          leaderInitiated
              ? "Pose detected address; starting 720p240…"
              : "Pose leader requested 720p240…");
      warmCamera = standby.transferToHighSpeed();
      lastPoseMetrics = standby.metrics();
      PreviewEvidenceRing.Snapshot previewSnapshot = standby.previewEvidenceSnapshot();
      poseStandbyEngine = null;
      if (externalArmLifecycle != null) {
        externalArmLifecycle.inferenceQuiesced();
        decision =
            externalArmLifecycle.startController(
                poseTriggerControllerLease, SystemClock::elapsedRealtimeNanos);
        lastPoseDecision = decision;
      }
      PoseHighSpeedNoImpactDeadline.Deadline noImpactDeadline =
          PoseHighSpeedNoImpactDeadline.fromDecision(decision);
      poseNoImpactDeadline = noImpactDeadline;
      poseThermalHardStopElapsedRealtimeNanos = noImpactDeadline.thermalHardStopNs();
      COORDINATION.armed(sharedSessionId);
      RUNTIME.transitioningToHighSpeed();
      if (currentAudio != null) {
        closeStandbyAudioRecorder();
      }
      CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
      armRequestedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
      poseHighSpeedAttempt = true;
      ContinuousCaptureEngine created =
          new ContinuousCaptureEngine(
              this,
              captureConfiguration,
              sharedSessionId,
              poseArmHilEnabled,
              true,
              warmCamera,
              previewSnapshot,
              peerArmStatus,
              this);
      engine = created;
      warmCamera = null;
      created.start();
    } catch (Throwable failure) {
      if (warmCamera != null) {
        warmCamera.close();
      }
      onFailure(failure);
    }
  }

  private PoseStationConfigurationSnapshot requireActivePoseConfiguration() {
    PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
    if (poseConfiguration == null) {
      throw new IllegalStateException("pose configuration snapshot is unavailable");
    }
    return poseConfiguration;
  }

  private void finishSoakHil() {
    if (!soakHilRequested || !continuousHilRequested) {
      throw new IllegalStateException("a soak HIL capture is not active");
    }
    soakCompletionRequested = true;
    ContinuousCaptureEngine current = engine;
    if (current == null) {
      throw new IllegalStateException("the soak HIL capture engine is unavailable");
    }
    current.disableAutomaticTriggersForHil();
    if (RUNTIME.snapshot().state() == CaptureRuntime.State.ARMED) {
      triggerManual();
    }
  }

  private void schedulePoseNoImpactTimeout() {
    cancelPoseNoImpactTimeout();
    long now = SystemClock.elapsedRealtimeNanos();
    PoseHighSpeedNoImpactDeadline.Deadline deadline = poseNoImpactDeadline;
    if (deadline == null) {
      onFailure(new IllegalStateException("Pose no-impact deadline is unavailable"));
      return;
    }
    long delayNanos = deadline.delayFrom(now);
    poseNoImpactTimeout =
        poseTimeoutExecutor.schedule(
            () ->
                controlExecutor.execute(
                    () -> {
                      ContinuousCaptureEngine current = engine;
                      PoseHighSpeedTimeoutLifecycle.Phase phase =
                          PoseHighSpeedTimeoutLifecycle.phase(
                              poseHighSpeedAttempt,
                              current != null,
                              RUNTIME.snapshot().state() == CaptureRuntime.State.ARMED);
                      if (PoseHighSpeedTimeoutLifecycle.action(
                              PoseHighSpeedTimeoutLifecycle.Timeout.ACTIVE_EVIDENCE, phase)
                          != PoseHighSpeedTimeoutLifecycle.Action.RETAIN_NO_IMPACT) {
                        return;
                      }
                      String sessionId = newSessionId();
                      Log.i(
                          TAG,
                          "Pose high-speed no-impact stop: " + deadline.reason());
                      ContinuousCaptureEngine.TriggerAttempt attempt =
                          current.triggerPoseArmedNoImpact(sessionId);
                      if (!attempt.accepted()) {
                        onFailure(
                            new IllegalStateException(
                                "Unable to retain pose no-impact evidence: "
                                    + attempt.diagnostic()));
                      }
                    }),
            delayNanos,
            TimeUnit.NANOSECONDS);
    poseThermalHardStopTimeout =
        poseTimeoutExecutor.schedule(
            () ->
                controlExecutor.execute(
                    () -> {
                      PoseHighSpeedTimeoutLifecycle.Phase phase =
                          PoseHighSpeedTimeoutLifecycle.phase(
                              poseHighSpeedAttempt,
                              engine != null,
                              RUNTIME.snapshot().state() == CaptureRuntime.State.ARMED);
                      if (PoseHighSpeedTimeoutLifecycle.action(
                              PoseHighSpeedTimeoutLifecycle.Timeout.THERMAL_HARD_CAP, phase)
                          != PoseHighSpeedTimeoutLifecycle.Action.FAIL_THERMAL_HARD_CAP) {
                        return;
                      }
                      Log.e(TAG, "Pose high-speed thermal hard cap reached");
                      onFailure(
                          new IllegalStateException(
                              "Pose 240 fps capture exceeded its absolute thermal hard cap"));
                    }),
            deadline.thermalHardCapDelayFrom(now),
            TimeUnit.NANOSECONDS);
  }

  private void cancelPoseActiveEvidenceTimeout() {
    ScheduledFuture<?> timeout = poseNoImpactTimeout;
    poseNoImpactTimeout = null;
    if (timeout != null) {
      timeout.cancel(false);
    }
  }

  private void cancelPoseNoImpactTimeout() {
    cancelPoseActiveEvidenceTimeout();
    ScheduledFuture<?> hardStop = poseThermalHardStopTimeout;
    poseThermalHardStopTimeout = null;
    if (hardStop != null) {
      hardStop.cancel(false);
    }
  }

  private void stopCapture() {
    stopCapture(false);
  }

  private void stopCaptureForPoseRestart() {
    stopCapture(true);
  }

  private void stopCapture(boolean preservePoseController) {
    captureReady = false;
    cancelPoseNoImpactTimeout();
    stopEngineOnly(preservePoseController);
    RUNTIME.stopped();
    releaseWakeLock();
    updateNotification("Node service running; capture is not armed");
  }

  private void stopEngineOnly() {
    stopEngineOnly(false);
  }

  private void stopEngineOnly(boolean preservePoseController) {
    captureReady = false;
    cancelPoseNoImpactTimeout();
    closeStandbyAudioRecorder();
    PoseStandbyEngine currentPose = poseStandbyEngine;
    poseStandbyEngine = null;
    if (currentPose != null) {
      lastPoseMetrics = currentPose.metrics();
      currentPose.close();
    }
    ContinuousCaptureEngine current = engine;
    if (current != null) {
      current.stop();
    }
    synchronized (this) {
      if (engine == current) {
        engine = null;
        activeCaptureConfiguration = null;
        activePoseConfiguration = null;
        poseTransitionRequested = false;
        poseHighSpeedAttempt = false;
        poseNoImpactDeadline = null;
        poseThermalHardStopElapsedRealtimeNanos = 0L;
      }
    }
    if (!preservePoseController) {
      poseTriggerControllerLease.release();
    }
    releaseWakeLock();
  }

  private void closeStandbyAudioRecorder() {
    StandbyAudioRecorder currentAudio = standbyAudioRecorder;
    standbyAudioRecorder = null;
    standbyAudioReady = false;
    if (currentAudio != null) {
      currentAudio.close();
    }
    int incompleteEvents = standbyDiagnosticSessions.clear();
    if (incompleteEvents > 0) {
      standbyDiagnosticTelemetry.recordDroppedEvents(
          incompleteEvents, "audio_capture:closed_before_post_roll");
      Log.e(
          TAG,
          "Closed standby audio before post-roll completed for "
              + incompleteEvents
              + " diagnostic event(s)");
    }
  }

  private CaptureConfigurationSnapshot requireActiveCaptureConfiguration() {
    CaptureConfigurationSnapshot captureConfiguration = activeCaptureConfiguration;
    if (captureConfiguration == null) {
      throw new IllegalStateException("capture configuration snapshot is unavailable");
    }
    return captureConfiguration;
  }

  private void startServer() {
    try {
      CoordinationRecordStore coordinationRecords =
          new CoordinationRecordStore(
              new CoordinationFileTextStore(
                  new File(getFilesDir(), "coordination"), AndroidDirectorySync::synchronize));
      server =
          new NodeHttpServer(
              this,
              configuration,
              RUNTIME,
              COORDINATION,
              coordinationRecords,
              this,
              NodeHttpServer.DEFAULT_PORT);
      server.start();
      advertisedUrls = List.copyOf(server.advertisedUrls());
    } catch (Exception failure) {
      RUNTIME.failed(failure);
      updateNotification("Node API unavailable: " + failure.getMessage());
      Log.e(TAG, "Unable to start node HTTP API", failure);
    }
  }

  private void acquireWakeLock() {
    if (!wakeLock.isHeld()) {
      wakeLock.acquire();
    }
  }

  private void releaseWakeLock() {
    if (wakeLock != null && wakeLock.isHeld()) {
      wakeLock.release();
    }
  }

  private void createNotificationChannel() {
    NotificationChannel channel =
        new NotificationChannel(
            CHANNEL_ID, "Swing capture node", NotificationManager.IMPORTANCE_LOW);
    channel.setDescription("Continuous high-speed camera and impact detection status");
    getSystemService(NotificationManager.class).createNotificationChannel(channel);
  }

  private Notification notification(String message) {
    Intent activityIntent = new Intent(this, MainActivity.class);
    PendingIntent contentIntent =
        PendingIntent.getActivity(
            this,
            0,
            activityIntent,
            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    return new Notification.Builder(this, CHANNEL_ID)
        .setSmallIcon(android.R.drawable.ic_menu_camera)
        .setContentTitle("Swing Capture")
        .setContentText(message)
        .setOngoing(true)
        .setContentIntent(contentIntent)
        .build();
  }

  private void updateNotification(String message) {
    getSystemService(NotificationManager.class).notify(NOTIFICATION_ID, notification(message));
  }

  private static String newSessionId() {
    return "android-"
        + System.currentTimeMillis()
        + "-"
        + UUID.randomUUID().toString().substring(0, 8);
  }

  private void writeContinuousHilReport(String sessionId, Throwable failure) {
    try {
      JSONObject report =
          continuousHilReportBase()
              .put("complete", true)
              .put("passed", failure == null)
              .put("error", failure == null ? "" : String.valueOf(failure.getMessage()));
      ContinuousCaptureContinuityDiagnostic continuityDiagnostic =
          continuityDiagnostic(failure);
      if (continuityDiagnostic != null) {
        report.put("continuity_failure", new JSONObject(continuityDiagnostic.toJson()));
      }
      if (failure == null) {
        String mediaName = requireActiveCaptureConfiguration().mediaFileName();
        report.put("retained_session", retainedSessionHilEvidence(sessionId, mediaName));
      }
      ReportStore.writeLatest(this, report.toString(2) + "\n");
    } catch (Throwable reportFailure) {
      Log.e(TAG, "Unable to write continuous HIL report", reportFailure);
    }
  }

  private void writeContinuousHilArmedReport() {
    try {
      JSONObject report =
          continuousHilReportBase()
              .put("complete", false)
              .put("state", "armed_waiting_audio")
              .put(
                  "audio_diagnostics",
                  new JSONObject()
                      .put("maximum_peak_amplitude", audioPeakAmplitude)
                      .put("noise_floor", audioNoiseFloor)
                      .put("threshold", audioThreshold))
              .put("passed", false)
              .put("error", "");
      ReportStore.writeLatest(this, report.toString(2) + "\n");
    } catch (Throwable reportFailure) {
      onFailure(reportFailure);
    }
  }

  private JSONObject continuousHilReportBase() throws Exception {
    CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
    CaptureProfile profile = captureConfiguration.profile();
    JSONObject report =
        new JSONObject()
        .put("schema_version", 1)
        .put("report_type", "android_continuous_capture")
        .put("created_at_utc", java.time.Instant.now().toString())
        .put("node_id", captureConfiguration.nodeId())
        .put("role", captureConfiguration.role().wireName())
        .put(
            "request",
            new JSONObject()
                .put("width", profile.width())
                .put("height", profile.height())
                .put("frames_per_second", 240)
                .put("duration_ms", 3_000)
                .put("bitrate_bits_per_second", profile.bitrateBitsPerSecond())
                .put("mime", android.media.MediaFormat.MIMETYPE_VIDEO_AVC))
            .put("runtime_telemetry", runtimeTelemetry())
            .put("retain_session_requested", true);
    if (soakHilRequested || soakCompletionRequested || soakIncidentalTriggerCount > 0) {
      report.put(
          "soak_diagnostics",
          new JSONObject()
              .put("completion_requested", soakCompletionRequested)
              .put("incidental_trigger_count", soakIncidentalTriggerCount)
              .put("last_incidental_session_id", soakLastIncidentalSessionId));
    }
    ContinuousCaptureEngine current = engine;
    if (current != null) {
      CaptureStartupTiming startupTiming =
          current.startupTiming(armRequestedElapsedRealtimeNanos).orElse(null);
      if (startupTiming != null) {
        report.put(
            "startup_timing",
            startupTimingJson(startupTiming)
                .put(
                    "startup_continuity_reset_count",
                    Long.toString(current.startupContinuityResetCount()))
                .put(
                    "maximum_startup_continuity_gap_ns",
                    Long.toString(current.maximumStartupContinuityGapNanos())));
      }
    }
    return report;
  }

  private static JSONObject startupTimingJson(CaptureStartupTiming timing) throws Exception {
    return new JSONObject()
        .put("arm_requested_elapsed_realtime_ns", Long.toString(timing.armRequestedNs()))
        .put("engine_started_elapsed_realtime_ns", Long.toString(timing.engineStartedNs()))
        .put("first_camera_frame_elapsed_realtime_ns", Long.toString(timing.firstCameraFrameNs()))
        .put(
            "first_usable_encoded_frame_elapsed_realtime_ns",
            Long.toString(timing.firstUsableEncodedFrameNs()))
        .put(
            "full_pre_roll_ready_elapsed_realtime_ns",
            Long.toString(timing.fullPreRollReadyNs()))
        .put("arm_to_engine_start_ns", Long.toString(timing.armToEngineStartNs()))
        .put("arm_to_first_camera_frame_ns", Long.toString(timing.armToFirstCameraFrameNs()))
        .put(
            "arm_to_first_usable_encoded_frame_ns",
            Long.toString(timing.armToFirstUsableEncodedFrameNs()))
        .put("arm_to_full_pre_roll_ready_ns", Long.toString(timing.armToFullPreRollReadyNs()))
        .put(
            "first_usable_encoded_frame_to_full_pre_roll_ready_ns",
            Long.toString(timing.firstUsableEncodedFrameToFullPreRollReadyNs()));
  }

  private JSONObject runtimeTelemetry() throws Exception {
    CaptureRuntime.Snapshot capture = RUNTIME.snapshot();
    PowerManager power = getSystemService(PowerManager.class);
    float thermalHeadroom = power.getThermalHeadroom(0);
    Runtime processRuntime = Runtime.getRuntime();
    JSONObject telemetry =
        new JSONObject()
            .put("elapsed_realtime_ns", Long.toString(SystemClock.elapsedRealtimeNanos()))
            .put("thermal_status", power.getCurrentThermalStatus())
            .put("video_frames", capture.videoFrames())
            .put("audio_frames", capture.audioFrames())
            .put("ring_bytes", capture.ringBytes())
            .put("ring_duration_us", capture.ringDurationUs())
            .put(
                "java_heap_used_bytes",
                processRuntime.totalMemory() - processRuntime.freeMemory())
            .put("native_heap_allocated_bytes", Debug.getNativeHeapAllocatedSize())
            .put("storage_usable_bytes", getFilesDir().getUsableSpace());
    telemetry.put(
        "thermal_headroom", Float.isFinite(thermalHeadroom) ? thermalHeadroom : JSONObject.NULL);

    Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
    if (battery != null) {
      int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
      int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
      telemetry
          .put(
              "battery_temperature_celsius",
              battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1) / 10.0)
          .put("battery_voltage_millivolts", battery.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1))
          .put("battery_plugged", battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0))
          .put(
              "battery_level_percent",
              level >= 0 && scale > 0 ? (100.0 * level) / scale : JSONObject.NULL);
    }
    return telemetry;
  }

  private JSONObject retainedSessionHilEvidence(String sessionId, String mediaName)
      throws Exception {
    File sessionDirectory = new File(new File(getFilesDir(), "sessions"), sessionId);
    File media = new File(sessionDirectory, mediaName);
    File manifestFile = new File(sessionDirectory, "manifest.json");
    final JSONObject manifest;
    try (FileInputStream input = new FileInputStream(manifestFile)) {
      manifest = new JSONObject(new String(input.readAllBytes(), StandardCharsets.UTF_8));
    }
    if (!sessionId.equals(manifest.getString("session_id"))) {
      throw new IllegalStateException("retained HIL manifest belongs to another session");
    }
    JSONObject retained =
        new JSONObject()
            .put("session_id", sessionId)
            .put("manifest", "sessions/" + sessionId + "/manifest.json")
            .put("media", "sessions/" + sessionId + "/" + mediaName)
            .put("encoded_bytes", media.length());
    JSONObject androidCapture = manifest.getJSONObject("android_capture");
    if (!androidCapture.has("audio_evidence") || androidCapture.isNull("audio_evidence")) {
      return retained;
    }

    JSONObject audio = androidCapture.getJSONObject("audio_evidence");
    Set<String> fields = new HashSet<>();
    Iterator<String> names = audio.keys();
    while (names.hasNext()) {
      fields.add(names.next());
    }
    if (!fields.equals(Pcm16WavFile.MANIFEST_FIELDS)) {
      throw new IllegalStateException("audio evidence manifest fields do not match the contract");
    }
    Pcm16WavFile.EvidenceMetadata metadata =
        new Pcm16WavFile.EvidenceMetadata(
            audio.getString("path"),
            strictJsonInteger(audio, "bytes"),
            Math.toIntExact(strictJsonInteger(audio, "sample_rate_hz")),
            canonicalDecimalLong(audio.get("first_frame_position"), "first_frame_position"),
            canonicalDecimalLong(audio.get("last_frame_position"), "last_frame_position"),
            canonicalDecimalLong(audio.get("strike_frame_position"), "strike_frame_position"),
            Math.toIntExact(strictJsonInteger(audio, "sample_count")),
            Math.toIntExact(strictJsonInteger(audio, "strike_sample_index")));
    File evidence = new File(sessionDirectory, metadata.relativePath());
    if (!sessionDirectory.getCanonicalFile().equals(evidence.getCanonicalFile().getParentFile())
        || !evidence.isFile()
        || evidence.length() != metadata.bytes()) {
      throw new IllegalStateException("audio evidence artifact does not match its manifest");
    }
    retained.put(
        "audio_evidence",
        new JSONObject()
            .put("path", "sessions/" + sessionId + "/" + metadata.relativePath())
            .put("bytes", metadata.bytes())
            .put("sample_rate_hz", metadata.sampleRateHz())
            .put("first_frame_position", Long.toString(metadata.firstFramePosition()))
            .put("last_frame_position", Long.toString(metadata.lastFramePosition()))
            .put("strike_frame_position", Long.toString(metadata.strikeFramePosition()))
            .put("sample_count", metadata.sampleCount())
            .put("strike_sample_index", metadata.strikeSampleIndex()));
    return retained;
  }

  private static long strictJsonInteger(JSONObject object, String name) throws Exception {
    Object value = object.get(name);
    if (!(value instanceof Integer) && !(value instanceof Long)) {
      throw new IllegalArgumentException(name + " must be a JSON integer");
    }
    return ((Number) value).longValue();
  }

  private static long canonicalDecimalLong(Object value, String name) {
    if (!(value instanceof String text)
        || !text.matches("0|[1-9][0-9]*")) {
      throw new IllegalArgumentException(name + " must be a canonical decimal string");
    }
    try {
      return Long.parseLong(text);
    } catch (NumberFormatException failure) {
      throw new IllegalArgumentException(name + " exceeds signed 64-bit range", failure);
    }
  }

  private static ContinuousCaptureContinuityDiagnostic continuityDiagnostic(Throwable failure) {
    Throwable current = failure;
    while (current != null) {
      if (current instanceof ContinuousCaptureContinuityException continuityFailure) {
        return continuityFailure.diagnostic();
      }
      current = current.getCause();
    }
    return null;
  }
}
