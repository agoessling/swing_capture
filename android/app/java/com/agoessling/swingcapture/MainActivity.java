package com.agoessling.swingcapture;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.media.MediaFormat;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.PoseProjection;
import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegatePolicy;
import com.agoessling.swingcapture.pose.inference.PoseReplayConfiguration;
import com.agoessling.swingcapture.pose.inference.PoseReplayHilRunner;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
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
  private Spinner poseModeSpinner;
  private Spinner poseDelegateSpinner;
  private EditText peerOriginEdit;
  private EditText peerTokenEdit;
  private CheckBox debugEvidenceCheckBox;
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
  private Button savePoseConfigurationButton;
  private boolean configurationEditsLocallyEnabled = true;
  private boolean initialProbeRequested;
  private boolean initialRetainedCaptureRequested;
  private boolean initialWarmTransitionRequested;
  private boolean initialContinuousHilRequested;
  private boolean initialAudioHilRequested;
  private boolean initialSoakHilRequested;
  private boolean initialSoakFinishRequested;
  private boolean initialPoseStandbyHilRequested;
  private boolean initialPoseReplayHilRequested;
  private boolean initialPoseArmHilEnabled;
  private boolean initialAutonomousRecoveryHilRequested;
  private boolean initialFieldRecordingStartRejectionHilRequested;
  private boolean initialAcceptedFieldRecordingStartFailureHilRequested;
  private boolean initialAcceptedFieldRecordingStartFailureHilReleaseRequested;
  private boolean initialFieldRecordingStartRejectionHilClearRequested;

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
    initialPoseStandbyHilRequested =
        getIntent().getBooleanExtra("run_pose_standby_hil", false);
    initialPoseReplayHilRequested = getIntent().getBooleanExtra("run_pose_replay_hil", false);
    initialPoseArmHilEnabled = getIntent().getBooleanExtra("enable_pose_arm_hil", false);
    initialAutonomousRecoveryHilRequested =
        getIntent().getBooleanExtra("prepare_autonomous_recovery_hil", false);
    initialFieldRecordingStartRejectionHilRequested =
        getIntent().getBooleanExtra("reject_next_field_recording_start_hil", false);
    initialAcceptedFieldRecordingStartFailureHilRequested =
        getIntent().getBooleanExtra("fail_next_accepted_field_recording_start_hil", false);
    initialAcceptedFieldRecordingStartFailureHilReleaseRequested =
        getIntent().getBooleanExtra("release_accepted_field_recording_start_failure_hil", false);
    initialFieldRecordingStartRejectionHilClearRequested =
        getIntent().getBooleanExtra("clear_field_recording_start_hil", false);
    setContentView(createContentView());
    if (initialPoseReplayHilRequested || !requestRuntimePermissions()) {
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
    initialPoseStandbyHilRequested = intent.getBooleanExtra("run_pose_standby_hil", false);
    initialPoseReplayHilRequested = intent.getBooleanExtra("run_pose_replay_hil", false);
    initialPoseArmHilEnabled = intent.getBooleanExtra("enable_pose_arm_hil", false);
    initialAutonomousRecoveryHilRequested =
        intent.getBooleanExtra("prepare_autonomous_recovery_hil", false);
    initialFieldRecordingStartRejectionHilRequested =
        intent.getBooleanExtra("reject_next_field_recording_start_hil", false);
    initialAcceptedFieldRecordingStartFailureHilRequested =
        intent.getBooleanExtra("fail_next_accepted_field_recording_start_hil", false);
    initialAcceptedFieldRecordingStartFailureHilReleaseRequested =
        intent.getBooleanExtra("release_accepted_field_recording_start_failure_hil", false);
    initialFieldRecordingStartRejectionHilClearRequested =
        intent.getBooleanExtra("clear_field_recording_start_hil", false);
    selectConfiguredRole();
    selectConfiguredProfile();
    if (initialPoseReplayHilRequested || !requestRuntimePermissions()) {
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
    NodeConfiguration.StationConfiguration current = configuration.stationConfiguration();
    CaptureRole currentRole = current.capture().role();
    CaptureProfile currentProfile = current.capture().profile();
    CaptureRole requestedRole =
        intent.hasExtra("role")
            ? CaptureRole.parse(intent.getStringExtra("role"))
            : currentRole;
    CaptureProfile requestedProfile =
        intent.hasExtra("capture_profile")
            ? CaptureProfile.parse(intent.getStringExtra("capture_profile"))
            : currentProfile;
    PoseStationConfigurationSnapshot requestedPose = requestedIntentPose(intent, current.pose());
    if (requestedRole == currentRole
        && requestedProfile == currentProfile
        && requestedPose.equals(current.pose())) {
      return;
    }
    if (!configurationChangeAllowed()) {
      rejectConfigurationChange();
      return;
    }
    configuration.setStationConfiguration(requestedRole, requestedProfile, requestedPose);
  }

  private static PoseStationConfigurationSnapshot requestedIntentPose(
      Intent intent, PoseStationConfigurationSnapshot current) {
    if (!intent.hasExtra("pose_mode")
        && !intent.hasExtra("pose_delegate")
        && !intent.hasExtra("pose_hitting_region")
        && !intent.hasExtra("pose_debug_evidence")
        && !intent.hasExtra("pose_peer_origin")
        && !intent.hasExtra("pose_peer_control_token")) {
      return current;
    }
    NormalizedHittingRegion region = current.hittingRegion();
    if (intent.hasExtra("pose_hitting_region")) {
      String[] fields =
          intent.getStringExtra("pose_hitting_region").trim().split(",", -1);
      if (fields.length != 4) {
        throw new IllegalArgumentException("pose_hitting_region requires four coordinates");
      }
      region =
          new NormalizedHittingRegion(
              Double.parseDouble(fields[0]),
              Double.parseDouble(fields[1]),
              Double.parseDouble(fields[2]),
              Double.parseDouble(fields[3]));
    }
    boolean modeExplicit = intent.hasExtra("pose_mode");
    PoseNodeMode requestedMode =
        modeExplicit ? PoseNodeMode.parse(intent.getStringExtra("pose_mode")) : current.mode();
    boolean originExplicit = intent.hasExtra("pose_peer_origin");
    boolean controlTokenExplicit = intent.hasExtra("pose_peer_control_token");
    PoseIntentPeerPolicy.Peer peer =
        PoseIntentPeerPolicy.resolve(
            requestedMode,
            modeExplicit,
            current.peerOrigin(),
            current.peerControlToken(),
            originExplicit,
            originExplicit ? intent.getStringExtra("pose_peer_origin") : "",
            controlTokenExplicit,
            controlTokenExplicit ? intent.getStringExtra("pose_peer_control_token") : "");
    return new PoseStationConfigurationSnapshot(
            requestedMode,
            intent.hasExtra("pose_delegate")
                ? PoseStationConfigurationSnapshot.parseDelegatePolicy(
                    intent.getStringExtra("pose_delegate"))
                : current.delegatePolicy(),
            region,
            intent.hasExtra("pose_debug_evidence")
                ? intent.getBooleanExtra("pose_debug_evidence", true)
                : current.debugEvidenceEnabled(),
            peer.origin(),
            peer.controlToken());
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

    TextView poseHeading = new TextView(this);
    poseHeading.setText("Low-rate pose trigger prototype");
    poseHeading.setTextSize(18.0f);
    root.addView(poseHeading);

    PoseStationConfigurationSnapshot poseConfiguration =
        configuration.poseConfigurationSnapshot();
    poseModeSpinner = new Spinner(this);
    String[] poseModes = new String[PoseNodeMode.values().length];
    for (int index = 0; index < PoseNodeMode.values().length; ++index) {
      poseModes[index] = PoseNodeMode.values()[index].displayName();
    }
    poseModeSpinner.setAdapter(
        new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, poseModes));
    poseModeSpinner.setSelection(poseConfiguration.mode().ordinal());
    root.addView(poseModeSpinner);

    poseDelegateSpinner = new Spinner(this);
    String[] delegates = new String[PoseInferenceDelegatePolicy.values().length];
    for (int index = 0; index < PoseInferenceDelegatePolicy.values().length; ++index) {
      delegates[index] = PoseInferenceDelegatePolicy.values()[index].name();
    }
    poseDelegateSpinner.setAdapter(
        new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, delegates));
    poseDelegateSpinner.setSelection(poseConfiguration.delegatePolicy().ordinal());
    root.addView(poseDelegateSpinner);

    peerOriginEdit = new EditText(this);
    peerOriginEdit.setHint("Optional peer origin, e.g. http://192.168.1.20:8088");
    peerOriginEdit.setText(poseConfiguration.peerOrigin());
    root.addView(peerOriginEdit);

    peerTokenEdit = new EditText(this);
    peerTokenEdit.setHint("Optional peer control token");
    peerTokenEdit.setInputType(
        InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
    peerTokenEdit.setText(poseConfiguration.peerControlToken());
    root.addView(peerTokenEdit);

    debugEvidenceCheckBox = new CheckBox(this);
    debugEvidenceCheckBox.setText("Retain 60 seconds of low-rate debug evidence in memory");
    debugEvidenceCheckBox.setChecked(poseConfiguration.debugEvidenceEnabled());
    root.addView(debugEvidenceCheckBox);

    savePoseConfigurationButton = new Button(this);
    savePoseConfigurationButton.setText("Save pose trigger configuration");
    savePoseConfigurationButton.setOnClickListener(ignored -> savePoseConfiguration());
    root.addView(savePoseConfigurationButton);

    armButton = new Button(this);
    updateArmButtonLabel(poseConfiguration.mode());
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

  private void savePoseConfiguration() {
    if (!configurationChangeAllowed()) {
      rejectConfigurationChange();
      return;
    }
    try {
      PoseStationConfigurationSnapshot snapshot =
          new PoseStationConfigurationSnapshot(
              PoseNodeMode.values()[poseModeSpinner.getSelectedItemPosition()],
              PoseInferenceDelegatePolicy.values()[poseDelegateSpinner.getSelectedItemPosition()],
              PoseStationConfigurationSnapshot.DEFAULT_HITTING_REGION,
              debugEvidenceCheckBox.isChecked(),
              peerOriginEdit.getText().toString(),
              peerTokenEdit.getText().toString());
      configuration.setPoseConfiguration(snapshot);
      updateArmButtonLabel(snapshot.mode());
      status.setText("Pose trigger configuration saved.");
    } catch (RuntimeException invalid) {
      status.setText("Pose trigger configuration is invalid: " + invalid.getMessage());
    }
  }

  private void updateArmButtonLabel(PoseNodeMode mode) {
    if (armButton == null) {
      return;
    }
    armButton.setText(
        mode == PoseNodeMode.DISABLED
            ? "Arm continuous 720p240 capture"
            : "Start 5 Hz pose monitoring (screen may turn off)");
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
    if (poseModeSpinner != null) {
      poseModeSpinner.setEnabled(enabled);
      poseDelegateSpinner.setEnabled(enabled);
      peerOriginEdit.setEnabled(enabled);
      peerTokenEdit.setEnabled(enabled);
      debugEvidenceCheckBox.setEnabled(enabled);
      savePoseConfigurationButton.setEnabled(enabled);
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
    if (initialPoseReplayHilRequested) {
      initialPoseReplayHilRequested = false;
      runPoseReplayHil(getIntent());
      return;
    }
    if (capturePermissionsGranted()) {
      ensureNodeService();
    }
    if (initialFieldRecordingStartRejectionHilRequested && capturePermissionsGranted()) {
      initialFieldRecordingStartRejectionHilRequested = false;
      sendServiceAction(
          CaptureForegroundService.ACTION_REJECT_NEXT_FIELD_RECORDING_START_HIL);
      return;
    }
    if (initialAcceptedFieldRecordingStartFailureHilRequested && capturePermissionsGranted()) {
      initialAcceptedFieldRecordingStartFailureHilRequested = false;
      sendServiceAction(
          CaptureForegroundService.ACTION_FAIL_NEXT_ACCEPTED_FIELD_RECORDING_START_HIL);
      return;
    }
    if (initialAcceptedFieldRecordingStartFailureHilReleaseRequested
        && capturePermissionsGranted()) {
      initialAcceptedFieldRecordingStartFailureHilReleaseRequested = false;
      sendServiceAction(
          CaptureForegroundService.ACTION_RELEASE_ACCEPTED_FIELD_RECORDING_START_FAILURE_HIL);
      return;
    }
    if (initialFieldRecordingStartRejectionHilClearRequested && capturePermissionsGranted()) {
      initialFieldRecordingStartRejectionHilClearRequested = false;
      sendServiceAction(
          CaptureForegroundService.ACTION_CLEAR_FIELD_RECORDING_START_HIL);
      return;
    }
    if (initialAutonomousRecoveryHilRequested && capturePermissionsGranted()) {
      initialAutonomousRecoveryHilRequested = false;
      Intent service = new Intent(this, CaptureForegroundService.class);
      service.setAction(CaptureForegroundService.ACTION_PREPARE_AUTONOMOUS_RECOVERY_HIL);
      service.putExtra(
          CaptureForegroundService.EXTRA_SHARED_SESSION_ID,
          getIntent().getStringExtra("shared_session_id"));
      service.putExtra(
          CaptureForegroundService.EXTRA_PEER_NODE_ID,
          getIntent().getStringExtra("peer_node_id"));
      startForegroundService(service);
      return;
    }
    if (initialSoakFinishRequested && capturePermissionsGranted()) {
      initialSoakFinishRequested = false;
      sendServiceAction(CaptureForegroundService.ACTION_FINISH_SOAK_HIL);
      return;
    }
    if (initialPoseStandbyHilRequested && capturePermissionsGranted()) {
      initialPoseStandbyHilRequested = false;
      sendPoseExperimentHilAction(getIntent());
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

  private void runPoseReplayHil(Intent intent) {
    if (status == null) {
      return;
    }
    setControlsEnabled(false);
    try {
      CaptureRole role = CaptureRole.parse(intent.getStringExtra("pose_replay_role"));
      if (role == CaptureRole.UNASSIGNED) {
        throw new IllegalArgumentException("pose replay role is required");
      }
      PoseProjection projection =
          intent.hasExtra("pose_replay_projection")
              ? PoseProjection.parse(intent.getStringExtra("pose_replay_projection"))
              : PoseStandbyPolicy.projectionForRole(role);
      NormalizedHittingRegion hittingRegion =
          parsePoseReplayHittingRegion(intent.getStringExtra("pose_replay_hitting_region"));
      PoseInferenceDelegatePolicy delegatePolicy =
          PoseStationConfigurationSnapshot.parseDelegatePolicy(
              intent.getStringExtra("pose_replay_delegate"));
      PoseReplayConfiguration.Expectation expectation =
          parsePoseReplayExpectation(intent.getStringExtra("pose_replay_expectation"));
      PoseReplayConfiguration defaults =
          PoseReplayConfiguration.defaults(
              role.wireName(), projection, hittingRegion, delegatePolicy, expectation);
      int maximumFrames =
          intent.getIntExtra(
              "pose_replay_maximum_frames", PoseReplayConfiguration.DEFAULT_MAXIMUM_FRAMES);
      PoseReplayConfiguration replayConfiguration =
          new PoseReplayConfiguration(
              defaults.role(),
              defaults.projection(),
              defaults.hittingRegion(),
              defaults.delegatePolicy(),
              defaults.observationConfig(),
              defaults.controllerConfig(),
              defaults.expectation(),
              maximumFrames);
      String clipName = requirePoseReplayClipName(intent.getStringExtra("pose_replay_clip"));
      File replayRoot = new File(getFilesDir(), "pose_replay_hil");
      File clip = new File(replayRoot, clipName);
      status.setText("Running on-device pose replay for " + clipName + "…");
      new PoseReplayHilRunner()
          .runAndPersistAsync(
              worker,
              this,
              replayRoot,
              clip,
              replayConfiguration,
              json -> ReportStore.writeLatest(this, json))
          .whenComplete(
              (published, failure) ->
                  runOnUiThread(
                      () -> {
                        if (failure != null) {
                          Log.e(TAG, "Pose replay failed before publishing its report", failure);
                          status.setText("Pose replay report publication failed: " + failure);
                        } else {
                          Log.i(TAG, "Pose replay report written to " + published.file());
                          status.setText(published.report().toJson());
                        }
                        setControlsEnabled(true);
                      }));
    } catch (RuntimeException invalidRequest) {
      Log.e(TAG, "Pose replay request is invalid", invalidRequest);
      status.setText("Pose replay request is invalid: " + invalidRequest.getMessage());
      setControlsEnabled(true);
    }
  }

  private static NormalizedHittingRegion parsePoseReplayHittingRegion(String value) {
    String text = value == null ? "0.0,0.0,1.0,1.0" : value;
    String[] fields = text.trim().split(",", -1);
    if (fields.length != 4) {
      throw new IllegalArgumentException("pose replay hitting region requires four coordinates");
    }
    return new NormalizedHittingRegion(
        Double.parseDouble(fields[0].trim()),
        Double.parseDouble(fields[1].trim()),
        Double.parseDouble(fields[2].trim()),
        Double.parseDouble(fields[3].trim()));
  }

  private static PoseReplayConfiguration.Expectation parsePoseReplayExpectation(String value) {
    String normalized = value == null ? "OBSERVE_ONLY" : value.trim().toUpperCase(Locale.ROOT);
    try {
      return PoseReplayConfiguration.Expectation.valueOf(normalized);
    } catch (IllegalArgumentException invalid) {
      throw new IllegalArgumentException("Unknown pose replay expectation: " + value, invalid);
    }
  }

  private static String requirePoseReplayClipName(String value) {
    if (value == null || value.isEmpty() || value.length() > 128 || !value.endsWith(".mp4")) {
      throw new IllegalArgumentException("pose replay clip must be a bounded MP4 basename");
    }
    for (int index = 0; index < value.length(); index++) {
      char character = value.charAt(index);
      boolean safe =
          (character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '.'
              || character == '_'
              || character == '-';
      if (!safe) {
        throw new IllegalArgumentException("pose replay clip must be a safe basename");
      }
    }
    return value;
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
      service.setAction(
          initialPoseArmHilEnabled
              ? CaptureForegroundService.ACTION_START_POSE_HIL
              : CaptureForegroundService.ACTION_START);
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

  private void sendPoseExperimentHilAction(Intent source) {
    PoseExperimentConfiguration experiment =
        PoseExperimentConfiguration.parse(
            source.getStringExtra(CaptureForegroundService.EXTRA_POSE_EXPERIMENT_MODEL),
            source.getIntExtra(
                CaptureForegroundService.EXTRA_POSE_EXPERIMENT_STANDBY_WIDTH, -1),
            source.getIntExtra(
                CaptureForegroundService.EXTRA_POSE_EXPERIMENT_STANDBY_HEIGHT, -1));
    Intent service = new Intent(this, CaptureForegroundService.class);
    service.setAction(CaptureForegroundService.ACTION_ARM_POSE_EXPERIMENT_HIL);
    service.putExtra(
        CaptureForegroundService.EXTRA_POSE_EXPERIMENT_MODEL,
        experiment.modelVariant().wireName());
    service.putExtra(
        CaptureForegroundService.EXTRA_POSE_EXPERIMENT_STANDBY_WIDTH,
        experiment.standbyWidth());
    service.putExtra(
        CaptureForegroundService.EXTRA_POSE_EXPERIMENT_STANDBY_HEIGHT,
        experiment.standbyHeight());
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
