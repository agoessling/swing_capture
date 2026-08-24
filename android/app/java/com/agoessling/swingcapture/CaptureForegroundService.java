package com.agoessling.swingcapture;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.BatteryManager;
import android.os.Debug;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;
import com.agoessling.swingcapture.audio.AudioTimestampMapper;
import com.agoessling.swingcapture.audio.ImpactDetector;
import com.agoessling.swingcapture.core.coordination.AutonomousPairLifecycle;
import com.agoessling.swingcapture.core.coordination.ClockOffsetEstimate;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import com.agoessling.swingcapture.diagnostics.PreviewEvidence;
import com.agoessling.swingcapture.diagnostics.PreviewEvidenceRing;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import com.agoessling.swingcapture.pose.PoseLandmarkObservationExtractor;
import com.agoessling.swingcapture.pose.PoseTriggerController;
import com.agoessling.swingcapture.pose.inference.PoseModelVariant;
import com.agoessling.swingcapture.standby.StandbyDiagnosticCoordinator;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
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
  private record SetupPreviewPayload(
      SetupPreviewProvider.Snapshot snapshot, int imageRotationDegrees) {}

  public static final String ACTION_START = "com.agoessling.swingcapture.action.START";
  public static final String ACTION_ARM = "com.agoessling.swingcapture.action.ARM";
  public static final String ACTION_DISARM = "com.agoessling.swingcapture.action.DISARM";
  public static final String ACTION_TRIGGER = "com.agoessling.swingcapture.action.TRIGGER";
  public static final String ACTION_START_POSE_HIL =
      "com.agoessling.swingcapture.action.START_POSE_HIL";
  public static final String ACTION_ARM_POSE_EXPERIMENT_HIL =
      "com.agoessling.swingcapture.action.ARM_POSE_EXPERIMENT_HIL";
  public static final String ACTION_PREPARE_AUTONOMOUS_RECOVERY_HIL =
      "com.agoessling.swingcapture.action.PREPARE_AUTONOMOUS_RECOVERY_HIL";
  public static final String ACTION_ARM_CONTINUOUS_HIL =
      "com.agoessling.swingcapture.action.ARM_CONTINUOUS_HIL";
  public static final String ACTION_ARM_AUDIO_HIL =
      "com.agoessling.swingcapture.action.ARM_AUDIO_HIL";
  public static final String ACTION_ARM_SOAK_HIL =
      "com.agoessling.swingcapture.action.ARM_SOAK_HIL";
  public static final String ACTION_FINISH_SOAK_HIL =
      "com.agoessling.swingcapture.action.FINISH_SOAK_HIL";
  public static final String ACTION_REJECT_NEXT_FIELD_RECORDING_START_HIL =
      "com.agoessling.swingcapture.action.REJECT_NEXT_FIELD_RECORDING_START_HIL";
  public static final String ACTION_FAIL_NEXT_ACCEPTED_FIELD_RECORDING_START_HIL =
      "com.agoessling.swingcapture.action.FAIL_NEXT_ACCEPTED_FIELD_RECORDING_START_HIL";
  public static final String ACTION_RELEASE_ACCEPTED_FIELD_RECORDING_START_FAILURE_HIL =
      "com.agoessling.swingcapture.action.RELEASE_ACCEPTED_FIELD_RECORDING_START_FAILURE_HIL";
  public static final String ACTION_CLEAR_FIELD_RECORDING_START_HIL =
      "com.agoessling.swingcapture.action.CLEAR_FIELD_RECORDING_START_HIL";
  public static final String EXTRA_SHARED_SESSION_ID = "shared_session_id";
  public static final String EXTRA_PEER_NODE_ID = "peer_node_id";
  public static final String EXTRA_POSE_EXPERIMENT_MODEL = "pose_experiment_model";
  public static final String EXTRA_POSE_EXPERIMENT_STANDBY_WIDTH =
      "pose_experiment_standby_width";
  public static final String EXTRA_POSE_EXPERIMENT_STANDBY_HEIGHT =
      "pose_experiment_standby_height";
  private static final String TAG = "SwingCaptureService";
  private static final String CHANNEL_ID = "capture_node";
  private static final int NOTIFICATION_ID = 240;
  private static final int PEER_IMPACT_MAXIMUM_ATTEMPTS = 3;
  private static final long PEER_IMPACT_RETRY_DELAY_MILLIS = 50;
  private static final int PEER_ARM_MAXIMUM_ATTEMPTS = 3;
  private static final int PEER_ARM_READY_MAXIMUM_PENDING_RESPONSES = 50;
  private static final long PEER_ARM_RETRY_DELAY_MILLIS = 150;
  private static final long PEER_CLOCK_POLL_INTERVAL_MILLIS = 1_000;
  private static final long PEER_CLOCK_FAILURE_LOG_INTERVAL_NANOS =
      TimeUnit.SECONDS.toNanos(30);
  private static final long PAIR_NETWORK_HEALTH_POLL_INTERVAL_MILLIS = 10_000;
  private static final int AUTONOMOUS_PEER_MAXIMUM_ATTEMPTS = 3;
  private static final long AUTONOMOUS_PEER_RETRY_DELAY_MILLIS = 50;
  private static final long AUTONOMOUS_PAIR_POLL_INTERVAL_MILLIS = 250;
  // A phone can take roughly ten seconds to become peer-reachable after Wi-Fi reassociation.
  // Keep a pending durable record's retry cadence inside the bounded recovery window.
  private static final long AUTONOMOUS_BACKLOG_RETRY_INTERVAL_NANOS =
      TimeUnit.SECONDS.toNanos(2);
  private static final long SOAK_TELEMETRY_INTERVAL_NANOS = 30_000_000_000L;
  private static final CaptureRuntime RUNTIME = new CaptureRuntime();
  private static final NodeCoordinationState COORDINATION = new NodeCoordinationState();
  private static volatile List<String> advertisedUrls = Collections.emptyList();

  private final FieldRecordingStartFault fieldRecordingStartFault =
      new FieldRecordingStartFault();

  private final ExecutorService controlExecutor = Executors.newSingleThreadExecutor();
  private final ExecutorService peerExecutor = Executors.newSingleThreadExecutor();
  private final ScheduledExecutorService peerClockExecutor =
      Executors.newSingleThreadScheduledExecutor(
          runnable -> new Thread(runnable, "peer-clock-exchange"));
  private final ScheduledExecutorService pairNetworkHealthExecutor =
      Executors.newSingleThreadScheduledExecutor(
          runnable -> new Thread(runnable, "pair-network-health"));
  private final ScheduledExecutorService autonomousPairExecutor =
      Executors.newSingleThreadScheduledExecutor(
          runnable -> new Thread(runnable, "autonomous-pair-lifecycle"));
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
  private final PairNetworkHealthPolicy pairNetworkHealth =
      new PairNetworkHealthPolicy(
          new PairNetworkHealthPolicy.Config(
              TimeUnit.SECONDS.toNanos(30),
              CaptureProfile.standard().bitrateBitsPerSecond(),
              2,
              2,
              3));
  private final PairNetworkHealthPollTask pairNetworkHealthPollTask =
      new PairNetworkHealthPollTask(
          this::pollPairNetworkHealth,
          failure -> {
            invalidatePairNetworkHealthTargetAfterFailure();
            Log.e(
                TAG,
                "Pair network-health poll failed; scheduled sampling will continue",
                failure);
          });
  private final PairNetworkHealthImmediatePoll pairNetworkHealthImmediatePoll =
      new PairNetworkHealthImmediatePoll(
          pairNetworkHealthExecutor, pairNetworkHealthPollTask);
  private final Object pairNetworkHealthTargetMonitor = new Object();
  private PairNetworkHealthTarget reconciledPairNetworkHealthTarget;
  private static final AutonomousPairLifecycle.Config AUTONOMOUS_PAIR_CONFIG =
      new AutonomousPairLifecycle.Config(TimeUnit.SECONDS.toNanos(30));
  private AutonomousPairLifecycle autonomousPair =
      new AutonomousPairLifecycle(AUTONOMOUS_PAIR_CONFIG);
  private final Object peerClockMonitor = new Object();
  private PeerClockSynchronizer peerClockSynchronizer;
  private String peerClockOrigin = "";
  private long lastPeerClockFailureLogElapsedRealtimeNanos;
  private int consecutivePeerClockFailures;
  private NodeConfiguration configuration;
  private DeviceCapabilityPolicy.HardwareSnapshot deviceCapabilities;
  private NodeHttpServer server;
  private volatile CoordinationRecordStore coordinationRecords;
  private volatile AutonomousPairDurableStore autonomousPairDurableStore;
  private volatile PoseStationConfigurationSnapshot autonomousPeerConfiguration;
  private volatile NodeCoordinationState.TriggerReport autonomousLocalTrigger;
  private volatile NodeCoordinationState.TriggerReport autonomousPeerTrigger;
  private volatile PeerClockSynchronizer.Snapshot autonomousPeerClock;
  private volatile PairedCoordinationRecord autonomousPendingRecord;
  private volatile boolean autonomousRecoveredFromCheckpoint;
  private volatile String autonomousRecoveryDiagnostic = "no durable checkpoint";
  private volatile int autonomousReplicationBacklogSize;
  private volatile String autonomousLastBacklogSessionId = "";
  private volatile String autonomousLastBacklogOutcome = "none";
  private volatile boolean autonomousRestartResumeInProgress;
  private volatile long autonomousLastBacklogAttemptElapsedRealtimeNanos;
  private volatile ContinuousCaptureEngine engine;
  private volatile PoseStandbyEngine poseStandbyEngine;
  private final SetupPreviewProvider setupPreviewProvider =
      SetupPreviewProvider.create(
          SetupPreviewProvider.Config.defaults(), new AndroidNv21SetupPreviewEncoder());
  private Object activeSetupPreviewSource;
  private long setupPreviewEvidenceTimestampNanos = -1L;
  private int setupPreviewImageRotationDegrees;
  private volatile StandbyAudioRecorder standbyAudioRecorder;
  private final SessionBoundValue<PeerImpactMappingEvidence> lastPeerImpactMappingEvidence =
      new SessionBoundValue<>();
  private volatile FieldDataRecorder fieldDataRecorder;
  private volatile boolean fieldRecordingStopRequested;
  private volatile CaptureConfigurationSnapshot activeCaptureConfiguration;
  private volatile PoseStationConfigurationSnapshot activePoseConfiguration;
  private volatile PoseStandbyMetrics.Snapshot lastPoseMetrics;
  private volatile PoseStandbyEngine.ReadyStatus lastPoseReadyStatus;
  private volatile PoseTriggerController.Decision lastPoseDecision;
  private volatile PoseExperimentConfiguration poseExperimentConfiguration;
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
    deviceCapabilities = AndroidDeviceCapabilityProbe.collectOrUnavailable(this);
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
    initializeAutonomousPairDurability();
    startServer();
    peerClockExecutor.scheduleWithFixedDelay(
        this::pollPeerClock,
        0,
        PEER_CLOCK_POLL_INTERVAL_MILLIS,
        TimeUnit.MILLISECONDS);
    pairNetworkHealthExecutor.scheduleWithFixedDelay(
        pairNetworkHealthPollTask,
        0,
        PAIR_NETWORK_HEALTH_POLL_INTERVAL_MILLIS,
        TimeUnit.MILLISECONDS);
    autonomousPairExecutor.scheduleWithFixedDelay(
        this::maintainAutonomousPair,
        AUTONOMOUS_PAIR_POLL_INTERVAL_MILLIS,
        AUTONOMOUS_PAIR_POLL_INTERVAL_MILLIS,
        TimeUnit.MILLISECONDS);
    resumeAutonomousPairAfterRestart();
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

  private void initializeAutonomousPairDurability() {
    try {
      AutonomousPairDurableStore durableStore =
          new AutonomousPairDurableStore(
              new File(getFilesDir(), "autonomous_pair"), AndroidDirectorySync::synchronize);
      autonomousPairDurableStore = durableStore;
      Optional<AutonomousPairDurableStore.Recovery> recovered = durableStore.loadCheckpoint();
      autonomousReplicationBacklogSize = durableStore.backlogSize();
      if (recovered.isEmpty()) {
        return;
      }
      AutonomousPairDurableStore.Recovery value = recovered.orElseThrow();
      autonomousPair = AutonomousPairLifecycle.restore(AUTONOMOUS_PAIR_CONFIG, value.checkpoint());
      autonomousPendingRecord = value.pendingRecord().orElse(null);
      autonomousRecoveredFromCheckpoint = true;
      autonomousRecoveryDiagnostic =
          "restored "
              + value.checkpoint().state().name().toLowerCase(java.util.Locale.ROOT)
              + " checkpoint";
    } catch (Throwable failure) {
      autonomousRecoveryDiagnostic =
          "durable recovery failed: " + failure.getClass().getSimpleName();
      autonomousPair.durabilityFailed(failure.getClass().getSimpleName());
      Log.e(TAG, "Unable to restore autonomous-pair durability state", failure);
    }
  }

  private void resumeAutonomousPairAfterRestart() {
    if (!autonomousRecoveredFromCheckpoint) {
      return;
    }
    AutonomousPairLifecycle.State recoveredState = autonomousPair.snapshot().state();
    if (recoveredState == AutonomousPairLifecycle.State.STOPPED
        || recoveredState == AutonomousPairLifecycle.State.TERMINAL_FAILURE) {
      return;
    }
    try {
      PoseStationConfigurationSnapshot poseConfiguration = configuration.poseConfigurationSnapshot();
      if (poseConfiguration.mode() != PoseNodeMode.LEADER || !poseConfiguration.hasPeer()) {
        handleAutonomousTransition(
            autonomousPair.durabilityFailed(
                "restored leader checkpoint has no configured authenticated peer"));
        return;
      }
      autonomousPeerConfiguration = poseConfiguration;
      if (recoveredState != AutonomousPairLifecycle.State.STOPPING) {
        autonomousRestartResumeInProgress = true;
        try {
          setArmedInternal(true, null, false, false, false);
        } finally {
          autonomousRestartResumeInProgress = false;
        }
      }
      boolean durableRecord = autonomousPendingRecord != null;
      handleAutonomousTransition(
          autonomousPair.recover(durableRecord, SystemClock.elapsedRealtimeNanos()));
      autonomousRecoveryDiagnostic =
          "recovery actions dispatched for existing checkpoint session";
    } catch (Throwable failure) {
      autonomousRestartResumeInProgress = false;
      autonomousRecoveryDiagnostic =
          "checkpoint resume failed: " + failure.getClass().getSimpleName();
      handleAutonomousTransition(
          autonomousPair.durabilityFailed(failure.getClass().getSimpleName()));
      Log.e(TAG, "Unable to resume autonomous pair after process restart", failure);
    }
  }

  /** Builds deterministic published evidence for the bounded process-restart HIL only. */
  private synchronized void prepareAutonomousRecoveryHil(
      String sharedSessionId, String peerNodeId) throws IOException {
    PoseHilEndpointAccess.requireEnabled(poseArmHilEnabled);
    com.agoessling.swingcapture.node.NodeCoordinationState.validateSharedSessionId(sharedSessionId);
    if (peerNodeId == null || !peerNodeId.matches("[A-Za-z0-9._-]{1,128}")) {
      throw new IllegalArgumentException("peer node ID is invalid");
    }
    if (autonomousPair.snapshot().state() != AutonomousPairLifecycle.State.STOPPED
        || RUNTIME.snapshot().state() != CaptureRuntime.State.STOPPED) {
      throw new IllegalStateException("autonomous recovery HIL requires a stopped station");
    }
    PoseStationConfigurationSnapshot poseConfiguration = configuration.poseConfigurationSnapshot();
    if (poseConfiguration.mode() != PoseNodeMode.LEADER || !poseConfiguration.hasPeer()) {
      throw new IllegalStateException("autonomous recovery HIL requires a configured leader peer");
    }
    AutonomousPairDurableStore durableStore =
        Objects.requireNonNull(autonomousPairDurableStore, "autonomous durable store");
    if (durableStore.loadCheckpoint().isPresent()) {
      throw new IllegalStateException("autonomous recovery HIL refuses to replace a checkpoint");
    }
    CoordinationRecordStore recordStore =
        Objects.requireNonNull(coordinationRecords, "coordination record store");
    CaptureConfigurationSnapshot capture = configuration.captureSnapshot();
    capture.requireAssignedRole();
    com.agoessling.swingcapture.core.coordination.CaptureRole localRole =
        com.agoessling.swingcapture.core.coordination.CaptureRole.parse(capture.role().wireName());
    com.agoessling.swingcapture.core.coordination.CaptureRole peerRole =
        localRole == com.agoessling.swingcapture.core.coordination.CaptureRole.DOWN_THE_LINE
            ? com.agoessling.swingcapture.core.coordination.CaptureRole.FACE_ON
            : com.agoessling.swingcapture.core.coordination.CaptureRole.DOWN_THE_LINE;
    long timestamp = SystemClock.elapsedRealtimeNanos();
    PairedCoordinationRecord.NodeEvidence local =
        autonomousRecoveryHilEvidence(
            localRole,
            capture.nodeId(),
            "hil-local-" + sharedSessionId,
            timestamp,
            "hil_restart_local");
    PairedCoordinationRecord.NodeEvidence peer =
        autonomousRecoveryHilEvidence(
            peerRole,
            peerNodeId,
            "hil-peer-" + sharedSessionId,
            Math.addExact(timestamp, TimeUnit.MILLISECONDS.toNanos(1)),
            "hil_restart_peer");
    PairedCoordinationRecord record =
        localRole == com.agoessling.swingcapture.core.coordination.CaptureRole.DOWN_THE_LINE
            ? PairedCoordinationRecord.create(
                sharedSessionId, System.currentTimeMillis(), local, peer)
            : PairedCoordinationRecord.create(
                sharedSessionId, System.currentTimeMillis(), peer, local);

    AutonomousPairLifecycle prepared = new AutonomousPairLifecycle(AUTONOMOUS_PAIR_CONFIG);
    prepared.startStation();
    prepared.peerStandbyStarted();
    prepared.claimSwing(sharedSessionId, timestamp);
    prepared.peerArmAccepted(sharedSessionId);
    prepared.localTriggered(sharedSessionId, local.localSessionId());
    prepared.peerTriggered(sharedSessionId, peer.localSessionId(), true);
    prepared.localPublished(sharedSessionId, local.localSessionId());
    prepared.peerPublished(sharedSessionId, peer.localSessionId());
    prepared.pairAdmitted(sharedSessionId);

    CoordinationRecordStore.StoreStatus localStore = recordStore.storeIfAbsent(record).status();
    if (localStore == CoordinationRecordStore.StoreStatus.CONFLICT) {
      throw new IOException("autonomous recovery HIL local record conflicts");
    }
    AutonomousPairDurableStore.EnqueueStatus queued = durableStore.enqueue(record);
    if (queued == AutonomousPairDurableStore.EnqueueStatus.CONFLICT) {
      throw new IOException("autonomous recovery HIL backlog conflicts");
    }
    autonomousPair = prepared;
    autonomousPendingRecord = record;
    autonomousPeerConfiguration = poseConfiguration;
    durableStore.saveCheckpoint(prepared.checkpoint(), Optional.of(record));
    autonomousReplicationBacklogSize = durableStore.backlogSize();
    autonomousLastBacklogSessionId = sharedSessionId;
    autonomousLastBacklogOutcome = "hil_queued_for_restart";
    autonomousRecoveryDiagnostic = "deterministic HIL checkpoint ready for force-stop";
  }

  private static PairedCoordinationRecord.NodeEvidence autonomousRecoveryHilEvidence(
      com.agoessling.swingcapture.core.coordination.CaptureRole role,
      String nodeId,
      String localSessionId,
      long timestampNanos,
      String source) {
    return new PairedCoordinationRecord.NodeEvidence(
        role,
        nodeId,
        localSessionId,
        timestampNanos,
        TimeUnit.MICROSECONDS.toNanos(100),
        timestampNanos,
        TimeUnit.MICROSECONDS.toNanos(100),
        0,
        0,
        TimeUnit.MILLISECONDS.toNanos(1),
        TimeUnit.MILLISECONDS.toNanos(1),
        3,
        source);
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    String action = intent == null ? ACTION_START : intent.getAction();
    if (ACTION_START_POSE_HIL.equals(action)) {
      poseArmHilEnabled = true;
    }
    if (ACTION_REJECT_NEXT_FIELD_RECORDING_START_HIL.equals(action)) {
      fieldRecordingStartFault.arm();
      Log.i(TAG, "Next field-recording start will be rejected by explicit HIL request");
    }
    if (ACTION_FAIL_NEXT_ACCEPTED_FIELD_RECORDING_START_HIL.equals(action)) {
      fieldRecordingStartFault.armAcceptedFailure();
      Log.i(TAG, "Next accepted field-recording start will wait at its explicit HIL fault gate");
    }
    if (ACTION_RELEASE_ACCEPTED_FIELD_RECORDING_START_FAILURE_HIL.equals(action)) {
      boolean released = fieldRecordingStartFault.releaseAcceptedStartFailure();
      Log.i(TAG, "Accepted field-recording start HIL fault release=" + released);
    }
    if (ACTION_CLEAR_FIELD_RECORDING_START_HIL.equals(action)) {
      fieldRecordingStartFault.clear();
      Log.i(TAG, "Cleared explicit field-recording start HIL request");
    }
    if (ACTION_PREPARE_AUTONOMOUS_RECOVERY_HIL.equals(action)) {
      try {
        prepareAutonomousRecoveryHil(
            intent == null ? null : intent.getStringExtra(EXTRA_SHARED_SESSION_ID),
            intent == null ? null : intent.getStringExtra(EXTRA_PEER_NODE_ID));
      } catch (Throwable failure) {
        autonomousRecoveryDiagnostic =
            "HIL checkpoint preparation failed: " + failure.getClass().getSimpleName();
        Log.e(TAG, "Unable to prepare autonomous recovery HIL checkpoint", failure);
      }
    } else if (ACTION_ARM.equals(action)
        || ACTION_ARM_POSE_EXPERIMENT_HIL.equals(action)
        || ACTION_ARM_CONTINUOUS_HIL.equals(action)
        || ACTION_ARM_AUDIO_HIL.equals(action)
        || ACTION_ARM_SOAK_HIL.equals(action)) {
      boolean previousContinuousHilRequested = continuousHilRequested;
      boolean previousAudioHilRequested = audioHilRequested;
      boolean previousSoakHilRequested = soakHilRequested;
      boolean previousCaptureReady = captureReady;
      PoseExperimentConfiguration previousPoseExperiment = poseExperimentConfiguration;
      try {
        poseExperimentConfiguration =
            ACTION_ARM_POSE_EXPERIMENT_HIL.equals(action)
                ? requirePoseExperimentConfiguration(intent)
                : null;
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
        String requestedSessionId =
            intent == null ? null : intent.getStringExtra(EXTRA_SHARED_SESSION_ID);
        if (ACTION_ARM_POSE_EXPERIMENT_HIL.equals(action)) {
          setArmedInternal(true, requestedSessionId, false, false, false);
        } else {
          setArmedInternal(true, requestedSessionId, false, false, false);
        }
      } catch (IllegalArgumentException | IllegalStateException rejected) {
        continuousHilRequested = previousContinuousHilRequested;
        audioHilRequested = previousAudioHilRequested;
        soakHilRequested = previousSoakHilRequested;
        captureReady = previousCaptureReady;
        poseExperimentConfiguration = previousPoseExperiment;
        Log.i(TAG, "Arm request rejected: " + rejected.getMessage());
      }
    } else if (ACTION_FINISH_SOAK_HIL.equals(action)) {
      try {
        finishSoakHil();
      } catch (IllegalStateException rejected) {
        onFailure(rejected);
      }
    } else if (ACTION_DISARM.equals(action)) {
      setArmedInternal(false, null, false, false, false);
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
    fieldRecordingStartFault.clear();
    stopFieldRecordingNow(fieldDataRecorder);
    setupPreviewProvider.close();
    if (server != null) {
      server.stop();
    }
    advertisedUrls = Collections.emptyList();
    drainStandbyDiagnosticPublisher();
    controlExecutor.shutdownNow();
    peerExecutor.shutdownNow();
    peerClockExecutor.shutdownNow();
    pairNetworkHealthExecutor.shutdownNow();
    autonomousPairExecutor.shutdownNow();
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
  public void setArmed(
      boolean armed, String sharedSessionId, boolean allowDegradedNetwork) {
    if (!armed && allowDegradedNetwork) {
      throw new IllegalArgumentException(
          "allow_degraded_network is valid only while arming");
    }
    if (armed) {
      poseExperimentConfiguration = null;
    }
    setArmedInternal(armed, sharedSessionId, false, true, allowDegradedNetwork);
  }

  private static PoseExperimentConfiguration requirePoseExperimentConfiguration(Intent intent) {
    if (intent == null) {
      throw new IllegalArgumentException("pose experiment HIL requires explicit configuration");
    }
    return PoseExperimentConfiguration.parse(
        intent.getStringExtra(EXTRA_POSE_EXPERIMENT_MODEL),
        intent.getIntExtra(EXTRA_POSE_EXPERIMENT_STANDBY_WIDTH, -1),
        intent.getIntExtra(EXTRA_POSE_EXPERIMENT_STANDBY_HEIGHT, -1));
  }

  private void setArmedInternal(
      boolean armed,
      String sharedSessionId,
      boolean preserveCompletedTriggerReport,
      boolean enforcePairNetworkAdmission,
      boolean allowDegradedNetwork) {
    if (!armed) {
      if (preserveCompletedTriggerReport) {
        throw new IllegalArgumentException("disarming cannot preserve a trigger report");
      }
      if (sharedSessionId != null) {
        throw new IllegalArgumentException("shared_session_id is valid only while arming");
      }
      lastPeerImpactMappingEvidence.clear();
      PoseStationConfigurationSnapshot stoppingPose = activePoseConfiguration;
      if (stoppingPose != null && stoppingPose.mode() == PoseNodeMode.LEADER) {
        handleAutonomousTransition(autonomousPair.stopStation());
      }
      controlExecutor.execute(this::stopCapture);
      return;
    }
    PoseStationConfigurationSnapshot requestedPoseConfiguration;
    long stationArmRequestedElapsedRealtimeNanos;
    synchronized (this) {
      lastPeerImpactMappingEvidence.clear();
      if (fieldRecordingInProgress(fieldDataRecorder)) {
        throw new IllegalStateException("field data recording is active");
      }
      CaptureRuntime.Snapshot snapshot = RUNTIME.snapshot();
      CaptureConfigurationSnapshot requestedConfiguration = configuration.captureSnapshot();
      requestedConfiguration.requireAssignedRole();
      DeviceCapabilityPolicy.assess(currentDeviceCapabilities(), requestedConfiguration.profile())
          .requireReady();
      requestedPoseConfiguration = configuration.poseConfigurationSnapshot();
      if (enforcePairNetworkAdmission) {
        PairNetworkArmPolicy.requireAllowed(
            true,
            requestedPoseConfiguration.mode(),
            pairNetworkHealthStatus().state(),
            allowDegradedNetwork);
      }
      if (!preserveCompletedTriggerReport
          && requestedPoseConfiguration.mode() == PoseNodeMode.SHADOW
          && activePoseConfiguration != null
          && activePoseConfiguration.mode() == PoseNodeMode.SHADOW
          && (snapshot.state() == CaptureRuntime.State.STARTING
              || snapshot.state() == CaptureRuntime.State.ARMED)) {
        // Station-start retransmission and the legacy browser's concurrent shadow arm are both
        // idempotent. The per-swing shared ID still arrives through the strict pose-arm endpoint.
        return;
      }
      if (snapshot.state() != CaptureRuntime.State.STOPPED
          && snapshot.state() != CaptureRuntime.State.ERROR) {
        throw new IllegalStateException("capture is already running");
      }
      if (activeCaptureConfiguration != null) {
        throw new IllegalStateException("the previous capture is still shutting down");
      }
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
      stationArmRequestedElapsedRealtimeNanos = armRequestedElapsedRealtimeNanos;
    }
    if (!preserveCompletedTriggerReport
        && !autonomousRestartResumeInProgress
        && requestedPoseConfiguration.mode() == PoseNodeMode.LEADER
        && requestedPoseConfiguration.hasPeer()) {
      autonomousPeerConfiguration = requestedPoseConfiguration;
      handleAutonomousTransition(autonomousPair.startStation());
    }
    if (!continuousHilRequested
        && activePoseConfiguration.mode() != PoseNodeMode.DISABLED) {
      updateNotification("Starting 5 Hz pose standby…");
      controlExecutor.execute(this::startPoseStandby);
    } else {
      updateNotification("Starting camera, microphone, and encoded pre-roll…");
      controlExecutor.execute(
          () -> startCapture(stationArmRequestedElapsedRealtimeNanos));
    }
  }

  @Override
  public JSONObject fieldRecordingStatus() {
    FieldDataRecorder current = fieldDataRecorder;
    JSONObject status = new JSONObject();
    try {
      status
          .put("schema_version", 1)
          .put("max_duration_seconds", FieldDataRecorder.MAXIMUM_DURATION_MILLIS / 1_000L)
          .put("hil_reject_next_start", fieldRecordingStartFault.armed())
          .put(
              "hil_fail_next_accepted_start",
              fieldRecordingStartFault.acceptedFailureArmed())
          .put(
              "hil_accepted_start_waiting",
              fieldRecordingStartFault.acceptedStartWaiting());
      if (current == null) {
        return status
            .put("state", "idle")
            .put("active_recording_id", JSONObject.NULL)
            .put("shared_recording_id", JSONObject.NULL)
            .put("started_at_utc", JSONObject.NULL)
            .put("started_elapsed_realtime_ns", JSONObject.NULL)
            .put("elapsed_ms", 0)
            .put("video_bytes", "0")
            .put("audio_frames", "0")
            .put("error", "");
      }

      FieldDataRecorder.Status recorderStatus = current.status();
      boolean active = fieldRecordingInProgress(recorderStatus);
      String wireState = fieldRecordingState(recorderStatus, fieldRecordingStopRequested);
      return status
          .put("state", wireState)
          .put(
              "active_recording_id",
              active ? recorderStatus.recordingId() : JSONObject.NULL)
          .put("shared_recording_id", recorderStatus.sharedRecordingId())
          .put("started_at_utc", recorderStatus.createdAtUtc())
          .put(
              "started_elapsed_realtime_ns",
              recorderStatus.startedElapsedRealtimeNs() == 0
                  ? JSONObject.NULL
                  : Long.toString(recorderStatus.startedElapsedRealtimeNs()))
          .put("elapsed_ms", recorderStatus.elapsedMillis())
          .put("video_bytes", Long.toString(recorderStatus.videoBytes()))
          .put("audio_frames", Long.toString(recorderStatus.audioFrames()))
          .put("error", recorderStatus.error());
    } catch (org.json.JSONException impossible) {
      throw new IllegalStateException("Unable to serialize field recording status", impossible);
    }
  }

  @Override
  public JSONObject startFieldRecording(String sharedRecordingId) {
    CaptureConfigurationSnapshot captureConfiguration = configuration.captureSnapshot();
    captureConfiguration.requireAssignedRole();
    FieldDataRecorder created;
    synchronized (this) {
      if (fieldRecordingInProgress(fieldDataRecorder)) {
        throw new IllegalStateException("field data recording is already active");
      }
      fieldRecordingStartFault.rejectIfArmed();
      CaptureRuntime.Snapshot snapshot = RUNTIME.snapshot();
      if ((snapshot.state() != CaptureRuntime.State.STOPPED
              && snapshot.state() != CaptureRuntime.State.ERROR)
          || activeCaptureConfiguration != null
          || engine != null
          || poseStandbyEngine != null
          || standbyAudioRecorder != null) {
        throw new IllegalStateException("disarm high-speed capture before field recording");
      }
      String recordingId = sharedRecordingId;
      created =
          new FieldDataRecorder(
              this,
              FieldDataRecorder.Config.tenMinutePortrait(
                  recordingId,
                  sharedRecordingId,
                  captureConfiguration.nodeId(),
                  captureConfiguration.role().wireName()),
              new FieldDataRecorder.Listener() {
                @Override
                public void onCompleted(FieldDataRecorder.Result ignored) {
                  finishFieldRecordingNotification(recordingId, false);
                }

                @Override
                public void onFailure(Throwable failure) {
                  Log.e(TAG, "Field data recording failed", failure);
                  finishFieldRecordingNotification(recordingId, true);
                }
              });
      fieldDataRecorder = created;
      fieldRecordingStopRequested = false;
    }
    updateNotification("Starting 720p field data recording…");
    controlExecutor.execute(() -> startFieldRecordingNow(created));
    return fieldRecordingStatus();
  }

  @Override
  public JSONObject stopFieldRecording() {
    FieldDataRecorder current = fieldDataRecorder;
    FieldDataRecorder.Status currentStatus = current == null ? null : current.status();
    boolean active = currentStatus != null && fieldRecordingInProgress(currentStatus);
    boolean failed =
        currentStatus != null && currentStatus.state() == FieldDataRecorder.State.FAILED;
    FieldRecordingStopPolicy.Action action =
        FieldRecordingStopPolicy.decide(current != null, active, failed);
    if (action == FieldRecordingStopPolicy.Action.NO_OP) {
      return fieldRecordingStatus();
    }
    if (action == FieldRecordingStopPolicy.Action.ACKNOWLEDGE_FAILED) {
      JSONObject failure = fieldRecordingStatus();
      synchronized (this) {
        if (fieldDataRecorder != current
            || current.status().state() != FieldDataRecorder.State.FAILED) {
          return fieldRecordingStatus();
        }
        fieldDataRecorder = null;
        fieldRecordingStopRequested = false;
      }
      releaseWakeLock();
      updateNotification("Field data recording failure acknowledged; ready to retry");
      try {
        return failure
            .put("state", "idle")
            .put("active_recording_id", JSONObject.NULL)
            .put("acknowledged_terminal_failure", true);
      } catch (org.json.JSONException impossible) {
        throw new IllegalStateException(
            "Unable to acknowledge field recording failure", impossible);
      }
    }
    fieldRecordingStopRequested = true;
    updateNotification("Stopping and publishing field data recording…");
    controlExecutor.execute(() -> stopFieldRecordingNow(current));
    return fieldRecordingStatus();
  }

  private void startFieldRecordingNow(FieldDataRecorder recorder) {
    try {
      acquireWakeLock();
      recorder.start(fieldRecordingStartFault::failAcceptedStartIfArmed);
      updateNotification("Recording 720p video and raw audio for field analysis");
    } catch (Throwable failure) {
      Log.e(TAG, "Unable to start field data recording", failure);
      releaseWakeLock();
    }
  }

  private void stopFieldRecordingNow(FieldDataRecorder recorder) {
    if (recorder == null) {
      return;
    }
    try {
      if (fieldRecordingInProgress(recorder)) {
        recorder.stop();
      }
    } catch (Throwable failure) {
      Log.e(TAG, "Unable to stop field data recording", failure);
    } finally {
      boolean acknowledgedFailure = false;
      synchronized (this) {
        if (fieldDataRecorder == recorder) {
          fieldRecordingStopRequested = false;
          if (recorder.status().state() == FieldDataRecorder.State.FAILED) {
            fieldDataRecorder = null;
            acknowledgedFailure = true;
          }
        }
      }
      releaseWakeLock();
      if (acknowledgedFailure) {
        updateNotification("Field data recording failure acknowledged; ready to retry");
      } else if (fieldDataRecorder == recorder) {
        updateNotification(
            recorder.status().state() == FieldDataRecorder.State.FAILED
                ? "Field data recording failed"
                : "Field data recording ready to download");
      }
    }
  }

  private void finishFieldRecordingNotification(String recordingId, boolean failed) {
    FieldDataRecorder current = fieldDataRecorder;
    if (current == null || !current.status().recordingId().equals(recordingId)) {
      return;
    }
    fieldRecordingStopRequested = false;
    releaseWakeLock();
    updateNotification(
        failed
            ? "Field data recording failed"
            : "Field data recording ready to download");
  }

  private static boolean fieldRecordingInProgress(FieldDataRecorder recorder) {
    if (recorder == null) {
      return false;
    }
    return fieldRecordingInProgress(recorder.status());
  }

  private static boolean fieldRecordingInProgress(FieldDataRecorder.Status status) {
    return switch (status.state()) {
      case NEW, STARTING, RECORDING, STOPPING -> true;
      case COMPLETED, FAILED, CLOSED -> false;
    };
  }

  private static String fieldRecordingState(
      FieldDataRecorder.Status status, boolean stopRequested) {
    if (stopRequested
        && (status.state() == FieldDataRecorder.State.NEW
            || status.state() == FieldDataRecorder.State.STARTING
            || status.state() == FieldDataRecorder.State.RECORDING)) {
      return "stopping";
    }
    return switch (status.state()) {
      case NEW, STARTING -> "starting";
      case RECORDING -> "recording";
      case STOPPING -> "stopping";
      case COMPLETED -> "ready";
      case FAILED -> "error";
      case CLOSED -> status.error().isEmpty() ? "ready" : "error";
    };
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
  public boolean triggerPoseArm(PosePeerArmClient.Candidate candidate) {
    if (candidate == null) {
      throw new IllegalArgumentException("pose-arm candidate is required");
    }
    PoseExternalArmLifecycle externalArmLifecycle;
    long highSpeedArmRequestedElapsedRealtimeNanos;
    synchronized (this) {
      PeerArmStatusTracker.InboundRequestDisposition disposition =
          peerArmStatus.classifyInboundRequest(candidate.sharedSessionId());
      if (disposition == PeerArmStatusTracker.InboundRequestDisposition.STALE) {
        throw new IllegalStateException("pose-arm session was already completed or abandoned");
      }
      if (disposition == PeerArmStatusTracker.InboundRequestDisposition.DUPLICATE_ACTIVE) {
        if (poseHighSpeedAttempt) {
          return captureReady
              && engine != null
              && RUNTIME.snapshot().state() == CaptureRuntime.State.ARMED;
        }
        if (poseTransitionRequested) {
          return false;
        }
        throw new IllegalStateException("the duplicated pose-arm session is no longer active");
      }
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
      highSpeedArmRequestedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
      armRequestedElapsedRealtimeNanos = highSpeedArmRequestedElapsedRealtimeNanos;
    }
    controlExecutor.execute(
        () ->
            beginPoseHighSpeed(
                candidate.sharedSessionId(),
                false,
                null,
                externalArmLifecycle,
                highSpeedArmRequestedElapsedRealtimeNanos));
    return false;
  }

  @Override
  public void triggerPoseImpact(PosePeerArmClient.ImpactTrigger trigger) {
    if (trigger == null) {
      throw new IllegalArgumentException("pose-impact trigger is required");
    }
    ContinuousCaptureEngine current;
    synchronized (this) {
      PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
      CaptureConfigurationSnapshot captureConfiguration = activeCaptureConfiguration;
      current = engine;
      if (poseConfiguration == null
          || captureConfiguration == null
          || poseConfiguration.mode() != PoseNodeMode.SHADOW
          || !poseHighSpeedAttempt
          || current == null
          || RUNTIME.snapshot().state() != CaptureRuntime.State.ARMED) {
        throw new IllegalStateException("shadow high-speed capture is not ready");
      }
      if (captureConfiguration.nodeId().equals(trigger.leaderNodeId())) {
        throw new IllegalArgumentException("pose leader and peer must be distinct nodes");
      }
      if (trigger.hasClockMapping()
          && !captureConfiguration.nodeId().equals(trigger.targetPeerNodeId())) {
        throw new IllegalArgumentException("peer impact clock mapping targets another node");
      }
    }
    boolean mappingWithinPolicy =
        trigger.hasClockMapping()
            && trigger.mappingAgeAtSendNanos()
                <= PeerClockSynchronizer.MAXIMUM_SNAPSHOT_AGE_NANOS
            && trigger.mappingUncertaintyNanos()
                <= PeerClockSynchronizer.MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS;
    PosePeerArmClient.ImpactTrigger effectiveTrigger =
        !trigger.hasClockMapping() || mappingWithinPolicy
            ? trigger
            : new PosePeerArmClient.ImpactTrigger(
                trigger.sharedSessionId(),
                trigger.leaderNodeId(),
                trigger.leaderTriggerElapsedRealtimeNanos());
    ContinuousCaptureEngine.TriggerAttempt attempt =
        current.triggerPeerImpact(trigger, effectiveTrigger);
    if (!attempt.accepted()) {
      throw new IllegalStateException(attempt.diagnostic());
    }
    NodeCoordinationState.TriggerReport report = COORDINATION.latestTrigger();
    if (report != null && report.sharedSessionId().equals(trigger.sharedSessionId())) {
      lastPeerImpactMappingEvidence.set(
          trigger.sharedSessionId(),
          new PeerImpactMappingEvidence(trigger, mappingWithinPolicy, report.source()));
    }
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
      armRequestedElapsedRealtimeNanos = candidateTimestamp;
    }
    PosePeerArmClient.Candidate candidate =
        new PosePeerArmClient.Candidate(
            sharedSessionId,
            captureConfiguration.nodeId(),
            candidateTimestamp,
            1.0,
            1.0);
    handleAutonomousTransition(
        autonomousPair.claimSwing(sharedSessionId, candidateTimestamp));
    peerExecutor.execute(() -> requestPeerPoseArm(poseConfiguration, candidate));
    controlExecutor.execute(
        () ->
            beginPoseHighSpeed(
                sharedSessionId,
                true,
                null,
                externalArmLifecycle,
                candidateTimestamp));
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
    String posePhase = poseHighSpeedAttempt ? "high_speed" : "idle";
    if (standby != null) {
      if (metrics == null || metrics.warmupInferenceCount() == 0) {
        posePhase = "warming_up";
      } else if (metrics.failedWarmupInferences() != 0) {
        posePhase = "warmup_failed";
      } else {
        posePhase = "monitoring";
      }
    }
    PoseTriggerController.Decision decision = lastPoseDecision;
    PoseExperimentConfiguration experiment = poseExperimentConfiguration;
    PoseStandbyEngine.ReadyStatus readyStatus = lastPoseReadyStatus;
    PoseModelVariant modelVariant =
        readyStatus != null
            ? readyStatus.modelVariant()
            : (experiment == null ? PoseModelVariant.productionDefault() : experiment.modelVariant());
    StandbyDiagnosticTelemetry.Snapshot diagnosticTelemetry =
        standbyDiagnosticTelemetry.snapshot();
    JSONObject status = new JSONObject();
    try {
      status
          .put("mode", poseConfiguration.mode().wireName())
          .put("hil_pose_arm_enabled", poseArmHilEnabled)
          .put("configured_delegate", poseConfiguration.delegateWireName())
          .put("experiment_enabled", experiment != null)
          .put("model_variant", modelVariant.wireName())
          .put("model_asset_path", modelVariant.assetPath())
          .put(
              "requested_standby_width",
              experiment == null ? JSONObject.NULL : experiment.standbyWidth())
          .put(
              "requested_standby_height",
              experiment == null ? JSONObject.NULL : experiment.standbyHeight())
          .put(
              "actual_standby_width",
              readyStatus == null ? JSONObject.NULL : readyStatus.standbySize().getWidth())
          .put(
              "actual_standby_height",
              readyStatus == null ? JSONObject.NULL : readyStatus.standbySize().getHeight())
          .put("debug_evidence_enabled", poseConfiguration.debugEvidenceEnabled())
          .put("process_cpu_time_ms", android.os.Process.getElapsedCpuTime())
          .put("peer_configured", poseConfiguration.hasPeer())
          .put("phase", posePhase)
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
                .put("successful_warmup_inferences", metrics.successfulWarmupInferences())
                .put("failed_warmup_inferences", metrics.failedWarmupInferences())
                .put("total_warmup_duration_ns", metrics.totalWarmupDurationNs())
                .put("maximum_warmup_duration_ns", metrics.maximumWarmupDurationNs())
                .put("successful_inferences", metrics.successfulInferences())
                .put("failed_inferences", metrics.failedInferences())
                .put("mean_inference_duration_ms", metrics.meanInferenceDurationMs())
                .put("maximum_inference_duration_ns", metrics.maximumInferenceDurationNs())
                .put("inference_duration_p50_ns", metrics.inferenceDurationP50Ns())
                .put("inference_duration_p90_ns", metrics.inferenceDurationP90Ns())
                .put("inference_duration_p95_ns", metrics.inferenceDurationP95Ns())
                .put("inference_duration_p99_ns", metrics.inferenceDurationP99Ns())
                .put("inference_deadline_ns", PoseStandbyMetrics.INFERENCE_DEADLINE_NS)
                .put("inference_outlier_bound_ns", PoseStandbyMetrics.INFERENCE_OUTLIER_BOUND_NS)
                .put("inference_deadline_misses", metrics.inferenceDeadlineMisses())
                .put("inference_outliers", metrics.inferenceOutliers())
                .put("recent_inference_sample_count", metrics.recentInferenceSampleCount())
                .put(
                    "recent_inference_duration_p95_ns",
                    metrics.recentInferenceDurationP95Ns())
                .put(
                    "recent_maximum_inference_duration_ns",
                    metrics.recentMaximumInferenceDurationNs())
                .put(
                    "recent_inference_deadline_misses",
                    metrics.recentInferenceDeadlineMisses())
                .put("decision_age_samples", metrics.decisionAgeSamples())
                .put("rejected_decision_timestamps", metrics.rejectedDecisionTimestamps())
                .put("mean_decision_age_ms", metrics.meanDecisionAgeMs())
                .put("maximum_decision_age_ns", metrics.maximumDecisionAgeNs())
                .put("decision_age_p50_ns", metrics.decisionAgeP50Ns())
                .put("decision_age_p90_ns", metrics.decisionAgeP90Ns())
                .put("decision_age_p95_ns", metrics.decisionAgeP95Ns())
                .put("decision_age_p99_ns", metrics.decisionAgeP99Ns())
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
      AutonomousPairLifecycle.Snapshot autonomous = autonomousPair.snapshot();
      status.put(
          "autonomous_pair",
          new JSONObject()
              .put("state", autonomous.state().name().toLowerCase(java.util.Locale.ROOT))
              .put(
                  "active_shared_session_id",
                  autonomous.activeSessionId().isEmpty()
                      ? JSONObject.NULL
                      : autonomous.activeSessionId())
              .put("peer_available", autonomous.peerAvailable())
              .put("peer_armed", autonomous.peerArmed())
              .put("local_triggered", autonomous.localTriggered())
              .put("peer_triggered", autonomous.peerTriggered())
              .put("local_published", autonomous.localPublished())
              .put("peer_published", autonomous.peerPublished())
              .put("clock_fresh", autonomous.clockFresh())
              .put("recovered_from_checkpoint", autonomousRecoveredFromCheckpoint)
              .put("recovery_diagnostic", autonomousRecoveryDiagnostic)
              .put("replication_backlog_size", autonomousReplicationBacklogSize)
              .put(
                  "last_backlog_shared_session_id",
                  autonomousLastBacklogSessionId.isEmpty()
                      ? JSONObject.NULL
                      : autonomousLastBacklogSessionId)
              .put("last_backlog_outcome", autonomousLastBacklogOutcome)
              .put(
                  "last_outcome",
                  autonomous.lastOutcome().name().toLowerCase(java.util.Locale.ROOT))
              .put(
                  "last_completed_shared_session_id",
                  autonomous.lastCompletedSessionId().isEmpty()
                      ? JSONObject.NULL
                      : autonomous.lastCompletedSessionId())
              .put("diagnostic", autonomous.diagnostic()));
      PeerClockSynchronizer.Snapshot peerClock = latestPeerClockSnapshot(poseConfiguration);
      status.put(
          "peer_clock",
          peerClock == null
              ? JSONObject.NULL
              : new JSONObject()
                  .put("peer_node_id", peerClock.peerNodeId())
                  .put("peer_minus_local_ns", Long.toString(peerClock.peerMinusLocalNanos()))
                  .put("uncertainty_ns", Long.toString(peerClock.uncertaintyNanos()))
                  .put(
                      "measured_at_elapsed_realtime_ns",
                      Long.toString(peerClock.measuredAtLocalNanos()))
                  .put(
                      "age_ns",
                      Long.toString(
                          Math.max(
                              0,
                              SystemClock.elapsedRealtimeNanos()
                                  - peerClock.measuredAtLocalNanos())))
                  .put(
                      "minimum_round_trip_ns",
                      Long.toString(peerClock.minimumRoundTripNanos()))
                  .put(
                      "maximum_round_trip_ns",
                      Long.toString(peerClock.maximumRoundTripNanos()))
                  .put("sample_count", peerClock.sampleCount()));
      PeerImpactMappingEvidence peerImpact =
          lastPeerImpactMappingEvidence.current(COORDINATION.sharedSessionId());
      status.put(
          "peer_impact_mapping",
          peerImpact == null
              ? JSONObject.NULL
              : peerImpact.toJson());
      status.put(
          "high_speed_audio",
          new JSONObject()
              .put("ready", engine != null && captureReady)
              .put("maximum_peak_amplitude", audioPeakAmplitude)
              .put("noise_floor", audioNoiseFloor)
              .put("threshold", audioThreshold));
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

  @Override
  public NodeHttpServer.CaptureControl.SetupPreview setupPreview() {
    SetupPreviewPayload payload = setupPreviewSnapshot();
    SetupPreviewProvider.Snapshot preview = payload.snapshot();
    return new NodeHttpServer.CaptureControl.SetupPreview(
        preview.state().name().toLowerCase(java.util.Locale.ROOT),
        preview.reason().name().toLowerCase(java.util.Locale.ROOT),
        preview.generation(),
        preview.frameAgeNanos(),
        payload.imageRotationDegrees(),
        preview.jpeg());
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
      ContinuousCaptureEngine current = engine;
      if (current != null && current.activeEvidenceTimeoutAllowed()) {
        schedulePoseNoImpactTimeout();
      } else {
        schedulePoseThermalHardStopOnly();
      }
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
    if (captureReady) {
      audioPeakAmplitude = Math.max(audioPeakAmplitude, peakAmplitude);
      if (audioHilRequested && !soakHilRequested) {
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
    NodeCoordinationState.TriggerReport localTrigger = COORDINATION.latestTrigger();
    updateNotification("Impact detected; collecting post-roll");
    PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
    if (poseHighSpeedAttempt
        && poseConfiguration != null
        && poseConfiguration.mode() == PoseNodeMode.LEADER
        && localTrigger != null) {
      autonomousLocalTrigger = localTrigger;
      handleAutonomousTransition(
          autonomousPair.localTriggered(localTrigger.sharedSessionId(), sessionId));
    }
    if (poseHighSpeedAttempt
        && "local_audio".equals(source)
        && poseConfiguration != null
        && poseConfiguration.mode() == PoseNodeMode.LEADER
        && poseConfiguration.hasPeer()) {
      PosePeerArmClient.ImpactTrigger peerTrigger =
          buildPeerImpactTrigger(
              poseConfiguration,
              PosePeerArmClient.sharedSessionIdForLocalTrigger(
                  COORDINATION.latestTrigger(), sessionId),
              captureConfiguration.nodeId(),
              triggerTimestampNanos,
              timestampUncertaintyNanos);
      peerExecutor.execute(() -> requestPeerPoseImpact(poseConfiguration, peerTrigger));
    }
  }

  private PosePeerArmClient.ImpactTrigger buildPeerImpactTrigger(
      PoseStationConfigurationSnapshot poseConfiguration,
      String sessionId,
      String leaderNodeId,
      long triggerTimestampNanos,
      long triggerTimestampUncertaintyNanos) {
    PeerClockSynchronizer.Snapshot snapshot = latestPeerClockSnapshot(poseConfiguration);
    long now = SystemClock.elapsedRealtimeNanos();
    if (snapshot != null
        && snapshot.usableAt(
            now,
            PeerClockSynchronizer.MAXIMUM_SNAPSHOT_AGE_NANOS,
            PeerClockSynchronizer.MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS)) {
      try {
        long mappingUncertaintyNanos =
            Math.addExact(snapshot.uncertaintyNanos(), triggerTimestampUncertaintyNanos);
        if (mappingUncertaintyNanos
            > PeerClockSynchronizer.MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS) {
          return new PosePeerArmClient.ImpactTrigger(
              sessionId, leaderNodeId, triggerTimestampNanos);
        }
        return PosePeerArmClient.ImpactTrigger.mapped(
            sessionId,
            leaderNodeId,
            triggerTimestampNanos,
            snapshot.peerNodeId(),
            snapshot.localToPeerNanos(triggerTimestampNanos),
            mappingUncertaintyNanos,
            Math.subtractExact(now, snapshot.measuredAtLocalNanos()),
            snapshot.minimumRoundTripNanos(),
            snapshot.maximumRoundTripNanos(),
            snapshot.sampleCount());
      } catch (ArithmeticException | IllegalArgumentException invalidMapping) {
        Log.d(TAG, "Peer impact clock mapping overflowed; using schema 1 fallback");
      }
    }
    return new PosePeerArmClient.ImpactTrigger(sessionId, leaderNodeId, triggerTimestampNanos);
  }

  private record PeerImpactMappingEvidence(
      PosePeerArmClient.ImpactTrigger trigger,
      boolean mappingWithinPolicy,
      String selectedSource) {
    private JSONObject toJson() throws org.json.JSONException {
      return new JSONObject()
          .put("schema_version", trigger.schemaVersion())
          .put("shared_session_id", trigger.sharedSessionId())
          .put("leader_node_id", trigger.leaderNodeId())
          .put("target_peer_node_id", nullableText(trigger.targetPeerNodeId()))
          .put(
              "mapped_peer_trigger_elapsed_realtime_ns",
              trigger.hasClockMapping()
                  ? Long.toString(trigger.mappedPeerTriggerElapsedRealtimeNanos())
                  : JSONObject.NULL)
          .put(
              "mapping_uncertainty_ns",
              trigger.hasClockMapping()
                  ? Long.toString(trigger.mappingUncertaintyNanos())
                  : JSONObject.NULL)
          .put(
              "mapping_age_at_send_ns",
              trigger.hasClockMapping()
                  ? Long.toString(trigger.mappingAgeAtSendNanos())
                  : JSONObject.NULL)
          .put(
              "minimum_round_trip_ns",
              trigger.hasClockMapping()
                  ? Long.toString(trigger.minimumRoundTripNanos())
                  : JSONObject.NULL)
          .put(
              "maximum_round_trip_ns",
              trigger.hasClockMapping()
                  ? Long.toString(trigger.maximumRoundTripNanos())
                  : JSONObject.NULL)
          .put("sample_count", trigger.hasClockMapping() ? trigger.sampleCount() : JSONObject.NULL)
          .put("selected_source", selectedSource)
          .put("mapping_within_policy", mappingWithinPolicy)
          .put(
              "fallback_semantics",
              trigger.hasClockMapping() && mappingWithinPolicy
                  ? "mapped_candidate_else_arrival"
                  : "fresh_local_candidate_else_arrival");
    }

    private static Object nullableText(String value) {
      return value.isEmpty() ? JSONObject.NULL : value;
    }
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
    NodeCoordinationState.TriggerReport localTrigger = autonomousLocalTrigger;
    if (restartPoseStandby
        && localTrigger != null
        && localTrigger.localSessionId().equals(sessionId)) {
      handleAutonomousTransition(
          autonomousPair.localPublished(localTrigger.sharedSessionId(), sessionId));
    }
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
              setArmedInternal(true, null, true, false, false);
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

  private void startCapture(long captureArmRequestedElapsedRealtimeNanos) {
    try {
      markSetupPreviewSourceUnavailable(SetupPreviewProvider.Reason.HIGH_SPEED_CAPTURE);
      acquireWakeLock();
      CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
      ContinuousCaptureEngine created =
          new ContinuousCaptureEngine(
              this,
              captureConfiguration,
              COORDINATION.sharedSessionId(),
              audioHilRequested,
              !soakHilRequested,
              captureArmRequestedElapsedRealtimeNanos,
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
      PoseExperimentConfiguration experiment = poseExperimentConfiguration;
      if (experiment != null) {
        standbyConfig = standbyConfig.withExperiment(experiment);
      }
      lastPoseReadyStatus = null;
      PoseTriggerController triggerController =
          poseTriggerControllerLease.acquire(standbyConfig.controllerConfig());
      Object previewSource = new Object();
      PoseStandbyEngine created =
          PoseStandbyEngine.start(
              this,
              standbyConfig,
              triggerController,
              new PoseStandbyEngine.Listener() {
                @Override
                public void onReady(PoseStandbyEngine.ReadyStatus ready) {
                  lastPoseReadyStatus = ready;
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
                public void onSetupPreviewNv21(
                    long timestampBoottimeNanos,
                    int width,
                    int height,
                    int imageRotationDegrees,
                    byte[] nv21) {
                  offerSetupPreviewNv21(
                      previewSource,
                      timestampBoottimeNanos,
                      width,
                      height,
                      imageRotationDegrees,
                      nv21);
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
                    long highSpeedArmRequestedElapsedRealtimeNanos =
                        decision.armRequestedNs().orElse(evaluation.timestampNs());
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
                        armRequestedElapsedRealtimeNanos =
                            highSpeedArmRequestedElapsedRealtimeNanos;
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
                      handleAutonomousTransition(
                          autonomousPair.claimSwing(
                              sharedSessionId, evaluation.timestampNs()));
                      peerArmStatus.pending(sharedSessionId);
                      peerExecutor.execute(() -> requestPeerPoseArm(poseConfiguration, candidate));
                    } else {
                      peerArmStatus.reset();
                    }
                    controlExecutor.execute(
                        () ->
                            beginPoseHighSpeed(
                                sharedSessionId,
                                true,
                                decision,
                                null,
                                highSpeedArmRequestedElapsedRealtimeNanos));
                  }
                }

                @Override
                public void onFailure(Throwable failure) {
                  CaptureForegroundService.this.onFailure(failure);
                }
              });
      poseStandbyEngine = created;
      markSetupPreviewSourceAvailable(previewSource);
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

  private synchronized void markSetupPreviewSourceAvailable(Object source) {
    activeSetupPreviewSource = Objects.requireNonNull(source, "source");
    setupPreviewProvider.markSourceAvailable();
    setupPreviewEvidenceTimestampNanos = -1L;
    setupPreviewImageRotationDegrees = 0;
  }

  private synchronized void markSetupPreviewSourceUnavailable(SetupPreviewProvider.Reason reason) {
    activeSetupPreviewSource = null;
    SetupPreviewProvider.Snapshot current =
        setupPreviewProvider.snapshot(SystemClock.elapsedRealtimeNanos());
    if (current.reason() != reason) {
      setupPreviewProvider.markSourceUnavailable(reason);
    }
    setupPreviewEvidenceTimestampNanos = -1L;
  }

  private synchronized void offerSetupPreviewNv21(
      Object source,
      long timestampBoottimeNanos,
      int width,
      int height,
      int imageRotationDegrees,
      byte[] nv21) {
    if (source != activeSetupPreviewSource) {
      return;
    }
    CameraImageRotation.fromSensorOrientation(imageRotationDegrees);
    setupPreviewProvider.offerNv21(timestampBoottimeNanos, width, height, nv21);
    setupPreviewImageRotationDegrees = imageRotationDegrees;
  }

  /** Publishes only the newest already-encoded diagnostic frame; this never touches Camera2. */
  private synchronized SetupPreviewPayload setupPreviewSnapshot() {
    PoseStandbyEngine standby = poseStandbyEngine;
    if (standby != null) {
      PreviewEvidenceRing.Snapshot evidence = standby.previewEvidenceSnapshot();
      for (int index = evidence.entryCount() - 1; index >= 0; --index) {
        PreviewEvidence candidate = evidence.entryAt(index);
        if (candidate.hasCompressedFrame()
            && candidate.timestampBoottimeNanos() > setupPreviewEvidenceTimestampNanos) {
          setupPreviewProvider.offerEvidence(candidate);
          setupPreviewEvidenceTimestampNanos = candidate.timestampBoottimeNanos();
          setupPreviewImageRotationDegrees = candidate.imageRotationDegrees();
          break;
        }
      }
    }
    return new SetupPreviewPayload(
        setupPreviewProvider.snapshot(SystemClock.elapsedRealtimeNanos()),
        setupPreviewImageRotationDegrees);
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
    AutomaticTriggerReadinessGate.PeerArmResolution readinessResolution =
        AutomaticTriggerReadinessGate.PeerArmResolution.UNCONFIRMED;
    boolean resolveCaptureGate = true;
    try {
      PosePeerArmClient.Response response =
          new PosePeerArmClient(
                  poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
              .armUntilReady(
                  candidate,
                  PEER_ARM_MAXIMUM_ATTEMPTS,
                  PEER_ARM_READY_MAXIMUM_PENDING_RESPONSES,
                  PEER_ARM_RETRY_DELAY_MILLIS,
                  () -> poseArmAttemptActive(candidate.sharedSessionId()));
      boolean peerReady = response.accepted() && response.poseReady();
      if (peerReady) {
        readinessResolution = AutomaticTriggerReadinessGate.PeerArmResolution.READY;
        peerArmStatus.response(candidate.sharedSessionId(), true, response.statusCode());
        handleAutonomousTransition(autonomousPair.peerArmAccepted(candidate.sharedSessionId()));
      } else if (response.accepted() || response.statusCode() >= 500) {
        Log.e(
            TAG,
            "Peer did not confirm high-speed pre-roll readiness before the bounded poll ended"
                + " (HTTP "
                + response.statusCode()
                + ")");
        IllegalStateException readinessFailure =
            new IllegalStateException("peer high-speed pre-roll readiness was not confirmed");
        peerArmStatus.failed(candidate.sharedSessionId(), readinessFailure);
        handleAutonomousTransition(
            autonomousPair.peerArmUnconfirmed(
                candidate.sharedSessionId(), "HTTP " + response.statusCode()));
      } else {
        readinessResolution = AutomaticTriggerReadinessGate.PeerArmResolution.FAILED;
        Log.e(TAG, "Peer rejected pose arm with HTTP " + response.statusCode());
        peerArmStatus.response(candidate.sharedSessionId(), false, response.statusCode());
        handleAutonomousTransition(
            autonomousPair.peerArmFailed(
                candidate.sharedSessionId(), "HTTP " + response.statusCode()));
      }
    } catch (PosePeerArmClient.ArmPollingCancelledException cancelled) {
      resolveCaptureGate = false;
      Log.i(TAG, "Peer pose-arm readiness polling ended with its local arm lifecycle");
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      peerArmStatus.failed(candidate.sharedSessionId(), interrupted);
      handleAutonomousTransition(
          autonomousPair.peerArmUnconfirmed(candidate.sharedSessionId(), "interrupted"));
      Log.e(TAG, "Peer pose-arm delivery was interrupted", interrupted);
    } catch (Throwable failure) {
      if (!(failure instanceof IOException)) {
        readinessResolution = AutomaticTriggerReadinessGate.PeerArmResolution.FAILED;
      }
      peerArmStatus.failed(candidate.sharedSessionId(), failure);
      handleAutonomousTransition(
          failure instanceof IOException
              ? autonomousPair.peerArmUnconfirmed(
                  candidate.sharedSessionId(), failure.getClass().getSimpleName())
              : autonomousPair.peerArmFailed(
                  candidate.sharedSessionId(), failure.getClass().getSimpleName()));
      Log.e(TAG, "Unable to arm pose peer; local capture is continuing", failure);
    } finally {
      if (resolveCaptureGate) {
        AutomaticTriggerReadinessGate.PeerArmResolution completedResolution = readinessResolution;
        try {
          controlExecutor.execute(
              () -> resolvePeerArmForCapture(candidate.sharedSessionId(), completedResolution));
        } catch (RejectedExecutionException shuttingDown) {
          Log.d(TAG, "Peer arm resolution discarded during shutdown");
        }
      }
    }
  }

  private boolean poseArmAttemptActive(String sharedSessionId) {
    PeerArmStatusTracker.Snapshot snapshot = peerArmStatus.snapshot();
    return snapshot.state() == PeerArmStatusTracker.State.PENDING
        && snapshot.sharedSessionId().equals(sharedSessionId)
        && (poseTransitionRequested || poseHighSpeedAttempt);
  }

  private void resolvePeerArmForCapture(
      String sharedSessionId, AutomaticTriggerReadinessGate.PeerArmResolution resolution) {
    ContinuousCaptureEngine current = engine;
    if (current == null) {
      return;
    }
    current.resolvePeerArmForAutomaticTriggers(sharedSessionId, resolution);
    if (poseHighSpeedAttempt
        && captureReady
        && current.activeEvidenceTimeoutAllowed()
        && poseNoImpactTimeout == null) {
      schedulePoseActiveEvidenceTimeout();
    }
  }

  private void handleAutonomousTransition(AutonomousPairLifecycle.Transition transition) {
    try {
      persistAutonomousCheckpoint(transition);
    } catch (IOException persistenceFailure) {
      AutonomousPairLifecycle.Transition terminal =
          autonomousPair.durabilityFailed(persistenceFailure.getClass().getSimpleName());
      autonomousRecoveryDiagnostic =
          "checkpoint write failed: " + persistenceFailure.getClass().getSimpleName();
      try {
        persistAutonomousCheckpoint(terminal);
      } catch (IOException stillUnavailable) {
        Log.e(TAG, "Unable to persist terminal autonomous-pair state", stillUnavailable);
      }
      Log.e(TAG, "Autonomous-pair action withheld because its checkpoint was not durable", persistenceFailure);
      return;
    }
    Set<AutonomousPairLifecycle.Action> actions = transition.actions();
    try {
      if (actions.contains(AutonomousPairLifecycle.Action.START_PEER_STANDBY)) {
        autonomousPairExecutor.execute(this::startAutonomousPeerStandby);
      }
      if (actions.contains(AutonomousPairLifecycle.Action.STOP_PEER_STANDBY)) {
        autonomousPairExecutor.execute(this::stopAutonomousPeerStandby);
      }
      if (actions.contains(AutonomousPairLifecycle.Action.ADMIT_PAIR)) {
        autonomousPairExecutor.execute(this::admitAutonomousPair);
      }
      if (actions.contains(AutonomousPairLifecycle.Action.STORE_LOCAL_RECORD)) {
        autonomousPairExecutor.execute(this::storeAutonomousPairLocally);
      }
      if (actions.contains(AutonomousPairLifecycle.Action.REPLICATE_RECORD_TO_PEER)) {
        autonomousPairExecutor.execute(this::replicateAutonomousPairToPeer);
      }
      if (actions.contains(AutonomousPairLifecycle.Action.REARM_LOCAL)
          || actions.contains(AutonomousPairLifecycle.Action.REARM_PEER)) {
        autonomousPairExecutor.execute(this::completeAutonomousRearm);
      }
    } catch (RejectedExecutionException shuttingDown) {
      Log.d(TAG, "Autonomous pair action discarded during shutdown");
    }
  }

  private void persistAutonomousCheckpoint(AutonomousPairLifecycle.Transition transition)
      throws IOException {
    AutonomousPairDurableStore durableStore = autonomousPairDurableStore;
    if (durableStore == null) {
      throw new IOException("autonomous-pair durable store is unavailable");
    }
    AutonomousPairLifecycle.Snapshot snapshot = transition.snapshot();
    if (snapshot.state() == AutonomousPairLifecycle.State.STOPPED) {
      durableStore.clearCheckpoint();
      return;
    }
    PairedCoordinationRecord pending = autonomousPendingRecord;
    Optional<PairedCoordinationRecord> durablePending =
        pending != null && pending.sharedSessionId().equals(snapshot.activeSessionId())
            ? Optional.of(pending)
            : Optional.empty();
    durableStore.saveCheckpoint(transition.checkpoint(), durablePending);
  }

  private void startAutonomousPeerStandby() {
    PoseStationConfigurationSnapshot poseConfiguration = autonomousPeerConfiguration;
    if (poseConfiguration == null
        || poseConfiguration.mode() != PoseNodeMode.LEADER
        || !poseConfiguration.hasPeer()) {
      return;
    }
    try {
      AutonomousPairPeerClient.Response response =
          new AutonomousPairPeerClient(
                  poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
              .setStandbyWithRetries(
                  true,
                  AUTONOMOUS_PEER_MAXIMUM_ATTEMPTS,
                  AUTONOMOUS_PEER_RETRY_DELAY_MILLIS);
      handleAutonomousTransition(
          response.accepted()
              ? autonomousPair.peerStandbyStarted()
              : autonomousPair.peerUnavailable("station start HTTP " + response.statusCode()));
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      handleAutonomousTransition(autonomousPair.peerUnavailable("station start interrupted"));
    } catch (Exception failure) {
      handleAutonomousTransition(
          autonomousPair.peerUnavailable(
              "station start " + failure.getClass().getSimpleName()));
    }
  }

  private void stopAutonomousPeerStandby() {
    PoseStationConfigurationSnapshot poseConfiguration = autonomousPeerConfiguration;
    try {
      if (poseConfiguration != null && poseConfiguration.hasPeer()) {
        new AutonomousPairPeerClient(
                poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
            .setStandbyWithRetries(
                false,
                AUTONOMOUS_PEER_MAXIMUM_ATTEMPTS,
                AUTONOMOUS_PEER_RETRY_DELAY_MILLIS);
      }
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
    } catch (Exception failure) {
      Log.i(TAG, "Peer was unavailable while stopping autonomous station", failure);
    } finally {
      if (autonomousPair.snapshot().state() == AutonomousPairLifecycle.State.STOPPING) {
        handleAutonomousTransition(autonomousPair.stopped());
      }
      autonomousPeerConfiguration = null;
      clearAutonomousPairEvidence();
    }
  }

  private void maintainAutonomousPair() {
    try {
      PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
      if (poseConfiguration == null || poseConfiguration.mode() != PoseNodeMode.LEADER) {
        return;
      }
      AutonomousPairLifecycle.Snapshot snapshot = autonomousPair.snapshot();
      if (snapshot.state() == AutonomousPairLifecycle.State.MONITORING
          || snapshot.state() == AutonomousPairLifecycle.State.DEGRADED_MONITORING) {
        retryAutonomousReplicationBacklog(poseConfiguration);
        snapshot = autonomousPair.snapshot();
        if (snapshot.state() == AutonomousPairLifecycle.State.TERMINAL_FAILURE) {
          return;
        }
      }
      if (snapshot.state() == AutonomousPairLifecycle.State.DEGRADED_MONITORING) {
        handleAutonomousTransition(autonomousPair.retryPeer());
        return;
      }
      if (snapshot.localTriggered()
          && (snapshot.state() == AutonomousPairLifecycle.State.WAITING_EVIDENCE
              || snapshot.state() == AutonomousPairLifecycle.State.CAPTURING
              || snapshot.state() == AutonomousPairLifecycle.State.ARMING_SWING)) {
        pollAutonomousPeerEvidence(poseConfiguration, snapshot);
      }
      handleAutonomousTransition(autonomousPair.tick(SystemClock.elapsedRealtimeNanos()));
    } catch (Throwable failure) {
      Log.e(TAG, "Autonomous pair maintenance failed", failure);
    }
  }

  private void retryAutonomousReplicationBacklog(
      PoseStationConfigurationSnapshot poseConfiguration) {
    AutonomousPairDurableStore durableStore = autonomousPairDurableStore;
    if (durableStore == null || !poseConfiguration.hasPeer()) {
      return;
    }
    long now = SystemClock.elapsedRealtimeNanos();
    long previous = autonomousLastBacklogAttemptElapsedRealtimeNanos;
    if (previous != 0 && now - previous < AUTONOMOUS_BACKLOG_RETRY_INTERVAL_NANOS) {
      return;
    }
    autonomousLastBacklogAttemptElapsedRealtimeNanos = now;
    try {
      List<PairedCoordinationRecord> backlog = durableStore.backlog();
      autonomousReplicationBacklogSize = backlog.size();
      if (backlog.isEmpty()) {
        return;
      }
      PairedCoordinationRecord record = backlog.get(0);
      autonomousLastBacklogSessionId = record.sharedSessionId();
      AutonomousPairPeerClient.Response response =
          new AutonomousPairPeerClient(
                  poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
              .storeCoordinationRecord(record);
      if (response.statusCode() == 409) {
        autonomousLastBacklogOutcome = "conflict";
        handleAutonomousTransition(
            autonomousPair.backlogReplicationConflict(record.sharedSessionId()));
        return;
      }
      if (!response.accepted()) {
        autonomousLastBacklogOutcome = "http_" + response.statusCode();
        return;
      }
      durableStore.markReplicated(record);
      autonomousReplicationBacklogSize = durableStore.backlogSize();
      autonomousLastBacklogOutcome = "replicated";
    } catch (IOException failure) {
      autonomousLastBacklogOutcome = "unavailable_" + failure.getClass().getSimpleName();
      Log.i(TAG, "Autonomous-pair replication backlog remains pending", failure);
    }
  }

  private void pollAutonomousPeerEvidence(
      PoseStationConfigurationSnapshot poseConfiguration,
      AutonomousPairLifecycle.Snapshot snapshot)
      throws Exception {
    AutonomousPairPeerClient client =
        new AutonomousPairPeerClient(
            poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken());
    NodeCoordinationState.TriggerReport peerTrigger = autonomousPeerTrigger;
    if (peerTrigger == null) {
      AutonomousPairPeerClient.Response response = client.triggerReport();
      if (response.statusCode() == 404) {
        return;
      }
      if (!response.accepted()) {
        handleAutonomousTransition(
            autonomousPair.peerUnavailable("trigger report HTTP " + response.statusCode()));
        return;
      }
      peerTrigger = parseAutonomousPeerTrigger(response.body());
      PeerClockSynchronizer.Snapshot clock = latestPeerClockSnapshot(poseConfiguration);
      long now = SystemClock.elapsedRealtimeNanos();
      boolean freshClock =
          clock != null
              && clock.peerNodeId().equals(peerTrigger.nodeId())
              && clock.usableAt(
                  now,
                  PeerClockSynchronizer.MAXIMUM_SNAPSHOT_AGE_NANOS,
                  PeerClockSynchronizer.MAXIMUM_SNAPSHOT_UNCERTAINTY_NANOS);
      autonomousPeerTrigger = peerTrigger;
      autonomousPeerClock = freshClock ? clock : null;
      handleAutonomousTransition(
          autonomousPair.peerTriggered(
              peerTrigger.sharedSessionId(), peerTrigger.localSessionId(), freshClock));
      if (autonomousPair.snapshot().state() == AutonomousPairLifecycle.State.TERMINAL_FAILURE) {
        return;
      }
    }
    if (!snapshot.peerPublished()) {
      AutonomousPairPeerClient.PublicationProbe publication =
          client.publishedCapture(peerTrigger.localSessionId(), peerTrigger.role());
      if (publication.published()) {
        handleAutonomousTransition(
            autonomousPair.peerPublished(
                peerTrigger.sharedSessionId(), peerTrigger.localSessionId()));
      } else if (publication.statusCode() != 404) {
        handleAutonomousTransition(
            autonomousPair.peerUnavailable(
                "peer publication "
                    + publication.stage()
                    + " HTTP "
                    + publication.statusCode()));
      }
    }
  }

  private static NodeCoordinationState.TriggerReport parseAutonomousPeerTrigger(String json)
      throws Exception {
    JSONObject body = new JSONObject(json);
    Set<String> fields =
        Set.of(
            "schema_version",
            "role",
            "node_id",
            "shared_session_id",
            "local_session_id",
            "trigger_elapsed_realtime_ns",
            "timestamp_uncertainty_ns",
            "source");
    if (body.length() != fields.size() || body.getInt("schema_version") != 1) {
      throw new IllegalArgumentException("peer trigger report fields do not match schema 1");
    }
    for (String field : fields) {
      if (!body.has(field) || body.isNull(field)) {
        throw new IllegalArgumentException("peer trigger report is missing " + field);
      }
    }
    String timestamp = body.getString("trigger_elapsed_realtime_ns");
    long parsedTimestamp = Long.parseLong(timestamp);
    if (!Long.toString(parsedTimestamp).equals(timestamp)) {
      throw new IllegalArgumentException("peer trigger timestamp is not canonical decimal");
    }
    Object uncertaintyValue = body.get("timestamp_uncertainty_ns");
    if (!(uncertaintyValue instanceof Integer) && !(uncertaintyValue instanceof Long)) {
      throw new IllegalArgumentException("peer trigger uncertainty must be an integer");
    }
    return new NodeCoordinationState.TriggerReport(
        body.getString("role"),
        body.getString("node_id"),
        body.getString("shared_session_id"),
        body.getString("local_session_id"),
        parsedTimestamp,
        ((Number) uncertaintyValue).longValue(),
        body.getString("source"));
  }

  private void admitAutonomousPair() {
    String sharedSessionId = autonomousPair.snapshot().activeSessionId();
    try {
      NodeCoordinationState.TriggerReport local =
          Objects.requireNonNull(autonomousLocalTrigger, "local trigger");
      NodeCoordinationState.TriggerReport peer =
          Objects.requireNonNull(autonomousPeerTrigger, "peer trigger");
      PeerClockSynchronizer.Snapshot peerClock =
          Objects.requireNonNull(autonomousPeerClock, "fresh peer clock");
      if (!sharedSessionId.equals(local.sharedSessionId())
          || !sharedSessionId.equals(peer.sharedSessionId())) {
        throw new IllegalArgumentException("trigger reports contain split shared sessions");
      }

      PairedCoordinationRecord.NodeEvidence localEvidence =
          durableNodeEvidence(
              sharedSessionId,
              local,
              new ClockOffsetEstimate(local.nodeId(), 0, 0, 0, 0, 1));
      PairedCoordinationRecord.NodeEvidence peerEvidence =
          durableNodeEvidence(
              sharedSessionId,
              peer,
              new ClockOffsetEstimate(
                  peer.nodeId(),
                  peerClock.peerMinusLocalNanos(),
                  peerClock.uncertaintyNanos(),
                  peerClock.minimumRoundTripNanos(),
                  peerClock.maximumRoundTripNanos(),
                  peerClock.sampleCount()));
      if (localEvidence.mappedCoordinatorUncertaintyNs() > 10_000_000L
          || peerEvidence.mappedCoordinatorUncertaintyNs() > 10_000_000L
          || Math.addExact(
                  localEvidence.mappedCoordinatorUncertaintyNs(),
                  peerEvidence.mappedCoordinatorUncertaintyNs())
              > 20_000_000L) {
        throw new IllegalArgumentException("paired trigger uncertainty exceeds policy");
      }
      PairedCoordinationRecord record =
          localEvidence.role()
                  == com.agoessling.swingcapture.core.coordination.CaptureRole.DOWN_THE_LINE
              ? PairedCoordinationRecord.create(
                  sharedSessionId, System.currentTimeMillis(), localEvidence, peerEvidence)
              : PairedCoordinationRecord.create(
                  sharedSessionId, System.currentTimeMillis(), peerEvidence, localEvidence);
      if (record.maximumTriggerSeparationNs() > 50_000_000L) {
        throw new IllegalArgumentException("dual triggers cannot prove a match within 50 ms");
      }
      autonomousPendingRecord = record;
      handleAutonomousTransition(autonomousPair.pairAdmitted(sharedSessionId));
    } catch (Throwable failure) {
      Log.e(TAG, "Autonomous pair admission failed", failure);
      handleAutonomousTransition(
          autonomousPair.pairAdmissionFailed(
              sharedSessionId, failure.getClass().getSimpleName()));
    }
  }

  private static PairedCoordinationRecord.NodeEvidence durableNodeEvidence(
      String sharedSessionId,
      NodeCoordinationState.TriggerReport report,
      ClockOffsetEstimate clock) {
    com.agoessling.swingcapture.core.coordination.NodeTriggerReport coreReport =
        new com.agoessling.swingcapture.core.coordination.NodeTriggerReport(
            com.agoessling.swingcapture.core.coordination.CaptureRole.parse(report.role()),
            report.nodeId(),
            report.sharedSessionId(),
            report.triggerElapsedRealtimeNanos(),
            report.timestampUncertaintyNanos());
    return PairedCoordinationRecord.NodeEvidence.map(
        sharedSessionId, coreReport, report.localSessionId(), report.source(), clock);
  }

  private void storeAutonomousPairLocally() {
    PairedCoordinationRecord record = autonomousPendingRecord;
    CoordinationRecordStore store = coordinationRecords;
    AutonomousPairDurableStore durableStore = autonomousPairDurableStore;
    if (record == null || store == null || durableStore == null) {
      return;
    }
    AutonomousPairLifecycle.ImmutableStoreOutcome outcome;
    try {
      CoordinationRecordStore.StoreStatus status = store.storeIfAbsent(record).status();
      outcome =
          switch (status) {
            case STORED -> AutonomousPairLifecycle.ImmutableStoreOutcome.STORED;
            case ALREADY_PRESENT -> AutonomousPairLifecycle.ImmutableStoreOutcome.ALREADY_PRESENT;
            case CONFLICT -> AutonomousPairLifecycle.ImmutableStoreOutcome.CONFLICT;
          };
      if (outcome != AutonomousPairLifecycle.ImmutableStoreOutcome.CONFLICT) {
        AutonomousPairDurableStore.EnqueueStatus enqueue = durableStore.enqueue(record);
        if (enqueue == AutonomousPairDurableStore.EnqueueStatus.CONFLICT) {
          outcome = AutonomousPairLifecycle.ImmutableStoreOutcome.CONFLICT;
          autonomousLastBacklogOutcome = "local_conflict";
        } else {
          autonomousReplicationBacklogSize = durableStore.backlogSize();
          autonomousLastBacklogSessionId = record.sharedSessionId();
          autonomousLastBacklogOutcome = "queued";
        }
      }
    } catch (IOException failure) {
      autonomousLastBacklogOutcome = "durability_unavailable";
      handleAutonomousTransition(
          autonomousPair.durabilityFailed(failure.getClass().getSimpleName()));
      return;
    }
    handleAutonomousTransition(
        autonomousPair.localRecordStored(record.sharedSessionId(), outcome));
  }

  private void replicateAutonomousPairToPeer() {
    PairedCoordinationRecord record = autonomousPendingRecord;
    PoseStationConfigurationSnapshot poseConfiguration = autonomousPeerConfiguration;
    AutonomousPairDurableStore durableStore = autonomousPairDurableStore;
    if (record == null || poseConfiguration == null || durableStore == null) {
      return;
    }
    AutonomousPairLifecycle.ImmutableStoreOutcome outcome;
    try {
      AutonomousPairPeerClient.Response response =
          new AutonomousPairPeerClient(
                  poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
              .storeCoordinationRecord(record);
      outcome =
          response.statusCode() == 409
              ? AutonomousPairLifecycle.ImmutableStoreOutcome.CONFLICT
              : (response.accepted()
                  ? AutonomousPairLifecycle.ImmutableStoreOutcome.STORED
                  : AutonomousPairLifecycle.ImmutableStoreOutcome.UNAVAILABLE);
      if (outcome == AutonomousPairLifecycle.ImmutableStoreOutcome.STORED) {
        durableStore.markReplicated(record);
        autonomousReplicationBacklogSize = durableStore.backlogSize();
        autonomousLastBacklogSessionId = record.sharedSessionId();
        autonomousLastBacklogOutcome = "replicated";
      } else if (outcome == AutonomousPairLifecycle.ImmutableStoreOutcome.CONFLICT) {
        autonomousLastBacklogSessionId = record.sharedSessionId();
        autonomousLastBacklogOutcome = "conflict";
      } else {
        autonomousLastBacklogOutcome = "pending_http_" + response.statusCode();
      }
    } catch (IOException failure) {
      outcome = AutonomousPairLifecycle.ImmutableStoreOutcome.UNAVAILABLE;
      autonomousLastBacklogOutcome = "pending_" + failure.getClass().getSimpleName();
    }
    handleAutonomousTransition(
        autonomousPair.peerRecordStored(record.sharedSessionId(), outcome));
  }

  private void completeAutonomousRearm() {
    if (autonomousPair.snapshot().state() != AutonomousPairLifecycle.State.REARMING) {
      return;
    }
    boolean peerReady = autonomousPair.snapshot().peerAvailable();
    handleAutonomousTransition(autonomousPair.rearmed(peerReady));
    clearAutonomousPairEvidence();
  }

  private void clearAutonomousPairEvidence() {
    autonomousLocalTrigger = null;
    autonomousPeerTrigger = null;
    autonomousPeerClock = null;
    autonomousPendingRecord = null;
  }

  @Override
  public PairNetworkHealthPolicy.DirectionEvidence measurePairNetworkDirection(
      String callbackOrigin, String callbackNodeId) {
    return new PairNetworkHealthClient(
            SystemClock::elapsedRealtimeNanos, PeerClockJsonResponseParser::parse)
        .measureDirection(callbackOrigin, callbackNodeId);
  }

  @Override
  public PairNetworkHealthPolicy.Snapshot pairNetworkHealthStatus() {
    reconcilePairNetworkHealthTarget(configuredPairNetworkHealthTarget(), true);
    return pairNetworkHealth.snapshot(SystemClock.elapsedRealtimeNanos());
  }

  private void pollPairNetworkHealth() {
    PairNetworkHealthTarget target = configuredPairNetworkHealthTarget();
    if (target == null) {
      reconcilePairNetworkHealthTarget(null, false);
      return;
    }
    PairNetworkHealthPolicy.PeerTarget peer =
        new PairNetworkHealthPolicy.PeerTarget(target.origin(), target.peerNodeId());
    reconcilePairNetworkHealthTarget(target, false);

    PairNetworkHealthClient client =
        new PairNetworkHealthClient(
            SystemClock::elapsedRealtimeNanos, PeerClockJsonResponseParser::parse);
    PairNetworkHealthPolicy.DirectionEvidence localToPeer =
        client.measureDirection(target.origin(), target.peerNodeId());
    PairNetworkHealthPolicy.DirectionEvidence peerToLocal = unreachableNetworkDirection();
    try {
      String callbackOrigin =
          PairNetworkHealthClient.callbackOrigin(target.origin(), NodeHttpServer.DEFAULT_PORT);
      peerToLocal =
          client.requestReverseDirection(
              target.origin(),
              target.peerControlToken(),
              PairNetworkHealthJson.reverseRequest(callbackOrigin, target.localNodeId()),
              PairNetworkHealthJson::parseDirection);
    } catch (IOException | IllegalArgumentException reverseUnavailable) {
      Log.d(
          TAG,
          "Reverse pair network-health probe unavailable: "
              + reverseUnavailable.getClass().getSimpleName()
              + ": "
              + String.valueOf(reverseUnavailable.getMessage()));
    }

    PairNetworkHealthTarget current = configuredPairNetworkHealthTarget();
    if (!target.equals(current)) {
      reconcilePairNetworkHealthTarget(current, true);
      return;
    }
    pairNetworkHealth.record(
        peer,
        new PairNetworkHealthPolicy.Round(
            SystemClock.elapsedRealtimeNanos(), localToPeer, peerToLocal));
  }

  private void reconcilePairNetworkHealthTarget(
      PairNetworkHealthTarget target, boolean requestImmediateSample) {
    synchronized (pairNetworkHealthTargetMonitor) {
      if (Objects.equals(target, reconciledPairNetworkHealthTarget)) {
        return;
      }
      reconciledPairNetworkHealthTarget = target;
      pairNetworkHealth.clearPeer();
      if (target != null) {
        pairNetworkHealth.configurePeer(
            new PairNetworkHealthPolicy.PeerTarget(target.origin(), target.peerNodeId()));
      }
    }
    if (target != null && requestImmediateSample) {
      pairNetworkHealthImmediatePoll.request();
    }
  }

  private void invalidatePairNetworkHealthTargetAfterFailure() {
    synchronized (pairNetworkHealthTargetMonitor) {
      reconciledPairNetworkHealthTarget = null;
      pairNetworkHealth.clearPeer();
    }
  }

  private PairNetworkHealthTarget configuredPairNetworkHealthTarget() {
    NodeConfiguration.StationConfiguration station = configuration.stationConfiguration();
    PoseStationConfigurationSnapshot pose = station.pose();
    if (pose.mode() != PoseNodeMode.LEADER || !pose.hasPeer()) {
      return null;
    }
    PeerPairingBinding binding = station.pairing().orElse(null);
    if (binding == null
        || binding.state() != PeerPairingBinding.State.ACTIVE
        || !binding.origin().equals(pose.peerOrigin())) {
      return null;
    }
    return new PairNetworkHealthTarget(
        binding.origin(),
        binding.peerNodeId(),
        pose.peerControlToken(),
        station.capture().nodeId());
  }

  private static PairNetworkHealthPolicy.DirectionEvidence unreachableNetworkDirection() {
    return new PairNetworkHealthPolicy.DirectionEvidence(
        PairNetworkHealthClient.CLOCK_ATTEMPTS,
        0,
        0,
        List.of(),
        0,
        0,
        false);
  }

  private record PairNetworkHealthTarget(
      String origin, String peerNodeId, String peerControlToken, String localNodeId) {}

  private void pollPeerClock() {
    try {
      PeerClockTarget target = configuredPeerClockTarget();
      if (target == null) {
        synchronized (peerClockMonitor) {
          clearPeerClockStateLocked();
        }
        return;
      }
      PeerClockClient.Exchange exchange =
          new PeerClockClient(
                  target.origin(),
                  target.peerNodeId(),
                  SystemClock::elapsedRealtimeNanos,
                  PeerClockJsonResponseParser::parse)
              .exchange();
      consecutivePeerClockFailures = 0;
      synchronized (peerClockMonitor) {
        PeerClockTarget current = configuredPeerClockTarget();
        if (!target.equals(current)) {
          clearPeerClockStateLocked();
          return;
        }
        if (!target.origin().equals(peerClockOrigin)
            || peerClockSynchronizer == null
            || !target.peerNodeId().equals(peerClockSynchronizer.peerNodeId())) {
          peerClockOrigin = target.origin();
          peerClockSynchronizer =
              new PeerClockSynchronizer(target.origin(), target.peerNodeId());
        }
        peerClockSynchronizer.record(exchange.sample());
      }
    } catch (Exception failure) {
      // Clock evidence is optional. The terminal path retains its local/arrival fallback and must
      // not be affected by a peer reboot, Wi-Fi outage, malformed response, or poller failure.
      long now = SystemClock.elapsedRealtimeNanos();
      ++consecutivePeerClockFailures;
      if (consecutivePeerClockFailures >= 3) {
        PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
        if (poseConfiguration != null && poseConfiguration.mode() == PoseNodeMode.LEADER) {
          handleAutonomousTransition(
              autonomousPair.peerUnavailable("three consecutive clock exchanges failed"));
        }
      }
      if (lastPeerClockFailureLogElapsedRealtimeNanos == 0
          || now - lastPeerClockFailureLogElapsedRealtimeNanos
              >= PEER_CLOCK_FAILURE_LOG_INTERVAL_NANOS) {
        lastPeerClockFailureLogElapsedRealtimeNanos = now;
        Log.d(
            TAG,
            "Peer clock exchange unavailable: "
                + failure.getClass().getSimpleName()
                + ": "
                + String.valueOf(failure.getMessage()));
      }
    }
  }

  private record PeerClockTarget(String origin, String peerNodeId) {}

  /** Uses only the authenticated durable pairing as identity for the public clock hint. */
  private PeerClockTarget configuredPeerClockTarget() {
    NodeConfiguration.StationConfiguration station = configuration.stationConfiguration();
    PoseStationConfigurationSnapshot poseConfiguration = activePoseConfiguration;
    if (poseConfiguration == null) {
      poseConfiguration = station.pose();
    }
    if (!poseConfiguration.hasPeer()) {
      return null;
    }
    PeerPairingBinding binding = station.pairing().orElse(null);
    if (binding == null
        || binding.state() != PeerPairingBinding.State.ACTIVE
        || !binding.origin().equals(poseConfiguration.peerOrigin())) {
      return null;
    }
    return new PeerClockTarget(binding.origin(), binding.peerNodeId());
  }

  private PeerClockSynchronizer.Snapshot latestPeerClockSnapshot(
      PoseStationConfigurationSnapshot poseConfiguration) {
    synchronized (peerClockMonitor) {
      if (!poseConfiguration.hasPeer()
          || !poseConfiguration.peerOrigin().equals(peerClockOrigin)
          || peerClockSynchronizer == null) {
        clearPeerClockStateLocked();
        return null;
      }
      PeerClockSynchronizer.Snapshot snapshot = peerClockSynchronizer.snapshot().orElse(null);
      if (snapshot == null
          || !snapshot.belongsTo(
              poseConfiguration.peerOrigin(), peerClockSynchronizer.peerNodeId())) {
        clearPeerClockStateLocked();
        return null;
      }
      return snapshot;
    }
  }

  /** Called only while {@link #peerClockMonitor} is held. */
  private void clearPeerClockStateLocked() {
    peerClockOrigin = "";
    peerClockSynchronizer = null;
  }

  private void requestPeerPoseImpact(
      PoseStationConfigurationSnapshot poseConfiguration,
      PosePeerArmClient.ImpactTrigger trigger) {
    try {
      PosePeerArmClient.Response response =
          new PosePeerArmClient(
                  poseConfiguration.peerOrigin(), poseConfiguration.peerControlToken())
              .triggerImpactWithRetriesAndMappingFallback(
                  trigger,
                  PEER_IMPACT_MAXIMUM_ATTEMPTS,
                  PEER_IMPACT_RETRY_DELAY_MILLIS);
      if (!response.accepted()) {
        Log.e(TAG, "Peer rejected pose impact with HTTP " + response.statusCode());
      }
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      Log.e(TAG, "Peer impact delivery was interrupted", interrupted);
    } catch (Throwable failure) {
      Log.e(TAG, "Unable to trigger pose peer impact; leader capture is continuing", failure);
    }
  }

  private void beginPoseHighSpeed(
      String sharedSessionId,
      boolean leaderInitiated,
      PoseTriggerController.Decision decision,
      PoseExternalArmLifecycle externalArmLifecycle,
      long highSpeedArmRequestedElapsedRealtimeNanos) {
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
      markSetupPreviewSourceUnavailable(SetupPreviewProvider.Reason.HIGH_SPEED_CAPTURE);
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
      lastPeerImpactMappingEvidence.clear();
      COORDINATION.armed(sharedSessionId);
      RUNTIME.transitioningToHighSpeed();
      if (currentAudio != null) {
        closeStandbyAudioRecorder();
      }
      CaptureConfigurationSnapshot captureConfiguration = requireActiveCaptureConfiguration();
      poseHighSpeedAttempt = true;
      ContinuousCaptureEngine created =
          new ContinuousCaptureEngine(
              this,
              captureConfiguration,
              sharedSessionId,
              poseArmHilEnabled,
              leaderInitiated,
              warmCamera,
              previewSnapshot,
              peerArmStatus,
              leaderInitiated && requireActivePoseConfiguration().hasPeer(),
              highSpeedArmRequestedElapsedRealtimeNanos,
              this);
      engine = created;
      PeerArmStatusTracker.Snapshot peerArmSnapshot = peerArmStatus.snapshot();
      if (peerArmSnapshot.sharedSessionId().equals(sharedSessionId)
          && peerArmSnapshot.state() != PeerArmStatusTracker.State.PENDING) {
        AutomaticTriggerReadinessGate.PeerArmResolution completedResolution =
            switch (peerArmSnapshot.state()) {
              case ACCEPTED -> AutomaticTriggerReadinessGate.PeerArmResolution.READY;
              case REJECTED -> AutomaticTriggerReadinessGate.PeerArmResolution.FAILED;
              case FAILED -> AutomaticTriggerReadinessGate.PeerArmResolution.UNCONFIRMED;
              case NOT_REQUESTED, PENDING, INBOUND_ACCEPTED -> null;
            };
        if (completedResolution != null) {
          created.resolvePeerArmForAutomaticTriggers(sharedSessionId, completedResolution);
        }
      }
      warmCamera = null;
      created.start();
    } catch (Throwable failure) {
      if (warmCamera != null) {
        warmCamera.close();
      }
      if (!leaderInitiated) {
        peerArmStatus.inboundFailed(sharedSessionId, failure);
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
    schedulePoseActiveEvidenceTimeout();
    schedulePoseThermalHardStopTimeout();
  }

  private void schedulePoseThermalHardStopOnly() {
    cancelPoseNoImpactTimeout();
    schedulePoseThermalHardStopTimeout();
  }

  private void schedulePoseActiveEvidenceTimeout() {
    cancelPoseActiveEvidenceTimeout();
    PoseHighSpeedNoImpactDeadline.Deadline deadline = poseNoImpactDeadline;
    if (deadline == null) {
      onFailure(new IllegalStateException("Pose no-impact deadline is unavailable"));
      return;
    }
    long delayNanos = deadline.delayFrom(SystemClock.elapsedRealtimeNanos());
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
  }

  private void schedulePoseThermalHardStopTimeout() {
    cancelPoseThermalHardStopTimeout();
    PoseHighSpeedNoImpactDeadline.Deadline deadline = poseNoImpactDeadline;
    if (deadline == null) {
      onFailure(new IllegalStateException("Pose no-impact deadline is unavailable"));
      return;
    }
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
            deadline.thermalHardCapDelayFrom(SystemClock.elapsedRealtimeNanos()),
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
    cancelPoseThermalHardStopTimeout();
  }

  private void cancelPoseThermalHardStopTimeout() {
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
    markSetupPreviewSourceUnavailable(SetupPreviewProvider.Reason.SOURCE_PAUSED);
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

  private DeviceCapabilityPolicy.HardwareSnapshot currentDeviceCapabilities() {
    return deviceCapabilities.withPermissions(
        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED,
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED);
  }

  private void startServer() {
    try {
      CoordinationRecordStore createdCoordinationRecords =
          new CoordinationRecordStore(
              new CoordinationFileTextStore(
                  new File(getFilesDir(), "coordination"), AndroidDirectorySync::synchronize));
      coordinationRecords = createdCoordinationRecords;
      server =
          new NodeHttpServer(
              this,
              configuration,
              RUNTIME,
              COORDINATION,
              createdCoordinationRecords,
              this,
              deviceCapabilities,
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
      CaptureStartupTimingManifest startupTiming =
          current.startupTimingManifest().orElse(null);
      if (startupTiming != null) {
        report.put(
            CaptureStartupTimingManifest.MANIFEST_FIELD_NAME,
            new JSONObject(startupTiming.toCanonicalJson()));
      }
    }
    return report;
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
