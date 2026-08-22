package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegatePolicy;
import java.net.URI;
import java.util.Locale;
import java.util.Objects;

/** Immutable low-rate pose, evidence, and optional peer-arm configuration. */
public record PoseStationConfigurationSnapshot(
    PoseNodeMode mode,
    PoseInferenceDelegatePolicy delegatePolicy,
    NormalizedHittingRegion hittingRegion,
    boolean debugEvidenceEnabled,
    String peerOrigin,
    String peerControlToken) {
  public static final NormalizedHittingRegion DEFAULT_HITTING_REGION =
      new NormalizedHittingRegion(0.15, 0.30, 0.85, 1.0);

  public PoseStationConfigurationSnapshot {
    Objects.requireNonNull(mode, "mode");
    Objects.requireNonNull(delegatePolicy, "delegatePolicy");
    Objects.requireNonNull(hittingRegion, "hittingRegion");
    peerOrigin = Objects.requireNonNull(peerOrigin, "peerOrigin").trim();
    peerControlToken = Objects.requireNonNull(peerControlToken, "peerControlToken").trim();
    if (peerOrigin.isEmpty() != peerControlToken.isEmpty()) {
      throw new IllegalArgumentException("peer origin and control token must be configured together");
    }
    if (!peerOrigin.isEmpty()) {
      validatePeerOrigin(peerOrigin);
      if (!BearerAuthorization.isValidToken(peerControlToken)) {
        throw new IllegalArgumentException("peer control token is invalid");
      }
    }
  }

  public static PoseStationConfigurationSnapshot defaults() {
    return new PoseStationConfigurationSnapshot(
        PoseNodeMode.DISABLED,
        PoseInferenceDelegatePolicy.GPU_PREFERRED,
        DEFAULT_HITTING_REGION,
        false,
        "",
        "");
  }

  public boolean hasPeer() {
    return !peerOrigin.isEmpty();
  }

  public String delegateWireName() {
    return delegatePolicy.name().toLowerCase(Locale.ROOT);
  }

  public static PoseInferenceDelegatePolicy parseDelegatePolicy(String value) {
    if (value == null || value.isBlank()) {
      return PoseInferenceDelegatePolicy.GPU_PREFERRED;
    }
    try {
      return PoseInferenceDelegatePolicy.valueOf(value.trim().toUpperCase(Locale.ROOT));
    } catch (IllegalArgumentException invalid) {
      throw new IllegalArgumentException("Unknown pose delegate policy: " + value, invalid);
    }
  }

  private static void validatePeerOrigin(String value) {
    URI origin;
    try {
      origin = URI.create(value);
    } catch (IllegalArgumentException invalid) {
      throw new IllegalArgumentException("peer origin is not a valid URI", invalid);
    }
    if (!"http".equals(origin.getScheme())
        || origin.getHost() == null
        || origin.getUserInfo() != null
        || origin.getQuery() != null
        || origin.getFragment() != null
        || !(origin.getPath().isEmpty() || origin.getPath().equals("/"))
        || origin.getPort() <= 0
        || origin.getPort() > 65_535) {
      throw new IllegalArgumentException("peer origin must be http://host:port with no path");
    }
  }
}
