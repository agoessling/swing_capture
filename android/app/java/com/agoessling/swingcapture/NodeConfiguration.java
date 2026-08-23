package com.agoessling.swingcapture;

import android.content.Context;
import android.content.SharedPreferences;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import java.security.SecureRandom;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;

/** Persistent per-installation identity, camera role, and capture profile. */
public final class NodeConfiguration {
  private static final String PREFERENCES = "node_configuration";
  private static final String NODE_ID = "node_id";
  private static final String CAPTURE_ROLE = "capture_role";
  private static final String CAPTURE_PROFILE = "capture_profile";
  private static final String CONTROL_TOKEN = "control_token";
  private static final String CONTROL_TOKEN_GENERATION = "control_token_generation";
  private static final String POSE_MODE = "pose_mode";
  private static final String POSE_DELEGATE = "pose_delegate";
  private static final String POSE_REGION_LEFT = "pose_region_left";
  private static final String POSE_REGION_TOP = "pose_region_top";
  private static final String POSE_REGION_RIGHT = "pose_region_right";
  private static final String POSE_REGION_BOTTOM = "pose_region_bottom";
  private static final String POSE_DEBUG_EVIDENCE = "pose_debug_evidence";
  private static final String POSE_PEER_ORIGIN = "pose_peer_origin";
  private static final String POSE_PEER_CONTROL_TOKEN = "pose_peer_control_token";
  private static final String PAIRING_STATE = "pairing_state";
  private static final String PAIRING_PEER_NODE_ID = "pairing_peer_node_id";
  private static final String PAIRING_EXPECTED_ROLE = "pairing_expected_role";
  private static final String PAIRING_LABEL = "pairing_label";
  private static final String PAIRING_ORIGIN = "pairing_origin";
  private static final String PAIRING_CREDENTIAL_GENERATION = "pairing_credential_generation";
  private static final String PAIRING_VERIFIED_AT = "pairing_verified_at";
  private static final String PAIRING_REVOKED_AT = "pairing_revoked_at";
  private static final String SETUP_REVISION = "setup_revision";

  private final SharedPreferences preferences;

  public NodeConfiguration(Context context) {
    preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
  }

  public synchronized String nodeId() {
    return NodeConfigurationProcessLock.call(
        () -> {
          String existing = preferences.getString(NODE_ID, null);
          if (existing != null) {
            return existing;
          }
          String generated = UUID.randomUUID().toString();
          if (!preferences.edit().putString(NODE_ID, generated).commit()) {
            throw new IllegalStateException("Unable to persist node identity");
          }
          return generated;
        });
  }

  public synchronized CaptureRole role() {
    return NodeConfigurationProcessLock.call(
        () -> CaptureRole.parse(preferences.getString(CAPTURE_ROLE, null)));
  }

  public synchronized CaptureProfile captureProfile() {
    return NodeConfigurationProcessLock.call(
        () -> CaptureProfile.parse(preferences.getString(CAPTURE_PROFILE, null)));
  }

  /** Reads the persistent capture identity from one SharedPreferences generation. */
  public synchronized CaptureConfigurationSnapshot captureSnapshot() {
    return stationConfiguration().capture();
  }

  /** One persistent generation used by the authenticated browser setup endpoint. */
  public record StationConfiguration(
      long revision,
      long controlCredentialGeneration,
      CaptureConfigurationSnapshot capture,
      PoseStationConfigurationSnapshot pose,
      Optional<PeerPairingBinding> pairing) {
    public StationConfiguration {
      if (revision < 0) {
        throw new IllegalArgumentException("setup revision cannot be negative");
      }
      if (controlCredentialGeneration < 1) {
        throw new IllegalArgumentException("control credential generation must be positive");
      }
      java.util.Objects.requireNonNull(capture, "capture");
      java.util.Objects.requireNonNull(pose, "pose");
      java.util.Objects.requireNonNull(pairing, "pairing");
    }
  }

  /** Signals an optimistic browser update based on a stale persistent generation. */
  public static final class RevisionMismatchException extends IllegalStateException {
    public RevisionMismatchException() {
      super("setup configuration changed; reload before saving");
    }
  }

