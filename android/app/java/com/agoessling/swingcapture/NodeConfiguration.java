package com.agoessling.swingcapture;

import android.content.Context;
import android.content.SharedPreferences;
import com.agoessling.swingcapture.node.BearerAuthorization;
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

  private final SharedPreferences preferences;

  public NodeConfiguration(Context context) {
    preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
  }

  public String nodeId() {
    String existing = preferences.getString(NODE_ID, null);
    if (existing != null) {
      return existing;
    }
    String generated = UUID.randomUUID().toString();
    if (!preferences.edit().putString(NODE_ID, generated).commit()) {
      throw new IllegalStateException("Unable to persist node identity");
    }
    return generated;
  }

  public CaptureRole role() {
    return CaptureRole.parse(preferences.getString(CAPTURE_ROLE, null));
  }

  public CaptureProfile captureProfile() {
    return CaptureProfile.parse(preferences.getString(CAPTURE_PROFILE, null));
  }

  /** Reads the persistent capture identity from one SharedPreferences generation. */
  public CaptureConfigurationSnapshot captureSnapshot() {
    nodeId();
    Map<String, ?> values = preferences.getAll();
    Object storedNodeId = values.get(NODE_ID);
    Object storedRole = values.get(CAPTURE_ROLE);
    Object storedProfile = values.get(CAPTURE_PROFILE);
    if (!(storedNodeId instanceof String identity)) {
      throw new IllegalStateException("Persistent node identity is unavailable");
    }
    return new CaptureConfigurationSnapshot(
        identity,
        CaptureRole.parse(storedRole instanceof String role ? role : null),
        CaptureProfile.parse(storedProfile instanceof String profile ? profile : null));
  }

  /** Atomically changes the two user-editable capture fields. */
  public void setCaptureConfiguration(CaptureRole role, CaptureProfile profile) {
    if (!preferences
        .edit()
        .putString(CAPTURE_ROLE, role.wireName())
        .putString(CAPTURE_PROFILE, profile.wireName())
        .commit()) {
      throw new IllegalStateException("Unable to persist capture role and profile");
    }
  }

  /** Secret shown only on the phone and required for state-changing network requests. */
  public String controlToken() {
    String existing = preferences.getString(CONTROL_TOKEN, null);
    if (BearerAuthorization.isValidToken(existing)) {
      return existing;
    }
    String generated = BearerAuthorization.generate(new SecureRandom());
    if (!preferences.edit().putString(CONTROL_TOKEN, generated).commit()) {
      throw new IllegalStateException("Unable to persist control credential");
    }
    return generated;
  }
}
