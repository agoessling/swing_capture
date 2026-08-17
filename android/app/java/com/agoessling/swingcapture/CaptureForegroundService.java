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
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;

/** Owns the camera/microphone capture loop and node API beyond the activity lifecycle. */
public final class CaptureForegroundService extends Service
    implements NodeHttpServer.CaptureControl, ContinuousCaptureEngine.Listener {
  public static final String ACTION_START = "com.agoessling.swingcapture.action.START";
  public static final String ACTION_ARM = "com.agoessling.swingcapture.action.ARM";
  public static final String ACTION_DISARM = "com.agoessling.swingcapture.action.DISARM";
  public static final String ACTION_TRIGGER = "com.agoessling.swingcapture.action.TRIGGER";
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
  private NodeConfiguration configuration;
  private NodeHttpServer server;
  private volatile ContinuousCaptureEngine engine;
  private volatile CaptureConfigurationSnapshot activeCaptureConfiguration;
  private PowerManager.WakeLock wakeLock;
  private volatile boolean continuousHilRequested;
  private volatile boolean audioHilRequested;
  private volatile boolean soakHilRequested;
  private volatile boolean soakCompletionRequested;
  private volatile int soakIncidentalTriggerCount;
  private volatile String soakLastIncidentalSessionId = "";
  private volatile long lastSoakTelemetryElapsedRealtimeNanos;
  private volatile long armRequestedElapsedRealtimeNanos;
  private volatile boolean captureReady;
  private volatile float audioPeakAmplitude;
  private volatile float audioNoiseFloor;
  private volatile float audioThreshold;

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
    startServer();
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    String action = intent == null ? ACTION_START : intent.getAction();
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
    controlExecutor.shutdownNow();
    releaseWakeLock();
    RUNTIME.stopped();
    super.onDestroy();
  }

  @Override
  public void setArmed(boolean armed, String sharedSessionId) {
    if (!armed) {
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
      COORDINATION.armed(sharedSessionId);
      RUNTIME.starting();
      activeCaptureConfiguration = requestedConfiguration;
      armRequestedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
    }
    updateNotification("Starting camera, microphone, and encoded pre-roll…");
    controlExecutor.execute(this::startCapture);
  }

  @Override
  public String triggerManual() {
    return triggerOperatorCapture(false);
  }

  @Override
  public String triggerMissedShot() {
    return triggerOperatorCapture(true);
  }

  private String triggerOperatorCapture(boolean missedShot) {
    ContinuousCaptureEngine current = engine;
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
    return sessionId;
  }

  @Override
  public void onReady() {
    audioPeakAmplitude = 0.0f;
    RUNTIME.armed();
    updateNotification("Armed: waiting for a local audio impact");
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
    boolean operatorCapture = !continuousHilRequested;
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

  private void stopCapture() {
    captureReady = false;
    stopEngineOnly();
    RUNTIME.stopped();
    releaseWakeLock();
    updateNotification("Node service running; capture is not armed");
  }

  private void stopEngineOnly() {
    captureReady = false;
    ContinuousCaptureEngine current = engine;
    if (current != null) {
      current.stop();
    }
    synchronized (this) {
      if (engine == current) {
        engine = null;
        activeCaptureConfiguration = null;
      }
    }
    releaseWakeLock();
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