  /** Reads capture and pose configuration from the same SharedPreferences generation. */
  public synchronized StationConfiguration stationConfiguration() {
    return NodeConfigurationProcessLock.call(
        () -> {
          nodeId();
          controlToken();
          return stationConfiguration(preferences.getAll());
        });
  }

  private static StationConfiguration stationConfiguration(Map<String, ?> values) {
    Object storedNodeId = values.get(NODE_ID);
    Object storedRole = values.get(CAPTURE_ROLE);
    Object storedProfile = values.get(CAPTURE_PROFILE);
    if (!(storedNodeId instanceof String identity)) {
      throw new IllegalStateException("Persistent node identity is unavailable");
    }
    CaptureConfigurationSnapshot capture =
        new CaptureConfigurationSnapshot(
            identity,
            CaptureRole.parse(storedRole instanceof String role ? role : null),
            CaptureProfile.parse(storedProfile instanceof String profile ? profile : null));
    return new StationConfiguration(
        revisionValue(values),
        controlCredentialGenerationValue(values),
        capture,
        poseConfiguration(values),
        pairingBinding(values));
  }

  /** Atomically changes the two user-editable capture fields. */
  public synchronized void setCaptureConfiguration(CaptureRole role, CaptureProfile profile) {
    NodeConfigurationProcessLock.run(
        () -> {
          long nextRevision = nextRevision(revisionValue(preferences.getAll()));
          if (!preferences
              .edit()
              .putString(CAPTURE_ROLE, role.wireName())
              .putString(CAPTURE_PROFILE, profile.wireName())
              .putLong(SETUP_REVISION, nextRevision)
              .commit()) {
            throw new IllegalStateException("Unable to persist capture role and profile");
          }
        });
  }

  /** Reads every low-rate pose setting from one SharedPreferences generation. */
  public synchronized PoseStationConfigurationSnapshot poseConfigurationSnapshot() {
    return NodeConfigurationProcessLock.call(
        () -> poseConfiguration(preferences.getAll()));
  }

  private static PoseStationConfigurationSnapshot poseConfiguration(Map<String, ?> values) {
    NormalizedHittingRegion defaults = PoseStationConfigurationSnapshot.DEFAULT_HITTING_REGION;
    return new PoseStationConfigurationSnapshot(
        PoseNodeMode.parse(stringValue(values, POSE_MODE, null)),
        PoseStationConfigurationSnapshot.parseDelegatePolicy(
            stringValue(values, POSE_DELEGATE, null)),
        new NormalizedHittingRegion(
            doubleValue(values, POSE_REGION_LEFT, defaults.left()),
            doubleValue(values, POSE_REGION_TOP, defaults.top()),
            doubleValue(values, POSE_REGION_RIGHT, defaults.right()),
            doubleValue(values, POSE_REGION_BOTTOM, defaults.bottom())),
        booleanValue(values, POSE_DEBUG_EVIDENCE, false),
        stringValue(values, POSE_PEER_ORIGIN, ""),
        stringValue(values, POSE_PEER_CONTROL_TOKEN, ""));
  }

  /** Atomically replaces the complete low-rate pose and peer configuration. */
  public synchronized void setPoseConfiguration(PoseStationConfigurationSnapshot configuration) {
    NodeConfigurationProcessLock.run(
        () -> {
          Map<String, ?> before = preferences.getAll();
          long nextRevision = nextRevision(revisionValue(before));
          SharedPreferences.Editor editor = preferences.edit();
          putPoseConfiguration(editor, configuration);
          putPairingBinding(
              editor, pairingAfterLocalPoseChange(pairingBinding(before), configuration));
          if (!editor.putLong(SETUP_REVISION, nextRevision).commit()) {
            throw new IllegalStateException("Unable to persist pose configuration");
          }
        });
  }

  /** Atomically changes capture and pose fields for one local setup action. */
  public synchronized void setStationConfiguration(
      CaptureRole role,
      CaptureProfile profile,
      PoseStationConfigurationSnapshot pose) {
    NodeConfigurationProcessLock.run(
        () -> {
          Map<String, ?> before = preferences.getAll();
          long committedRevision = nextRevision(revisionValue(before));
          SharedPreferences.Editor editor =
              preferences
                  .edit()
                  .putString(CAPTURE_ROLE, role.wireName())
                  .putString(CAPTURE_PROFILE, profile.wireName());
          putPoseConfiguration(editor, pose);
          putPairingBinding(editor, pairingAfterLocalPoseChange(pairingBinding(before), pose));
          if (!editor.putLong(SETUP_REVISION, committedRevision).commit()) {
            throw new IllegalStateException("Unable to persist station configuration");
          }
        });
  }

