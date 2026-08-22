package com.agoessling.swingcapture;

import android.content.Context;
import android.content.SharedPreferences;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import java.security.SecureRandom;
import java.util.Map;
import java.util.UUID;

/** Persistent per-installation identity, camera role, and capture profile. */
public final class NodeConfiguration {
  private static final String PREFERENCES = "node_configuration";
  private static final String NODE_ID = "node_id";
  private static final String CAPTURE_ROLE = "capture_role";
  private static final String CAPTURE_PROFILE = "capture_profile";
  private static final String CONTROL_TOKEN = "control_token";
  private static final String POSE_MODE = "pose_mode";
  private static final String POSE_DELEGATE = "pose_delegate";
  private static final String POSE_REGION_LEFT = "pose_region_left";
  private static final String POSE_REGION_TOP = "pose_region_top";
  private static final String POSE_REGION_RIGHT = "pose_region_right";
  private static final String POSE_REGION_BOTTOM = "pose_region_bottom";
  private static final String POSE_DEBUG_EVIDENCE = "pose_debug_evidence";
  private static final String POSE_PEER_ORIGIN = "pose_peer_origin";
  private static final String POSE_PEER_CONTROL_TOKEN = "pose_peer_control_token";
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
      CaptureConfigurationSnapshot capture,
      PoseStationConfigurationSnapshot pose) {
    public StationConfiguration {
      if (revision < 0) {
        throw new IllegalArgumentException("setup revision cannot be negative");
      }
      java.util.Objects.requireNonNull(capture, "capture");
      java.util.Objects.requireNonNull(pose, "pose");
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
    return new StationConfiguration(revisionValue(values), capture, poseConfiguration(values));
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
          long nextRevision = nextRevision(revisionValue(preferences.getAll()));
          SharedPreferences.Editor editor = preferences.edit();
          putPoseConfiguration(editor, configuration);
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
          long committedRevision = nextRevision(revisionValue(preferences.getAll()));
          SharedPreferences.Editor editor =
              preferences
                  .edit()
                  .putString(CAPTURE_ROLE, role.wireName())
                  .putString(CAPTURE_PROFILE, profile.wireName());
          putPoseConfiguration(editor, pose);
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
      PoseStationConfigurationSnapshot pose) {
    if (expectedRevision < 0) {
      throw new IllegalArgumentException("expected_revision cannot be negative");
    }
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
          if (!editor.putLong(SETUP_REVISION, committedRevision).commit()) {
            throw new IllegalStateException("Unable to persist station setup");
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

  /** Secret shown only on the phone and required for state-changing network requests. */
  public synchronized String controlToken() {
    return NodeConfigurationProcessLock.call(
        () -> {
          String existing = preferences.getString(CONTROL_TOKEN, null);
          if (BearerAuthorization.isValidToken(existing)) {
            return existing;
          }
          String generated = BearerAuthorization.generate(new SecureRandom());
          if (!preferences.edit().putString(CONTROL_TOKEN, generated).commit()) {
            throw new IllegalStateException("Unable to persist control credential");
          }
          return generated;
        });
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

  private static long nextRevision(long revision) {
    if (revision == Long.MAX_VALUE) {
      throw new IllegalStateException("Setup revision is exhausted");
    }
    return revision + 1;
  }
}
