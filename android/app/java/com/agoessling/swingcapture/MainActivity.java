package com.agoessling.swingcapture;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import com.agoessling.swingcapture.node.CaptureRuntime;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;

/** Setup and capability-probe activity for one Android capture node. */
public final class MainActivity extends Activity {
  private static final String TAG = "SwingCapture";
  private static final int PERMISSION_REQUEST = 1001;

  private final ExecutorService worker = Executors.newSingleThreadExecutor();
  private final Runnable serviceDisplayRefresh = this::refreshServiceDisplay;
  private NodeConfiguration configuration;
  private Spinner roleSpinner;
  private Spinner profileSpinner;
  private TextView status;
  private TextView networkStatus;
  private Button refreshButton;
  private Button probeButton;
  private Button retainedCaptureButton;
  private Button warmTransitionButton;
  private Button armButton;
  private Button disarmButton;
  private Button manualTriggerButton;
  private Button saveConfigurationButton;
  private boolean configurationEditsLocallyEnabled = true;
  private boolean initialProbeRequested;
  private boolean initialRetainedCaptureRequested;
  private boolean initialWarmTransitionRequested;
  private boolean initialContinuousHilRequested;
  private boolean initialAudioHilRequested;
  private boolean initialSoakHilRequested;
  private boolean initialSoakFinishRequested;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    configuration = new NodeConfiguration(this);
    applyIntentConfiguration(getIntent());
    initialProbeRequested = getIntent().getBooleanExtra("run_probe", false);
    initialRetainedCaptureRequested = getIntent().getBooleanExtra("retain_clip", false);
    initialWarmTransitionRequested =
        getIntent().getBooleanExtra("run_warm_transition_hil", false);
    initialContinuousHilRequested = getIntent().getBooleanExtra("run_continuous_hil", false);
    initialAudioHilRequested = getIntent().getBooleanExtra("run_audio_hil", false);
    initialSoakHilRequested = getIntent().getBooleanExtra("run_audio_soak_hil", false);
    initialSoakFinishRequested = getIntent().getBooleanExtra("finish_audio_soak_hil", false);
    setContentView(createContentView());
    if (!requestRuntimePermissions()) {
      runInitialAction();
    }
  }

  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    applyIntentConfiguration(intent);
    initialProbeRequested = intent.getBooleanExtra("run_probe", false);
    initialRetainedCaptureRequested = intent.getBooleanExtra("retain_clip", false);
    initialWarmTransitionRequested = intent.getBooleanExtra("run_warm_transition_hil", false);
    initialContinuousHilRequested = intent.getBooleanExtra("run_continuous_hil", false);
    initialAudioHilRequested = intent.getBooleanExtra("run_audio_hil", false);
    initialSoakHilRequested = intent.getBooleanExtra("run_audio_soak_hil", false);
    initialSoakFinishRequested = intent.getBooleanExtra("finish_audio_soak_hil", false);
    selectConfiguredRole();
    selectConfiguredProfile();
    if (!requestRuntimePermissions()) {
      runInitialAction();
    }
  }

  @Override
  protected void onDestroy() {
    if (networkStatus != null) {
      networkStatus.removeCallbacks(serviceDisplayRefresh);
    }
    worker.shutdownNow();
    super.onDestroy();
  }

  @Override
  public void onRequestPermissionsResult(
      int requestCode, String[] permissions, int[] grantResults) {
    super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    if (requestCode == PERMISSION_REQUEST) {
      runInitialAction();
    }
  }

  private void applyIntentConfiguration(Intent intent) {
    if (intent == null) {
      return;
    }
    CaptureRole currentRole = configuration.role();
    CaptureProfile currentProfile = configuration.captureProfile();
    CaptureRole requestedRole =
        intent.hasExtra("role")
            ? CaptureRole.parse(intent.getStringExtra("role"))
            : currentRole;
    CaptureProfile requestedProfile =
        intent.hasExtra("capture_profile")
            ? CaptureProfile.parse(intent.getStringExtra("capture_profile"))
            : currentProfile;
    if (requestedRole == currentRole && requestedProfile == currentProfile) {
      return;
    }
    if (!configurationChangeAllowed()) {
      rejectConfigurationChange();
      return;
    }
    configuration.setCaptureConfiguration(requestedRole, requestedProfile);
  }

  private LinearLayout createContentView() {
    int padding = dp(20);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(LinearLayout.VERTICAL);
    root.setPadding(padding, padding, padding, padding);

    TextView heading = new TextView(this);
    heading.setText("Swing Capture Android Node");
    heading.setTextSize(24.0f);
    root.addView(
        heading,
        new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

    TextView identity = new TextView(this);
    identity.setText("Node " + configuration.nodeId());
    identity.setTextSize(13.0f);
    root.addView(identity);

    networkStatus = new TextView(this);
    networkStatus.setTextSize(13.0f);
    networkStatus.setTextIsSelectable(true);
    networkStatus.setText("Node service has not started");
    root.addView(networkStatus);

    roleSpinner = new Spinner(this);
    String[] roles = new String[CaptureRole.values().length];
    for (int index = 0; index < CaptureRole.values().length; ++index) {
      roles[index] = CaptureRole.values()[index].displayName();
    }
    roleSpinner.setAdapter(
        new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, roles));
    selectConfiguredRole();
    root.addView(roleSpinner);

    profileSpinner = new Spinner(this);
    String[] profiles = new String[CaptureProfile.values().length];
    for (int index = 0; index < CaptureProfile.values().length; ++index) {
      profiles[index] = CaptureProfile.values()[index].displayName();
    }
    profileSpinner.setAdapter(
        new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, profiles));
    selectConfiguredProfile();
    root.addView(profileSpinner);

    saveConfigurationButton = new Button(this);
    saveConfigurationButton.setText("Save role and capture profile");
    saveConfigurationButton.setOnClickListener(ignored -> saveCaptureConfiguration());
    root.addView(saveConfigurationButton);

    armButton = new Button(this);
    armButton.setText("Arm continuous capture (screen may turn off)");
    armButton.setOnClickListener(ignored -> sendServiceAction(CaptureForegroundService.ACTION_ARM));
    root.addView(armButton);

    TextView screenOffGuidance = new TextView(this);
    screenOffGuidance.setText(
        "Standard operation uses 720p240. After arming, leave the display off; the foreground "
            + "service and partial wake lock keep capture running.");
    screenOffGuidance.setTextSize(13.0f);
    root.addView(screenOffGuidance);

    disarmButton = new Button(this);
    disarmButton.setText("Disarm continuous capture");
    disarmButton.setOnClickListener(
        ignored -> sendServiceAction(CaptureForegroundService.ACTION_DISARM));
    root.addView(disarmButton);

    manualTriggerButton = new Button(this);
    manualTriggerButton.setText("Trigger retained clip now");
    manualTriggerButton.setOnClickListener(
        ignored -> sendServiceAction(CaptureForegroundService.ACTION_TRIGGER));
    root.addView(manualTriggerButton);

    refreshButton = new Button(this);
    refreshButton.setText("Refresh capability report");
    refreshButton.setOnClickListener(ignored -> refreshCapabilities());
    root.addView(refreshButton);

    probeButton = new Button(this);
    probeButton.setText("Run 3 second selected-profile probe");
    probeButton.setOnClickListener(ignored -> runHighSpeedProbe(selectedCaptureRequest()));
    root.addView(probeButton);

    retainedCaptureButton = new Button(this);
    retainedCaptureButton.setText("Capture retained synthetic-trigger clip");
    retainedCaptureButton.setOnClickListener(
        ignored -> runRetainedCapture(selectedCaptureRequest()));
    root.addView(retainedCaptureButton);

    warmTransitionButton = new Button(this);
    warmTransitionButton.setText("Measure 5 fps standby to 720p240 transition");
    warmTransitionButton.setOnClickListener(
        ignored -> runWarmHighSpeedTransition(selectedCaptureRequest()));
    root.addView(warmTransitionButton);

    status = new TextView(this);
    status.setTypeface(Typeface.MONOSPACE);
    status.setTextSize(12.0f);
    status.setTextIsSelectable(true);
    ScrollView scroll = new ScrollView(this);
    scroll.addView(status);
    root.addView(
        scroll,
        new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f));
    updateConfigurationControls();
    return root;
  }

  private void saveCaptureConfiguration() {
    if (!configurationChangeAllowed()) {
      rejectConfigurationChange();
      return;
    }
    configuration.setCaptureConfiguration(
        CaptureRole.values()[roleSpinner.getSelectedItemPosition()], selectedCaptureProfile());
    refreshCapabilities();
  }

  private boolean configurationChangeAllowed() {
    return configurationEditsLocallyEnabled
        && CaptureConfigurationPolicy.mayChange(CaptureForegroundService.snapshot().state());
  }

  private void rejectConfigurationChange() {
    String message = "Disarm capture before changing the camera role or capture profile.";
    Log.i(TAG, message);
    selectConfiguredRole();
    selectConfiguredProfile();
    if (status != null) {
      status.setText(message);
    }
  }

  private void updateConfigurationControls() {
    boolean enabled = configurationChangeAllowed();
    if (roleSpinner != null) {
      roleSpinner.setEnabled(enabled);
    }
    if (profileSpinner != null) {
      profileSpinner.setEnabled(enabled);
    }
    if (saveConfigurationButton != null) {
      saveConfigurationButton.setEnabled(enabled);
    }
  }

  private boolean requestRuntimePermissions() {
    List<String> missing = new ArrayList<>();
    addIfMissing(missing, Manifest.permission.CAMERA);
    addIfMissing(missing, Manifest.permission.RECORD_AUDIO);
    addIfMissing(missing, Manifest.permission.POST_NOTIFICATIONS);
    if (missing.isEmpty()) {
      return false;
    }
    requestPermissions(missing.toArray(new String[0]), PERMISSION_REQUEST);
    return true;
  }

  private void addIfMissing(List<String> missing, String permission) {
    if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
      missing.add(permission);
    }
  }

  private void runInitialAction() {
    if (capturePermissionsGranted()) {
      ensureNodeService();
    }
    if (initialSoakFinishRequested && capturePermissionsGranted()) {
      initialSoakFinishRequested = false;
      sendServiceAction(CaptureForegroundService.ACTION_FINISH_SOAK_HIL);
      return;
    }
    if ((initialContinuousHilRequested || initialAudioHilRequested || initialSoakHilRequested)
        && capturePermissionsGranted()) {
      boolean audioHil = initialAudioHilRequested;
      boolean soakHil = initialSoakHilRequested;
      initialContinuousHilRequested = false;
      initialAudioHilRequested = false;
      initialSoakHilRequested = false;
      sendServiceAction(
          soakHil
              ? CaptureForegroundService.ACTION_ARM_SOAK_HIL
              : (audioHil
                  ? CaptureForegroundService.ACTION_ARM_AUDIO_HIL
                  : CaptureForegroundService.ACTION_ARM_CONTINUOUS_HIL),
          getIntent().getStringExtra("shared_session_id"));
      return;
    }
    if ((initialProbeRequested
            || initialRetainedCaptureRequested
            || initialWarmTransitionRequested)
        && capturePermissionsGranted()) {
      initialProbeRequested = false;
      boolean retain = initialRetainedCaptureRequested;
      boolean warmTransition = initialWarmTransitionRequested;
      initialRetainedCaptureRequested = false;
      initialWarmTransitionRequested = false;
      if (warmTransition) {
        runWarmHighSpeedTransition(requestFromIntent(getIntent()));
      } else if (retain) {
        runRetainedCapture(requestFromIntent(getIntent()));
      } else {
        runHighSpeedProbe(requestFromIntent(getIntent()));
      }
    } else {
      initialProbeRequested = false;
      initialRetainedCaptureRequested = false;
      initialWarmTransitionRequested = false;
      refreshCapabilities();
    }
  }

  private boolean capturePermissionsGranted() {
    return checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            == PackageManager.PERMISSION_GRANTED;
  }

  private HighSpeedProbe.Request requestFromIntent(Intent intent) {
    CaptureProfile standard = CaptureProfile.standard();
    return new HighSpeedProbe.Request(
        intent.getIntExtra("probe_width", standard.width()),
        intent.getIntExtra("probe_height", standard.height()),
        intent.getIntExtra("probe_fps", 240),
        intent.getIntExtra("probe_duration_ms", 3_000),
        intent.getIntExtra("probe_bitrate", standard.bitrateBitsPerSecond()),
        intent.getStringExtra("probe_mime") == null
            ? MediaFormat.MIMETYPE_VIDEO_AVC
            : intent.getStringExtra("probe_mime"));
  }

  private void selectConfiguredRole() {
    if (roleSpinner != null) {
      roleSpinner.setSelection(configuration.role().ordinal());
    }
  }

  private void selectConfiguredProfile() {
    if (profileSpinner != null) {
      profileSpinner.setSelection(configuration.captureProfile().ordinal());
    }
  }

  private CaptureProfile selectedCaptureProfile() {
    return CaptureProfile.values()[profileSpinner.getSelectedItemPosition()];
  }

  private HighSpeedProbe.Request selectedCaptureRequest() {
    CaptureProfile profile = selectedCaptureProfile();
    return new HighSpeedProbe.Request(
        profile.width(),
        profile.height(),
        240,
        3_000,
        profile.bitrateBitsPerSecond(),
        MediaFormat.MIMETYPE_VIDEO_AVC);
  }

  private void refreshCapabilities() {
    if (status == null) {
      return;
    }
    setControlsEnabled(false);
    status.setText("Collecting capabilities…");
    worker.execute(
        () -> {
          try {
            JSONObject report = CapabilityInventory.collect(this, configuration);
            String json = report.toString(2) + "\n";
            File file = ReportStore.writeLatest(this, json);
            Log.i(TAG, "Capability report written to " + file);
            runOnUiThread(
                () -> {
                  status.setText(json);
                  setControlsEnabled(true);
                });
          } catch (Exception failure) {
            Log.e(TAG, "Capability inventory failed", failure);
            runOnUiThread(
                () -> {
                  status.setText("Capability inventory failed: " + failure);
                  setControlsEnabled(true);
                });
          }
        });
  }

  private void runHighSpeedProbe(HighSpeedProbe.Request request) {
    runCapture(request, false);
  }

  private void runRetainedCapture(HighSpeedProbe.Request request) {
    runCapture(request, true);
  }

  private void runWarmHighSpeedTransition(HighSpeedProbe.Request request) {
    if (status == null) {
      return;
    }
    if (!capturePermissionsGranted()) {
      status.setText("Camera permission is required for the warm transition probe.");
      requestRuntimePermissions();
      return;
    }
    setControlsEnabled(false);
    CaptureConfigurationSnapshot captureConfiguration;
    try {
      captureConfiguration = configuration.captureSnapshot();
      captureConfiguration.requireAssignedRole();
    } catch (RuntimeException invalidConfiguration) {
      status.setText("Capture configuration is invalid: " + invalidConfiguration.getMessage());
      setControlsEnabled(true);
      return;
    }
    status.setText(
        "Measuring warm 5 fps standby to "
            + request.width()
            + "x"
            + request.height()
            + " / "
            + request.framesPerSecond()
            + " fps…");
    worker.execute(
        () -> {
          try {
            JSONObject report =
                WarmHighSpeedTransitionProbe.run(this, captureConfiguration, request);
            String json = report.toString(2) + "\n";
            File file = ReportStore.writeLatest(this, json);
            Log.i(TAG, "Warm transition report written to " + file);
            runOnUiThread(
                () -> {
                  status.setText(json);
                  setControlsEnabled(true);
                });
          } catch (Exception failure) {
            Log.e(TAG, "Warm transition probe failed before producing a report", failure);
            runOnUiThread(
                () -> {
                  status.setText("Warm transition probe failed: " + failure);
                  setControlsEnabled(true);
                });
          }
        });
  }

  private void runCapture(HighSpeedProbe.Request request, boolean retainSession) {
    if (status == null) {
      return;
    }
    if (!capturePermissionsGranted()) {
      status.setText("Camera and microphone permissions are required for the probe.");
      requestRuntimePermissions();
      return;
    }
    setControlsEnabled(false);
    CaptureConfigurationSnapshot captureConfiguration;
    try {
      captureConfiguration = configuration.captureSnapshot();
      captureConfiguration.requireAssignedRole();
    } catch (RuntimeException invalidConfiguration) {
      status.setText("Capture configuration is invalid: " + invalidConfiguration.getMessage());
      setControlsEnabled(true);
      return;
    }
    status.setText(
        (retainSession ? "Capturing retained " : "Running bounded ")
            + request.width()
            + "x"
            + request.height()
            + " / "
            + request.framesPerSecond()
            + " fps camera + microphone probe…");
    worker.execute(
        () -> {
          try {
            JSONObject report =
                retainSession
                    ? HighSpeedProbe.runRetained(this, captureConfiguration, request)
                    : HighSpeedProbe.run(this, captureConfiguration, request);
            String json = report.toString(2) + "\n";
            File file = ReportStore.writeLatest(this, json);
            Log.i(TAG, "High-speed probe report written to " + file);
            runOnUiThread(
                () -> {
                  status.setText(json);
                  setControlsEnabled(true);
                });
          } catch (Exception failure) {
            Log.e(TAG, "High-speed probe failed before producing a report", failure);
            runOnUiThread(
                () -> {
                  status.setText("High-speed probe failed: " + failure);
                  setControlsEnabled(true);
                });
          }
        });
  }

  private void setControlsEnabled(boolean enabled) {
    configurationEditsLocallyEnabled = enabled;
    refreshButton.setEnabled(enabled);
    probeButton.setEnabled(enabled);
    retainedCaptureButton.setEnabled(enabled);
    warmTransitionButton.setEnabled(enabled);
    updateConfigurationControls();
  }

  private void ensureNodeService() {
    try {
      Intent service = new Intent(this, CaptureForegroundService.class);
      service.setAction(CaptureForegroundService.ACTION_START);
      startForegroundService(service);
      refreshServiceDisplay();
      scheduleServiceDisplayRefresh(500);
    } catch (Exception failure) {
      Log.e(TAG, "Unable to start node service", failure);
      networkStatus.setText("Node service unavailable: " + failure.getMessage());
    }
  }

  private void sendServiceAction(String action) {
    sendServiceAction(action, null);
  }

  private void sendServiceAction(String action, String sharedSessionId) {
    if (!capturePermissionsGranted()) {
      requestRuntimePermissions();
      return;
    }
    Intent service = new Intent(this, CaptureForegroundService.class);
    service.setAction(action);
    if (sharedSessionId != null) {
      service.putExtra(CaptureForegroundService.EXTRA_SHARED_SESSION_ID, sharedSessionId);
    }
    startForegroundService(service);
    scheduleServiceDisplayRefresh(250);
  }

  private void refreshServiceDisplay() {
    CaptureRuntime.Snapshot capture = CaptureForegroundService.snapshot();
    List<String> urls = CaptureForegroundService.advertisedUrls();
    String endpoint =
        urls.isEmpty()
            ? "HTTP API starting on port " + NodeHttpServer.DEFAULT_PORT
            : "HTTP API " + String.join(" · ", urls);
    String reviewUrls =
        urls.isEmpty()
            ? ""
            : "\nReview URL "
                + String.join(
                    " · ",
                    urls.stream()
                        .map(url -> url + "/?node_token=" + configuration.controlToken() + "#review")
                        .toList());
    networkStatus.setText(
        endpoint
            + reviewUrls
            + "\nControl credential (keep private): "
            + configuration.controlToken()
            + "\nState: "
            + capture.state().wireName());
    updateConfigurationControls();
    if (!CaptureConfigurationPolicy.mayChange(capture.state())) {
      scheduleServiceDisplayRefresh(500);
    }
  }

  private void scheduleServiceDisplayRefresh(long delayMillis) {
    networkStatus.removeCallbacks(serviceDisplayRefresh);
    networkStatus.postDelayed(serviceDisplayRefresh, delayMillis);
  }

  private int dp(int value) {
    return Math.round(value * getResources().getDisplayMetrics().density);
  }
}