  /** Atomically applies one optimistic browser update and returns the committed generation. */
  public synchronized StationConfiguration updateSetupConfiguration(
      long expectedRevision,
      CaptureRole role,
      CaptureProfile profile,
      PoseStationConfigurationSnapshot pose,
      Optional<PeerPairingBinding> pairing) {
    if (expectedRevision < 0) {
      throw new IllegalArgumentException("expected_revision cannot be negative");
    }
    java.util.Objects.requireNonNull(pairing, "pairing");
    return NodeConfigurationProcessLock.call(
        () -> {
          Map<String, ?> before = preferences.getAll();
          long currentRevision = revisionValue(before);
          if (!NodeSetupPolicy.revisionMatches(expectedRevision, currentRevision)) {
            throw new RevisionMismatchException();
          }
          long committedRevision = nextRevision(currentRevision);
          SharedPreferences.Editor editor =
              preferences
                  .edit()
                  .putString(CAPTURE_ROLE, role.wireName())
                  .putString(CAPTURE_PROFILE, profile.wireName());
          putPoseConfiguration(editor, pose);
          putPairingBinding(editor, pairing);
          if (!editor.putLong(SETUP_REVISION, committedRevision).commit()) {
            throw new IllegalStateException("Unable to persist station setup");
          }
          return stationConfiguration(preferences.getAll());
        });
  }

  /** Removes a revoked tombstone only after the caller confirms its complete stable identity. */
  public synchronized StationConfiguration resetRevokedPairing(
      long expectedRevision, String expectedPeerNodeId) {
    if (expectedRevision < 0 || expectedPeerNodeId == null || expectedPeerNodeId.isBlank()) {
      throw new IllegalArgumentException("revision and complete peer node ID are required");
    }
    return NodeConfigurationProcessLock.call(
        () -> {
          Map<String, ?> before = preferences.getAll();
          long currentRevision = revisionValue(before);
          if (!NodeSetupPolicy.revisionMatches(expectedRevision, currentRevision)) {
            throw new RevisionMismatchException();
          }
          PeerPairingBinding binding =
              pairingBinding(before)
                  .orElseThrow(() -> new IllegalStateException("No pairing tombstone exists"));
          if (binding.state() != PeerPairingBinding.State.REVOKED) {
            throw new IllegalStateException("Revoke the active peer before resetting its identity");
          }
          if (!binding.peerNodeId().equals(expectedPeerNodeId)) {
            throw new IllegalArgumentException("The confirmed peer node ID does not match");
          }
          SharedPreferences.Editor editor = preferences.edit();
          putPairingBinding(editor, Optional.empty());
          if (!editor.putLong(SETUP_REVISION, nextRevision(currentRevision)).commit()) {
            throw new IllegalStateException("Unable to reset pairing identity");
          }
          return stationConfiguration(preferences.getAll());
        });
  }

  private static void putPoseConfiguration(
      SharedPreferences.Editor editor, PoseStationConfigurationSnapshot configuration) {
    NodeSetupPolicy.requireOutboundPeerAllowed(
        configuration.mode().wireName(),
        new NodeSetupPolicy.PeerCredentials(
            configuration.peerOrigin(), configuration.peerControlToken()));
    editor
        .putString(POSE_MODE, configuration.mode().wireName())
        .putString(POSE_DELEGATE, configuration.delegateWireName())
        .putString(POSE_REGION_LEFT, Double.toString(configuration.hittingRegion().left()))
        .putString(POSE_REGION_TOP, Double.toString(configuration.hittingRegion().top()))
        .putString(POSE_REGION_RIGHT, Double.toString(configuration.hittingRegion().right()))
        .putString(POSE_REGION_BOTTOM, Double.toString(configuration.hittingRegion().bottom()))
        .putBoolean(POSE_DEBUG_EVIDENCE, configuration.debugEvidenceEnabled())
        .putString(POSE_PEER_ORIGIN, configuration.peerOrigin())
        .putString(POSE_PEER_CONTROL_TOKEN, configuration.peerControlToken());
  }

  private static Optional<PeerPairingBinding> pairingBinding(Map<String, ?> values) {
    String state = stringValue(values, PAIRING_STATE, "");
    if (state.isEmpty()) {
      return Optional.empty();
    }
    try {
      return Optional.of(
          new PeerPairingBinding(
              PeerPairingBinding.State.valueOf(state),
              stringValue(values, PAIRING_PEER_NODE_ID, ""),
              stringValue(values, PAIRING_EXPECTED_ROLE, ""),
              stringValue(values, PAIRING_LABEL, ""),
              stringValue(values, PAIRING_ORIGIN, ""),
              requiredLong(values, PAIRING_CREDENTIAL_GENERATION),
              requiredLong(values, PAIRING_VERIFIED_AT),
              requiredLong(values, PAIRING_REVOKED_AT)));
    } catch (IllegalArgumentException malformed) {
      throw new IllegalStateException("Stored peer pairing binding is malformed", malformed);
    }
  }

  private static void putPairingBinding(
      SharedPreferences.Editor editor, Optional<PeerPairingBinding> pairing) {
    if (pairing.isEmpty()) {
      editor
          .remove(PAIRING_STATE)
          .remove(PAIRING_PEER_NODE_ID)
          .remove(PAIRING_EXPECTED_ROLE)
          .remove(PAIRING_LABEL)
          .remove(PAIRING_ORIGIN)
          .remove(PAIRING_CREDENTIAL_GENERATION)
          .remove(PAIRING_VERIFIED_AT)
          .remove(PAIRING_REVOKED_AT);
      return;
    }
    PeerPairingBinding binding = pairing.orElseThrow();
    editor
        .putString(PAIRING_STATE, binding.state().name())
        .putString(PAIRING_PEER_NODE_ID, binding.peerNodeId())
        .putString(PAIRING_EXPECTED_ROLE, binding.expectedRole())
        .putString(PAIRING_LABEL, binding.label())
        .putString(PAIRING_ORIGIN, binding.origin())
        .putLong(PAIRING_CREDENTIAL_GENERATION, binding.credentialGeneration())
        .putLong(PAIRING_VERIFIED_AT, binding.verifiedAtEpochMillis())
        .putLong(PAIRING_REVOKED_AT, binding.revokedAtEpochMillis());
  }

  private static Optional<PeerPairingBinding> pairingAfterLocalPoseChange(
      Optional<PeerPairingBinding> current, PoseStationConfigurationSnapshot pose) {
    if (current.isEmpty() || current.orElseThrow().state() == PeerPairingBinding.State.REVOKED) {
      return current;
    }
    PeerPairingBinding active = current.orElseThrow();
    if (pose.hasPeer() && pose.peerOrigin().equals(active.origin())) {
      return current;
    }
    return Optional.of(active.revoke(System.currentTimeMillis()));
  }

  /** Secret shown only on the phone and required for state-changing network requests. */
  public synchronized String controlToken() {
    return NodeConfigurationProcessLock.call(() -> ensureControlCredential().token());
  }

  /** One-time secret returned only by the explicitly confirmed credential-rotation action. */
  public record ControlCredentialRotation(
      String controlToken,
      long controlCredentialGeneration,
      StationConfiguration stationConfiguration) {
    public ControlCredentialRotation {
      if (!BearerAuthorization.isValidToken(controlToken)
          || controlCredentialGeneration < 2
          || stationConfiguration.controlCredentialGeneration()
              != controlCredentialGeneration) {
        throw new IllegalArgumentException("control credential rotation result is invalid");
      }
    }
  }

  /** Authenticates, validates, generates, and persists one rotation as a single generation. */
  public synchronized ControlCredentialRotation rotateControlCredential(
      String authorizationHeader,
      long expectedRevision,
      String confirmedNodeId,
      boolean setupEditable) {
    return NodeConfigurationProcessLock.call(
        () -> {
          String identity = nodeId();
          ControlCredentialSnapshot currentCredential = ensureControlCredential();
          Map<String, ?> before = preferences.getAll();
          long currentRevision = revisionValue(before);
          ControlCredentialRotationPolicy.Plan plan =
              ControlCredentialRotationPolicy.authorizeAndPlan(
                  authorizationHeader,
                  currentCredential.token(),
                  currentCredential.generation(),
                  currentRevision,
                  expectedRevision,
                  identity,
                  confirmedNodeId,
                  setupEditable,
                  () -> BearerAuthorization.generate(new SecureRandom()));
          if (!preferences
              .edit()
              .putString(CONTROL_TOKEN, plan.nextToken())
              .putLong(CONTROL_TOKEN_GENERATION, plan.nextCredentialGeneration())
              .putLong(SETUP_REVISION, plan.nextSetupRevision())
              .commit()) {
            throw new IllegalStateException("Unable to persist rotated control credential");
          }
          StationConfiguration committed = stationConfiguration(preferences.getAll());
          return new ControlCredentialRotation(
              plan.nextToken(), plan.nextCredentialGeneration(), committed);
        });
  }

  private record ControlCredentialSnapshot(String token, long generation) {}

  private ControlCredentialSnapshot ensureControlCredential() {
    Map<String, ?> values = preferences.getAll();
    String existing = stringValue(values, CONTROL_TOKEN, null);
    Object storedGeneration = values.get(CONTROL_TOKEN_GENERATION);
    long generation =
        storedGeneration instanceof Long value && value > 0
            ? value
            : 1;
    if (BearerAuthorization.isValidToken(existing)) {
      if (!(storedGeneration instanceof Long value) || value < 1) {
        if (!preferences.edit().putLong(CONTROL_TOKEN_GENERATION, generation).commit()) {
          throw new IllegalStateException("Unable to persist control credential generation");
        }
      }
      return new ControlCredentialSnapshot(existing, generation);
    }
    if (storedGeneration instanceof Long value && value > 0) {
      if (value == Long.MAX_VALUE) {
        throw new IllegalStateException("Control credential generation is exhausted");
      }
      generation = value + 1;
    }
    String generated = BearerAuthorization.generate(new SecureRandom());
    if (!preferences
        .edit()
        .putString(CONTROL_TOKEN, generated)
        .putLong(CONTROL_TOKEN_GENERATION, generation)
        .commit()) {
      throw new IllegalStateException("Unable to persist control credential");
    }
    return new ControlCredentialSnapshot(generated, generation);
  }

  private static String stringValue(Map<String, ?> values, String key, String fallback) {
    Object value = values.get(key);
    return value instanceof String string ? string : fallback;
  }

  private static double doubleValue(Map<String, ?> values, String key, double fallback) {
    String value = stringValue(values, key, null);
    if (value == null) {
      return fallback;
    }
    try {
      return Double.parseDouble(value);
    } catch (NumberFormatException malformed) {
      throw new IllegalStateException("Stored pose region is malformed", malformed);
    }
  }

  private static boolean booleanValue(Map<String, ?> values, String key, boolean fallback) {
    Object value = values.get(key);
    return value instanceof Boolean stored ? stored : fallback;
  }

  private static long requiredLong(Map<String, ?> values, String key) {
    Object value = values.get(key);
    if (!(value instanceof Long stored)) {
      throw new IllegalArgumentException(key + " is missing");
    }
    return stored;
  }

  private static long revisionValue(Map<String, ?> values) {
    Object stored = values.get(SETUP_REVISION);
    if (stored == null) {
      return 0;
    }
    if (!(stored instanceof Long revision) || revision < 0) {
      throw new IllegalStateException("Stored setup revision is malformed");
    }
    return revision;
  }

  private static long controlCredentialGenerationValue(Map<String, ?> values) {
    Object stored = values.get(CONTROL_TOKEN_GENERATION);
    if (!(stored instanceof Long generation) || generation < 1) {
      throw new IllegalStateException("Stored control credential generation is malformed");
    }
    return generation;
  }

  private static long nextRevision(long revision) {
    if (revision == Long.MAX_VALUE) {
      throw new IllegalStateException("Setup revision is exhausted");
    }
    return revision + 1;
  }
}
