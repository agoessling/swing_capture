package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** Pure product-floor admission policy over one phone's locally measured capabilities. */
final class DeviceCapabilityPolicy {
  static final int MINIMUM_API_LEVEL = 34;
  static final int MINIMUM_OPENGL_ES_VERSION = 0x00030001;
  static final int POSE_INPUT_WIDTH = 640;
  static final int POSE_INPUT_HEIGHT = 360;
  static final int POSE_CADENCE_HZ = 5;
  static final String POSE_MODEL = "lite";

  enum IssueCode {
    API_LEVEL,
    CAMERA_PERMISSION,
    AUDIO_PERMISSION,
    CAMERA_PROFILE,
    HARDWARE_AVC,
    PCM_AUDIO,
    GPU_INFERENCE_PLATFORM,
    PROBE_FAILED
  }

  record HardwareSnapshot(
      int apiLevel,
      boolean cameraPermission,
      boolean audioPermission,
      boolean rearRealtimeCamera720p240,
      boolean rearRealtimeCamera1080p240,
      boolean hardwareAvc720p240,
      boolean hardwareAvc1080p240,
      boolean pcm16Mono48Khz,
      int requiredOpenGlEsVersion,
      String probeFailure) {
    HardwareSnapshot {
      if (apiLevel < 1) {
        throw new IllegalArgumentException("apiLevel must be positive");
      }
      if (requiredOpenGlEsVersion < 0) {
        throw new IllegalArgumentException("requiredOpenGlEsVersion cannot be negative");
      }
      Objects.requireNonNull(probeFailure, "probeFailure");
    }

    static HardwareSnapshot unavailable(
        int apiLevel,
        boolean cameraPermission,
        boolean audioPermission,
        int requiredOpenGlEsVersion,
        String probeFailure) {
      if (probeFailure == null || probeFailure.isBlank()) {
        throw new IllegalArgumentException("an unavailable probe requires a diagnostic");
      }
      return new HardwareSnapshot(
          apiLevel,
          cameraPermission,
          audioPermission,
          false,
          false,
          false,
          false,
          false,
          requiredOpenGlEsVersion,
          probeFailure);
    }

    boolean probeSucceeded() {
      return probeFailure.isEmpty();
    }

    /** Applies current runtime grants without repeating the static camera/codec hardware probe. */
    HardwareSnapshot withPermissions(
        boolean currentCameraPermission, boolean currentAudioPermission) {
      return new HardwareSnapshot(
          apiLevel,
          currentCameraPermission,
          currentAudioPermission,
          rearRealtimeCamera720p240,
          rearRealtimeCamera1080p240,
          hardwareAvc720p240,
          hardwareAvc1080p240,
          pcm16Mono48Khz,
          requiredOpenGlEsVersion,
          probeFailure);
    }
  }

  record Issue(IssueCode code, String message) {
    Issue {
      Objects.requireNonNull(code, "code");
      if (message == null || message.isBlank()) {
        throw new IllegalArgumentException("capability issue requires a message");
      }
    }
  }

  record Assessment(CaptureProfile profile, List<Issue> issues) {
    Assessment {
      Objects.requireNonNull(profile, "profile");
      issues = List.copyOf(issues);
    }

    boolean ready() {
      return issues.isEmpty();
    }

    void requireReady() {
      if (!ready()) {
        throw new IllegalStateException(
            "This phone does not meet the "
                + profile.wireName()
                + " product floor: "
                + issues.get(0).message());
      }
    }
  }

  record InferenceContract(
      String model,
      int inputWidth,
      int inputHeight,
      int cadenceHz,
      String delegateSelectionScope,
      boolean deviceFallbackCanAffectPeer) {}

  private DeviceCapabilityPolicy() {}

  static Assessment assess(HardwareSnapshot hardware, CaptureProfile profile) {
    Objects.requireNonNull(hardware, "hardware");
    Objects.requireNonNull(profile, "profile");
    ArrayList<Issue> issues = new ArrayList<>();
    if (!hardware.probeSucceeded()) {
      issues.add(
          new Issue(
              IssueCode.PROBE_FAILED,
              "Capability probe failed; inspect the local capability diagnostic before capture."));
    }
    if (hardware.apiLevel() < MINIMUM_API_LEVEL) {
      issues.add(
          new Issue(
              IssueCode.API_LEVEL,
              "Android API " + MINIMUM_API_LEVEL + " or newer is required."));
    }
    if (!hardware.cameraPermission()) {
      issues.add(
          new Issue(IssueCode.CAMERA_PERMISSION, "Grant camera permission before capture."));
    }
    if (!hardware.audioPermission()) {
      issues.add(
          new Issue(IssueCode.AUDIO_PERMISSION, "Grant microphone permission before capture."));
    }
    if (hardware.probeSucceeded()) {
      if (!cameraSupports(hardware, profile)) {
        issues.add(
            new Issue(
                IssueCode.CAMERA_PROFILE,
                "A realtime rear camera with 640x360 YUV standby and fixed "
                    + profile.wireName()
                    + " capture is required."));
      }
      if (!encoderSupports(hardware, profile)) {
        issues.add(
            new Issue(
                IssueCode.HARDWARE_AVC,
                "A hardware H.264 encoder supporting " + profile.wireName() + " is required."));
      }
      if (!hardware.pcm16Mono48Khz()) {
        issues.add(
            new Issue(
                IssueCode.PCM_AUDIO, "48 kHz mono PCM16 AudioRecord input is required."));
      }
    }
    if (hardware.requiredOpenGlEsVersion() < MINIMUM_OPENGL_ES_VERSION) {
      issues.add(
          new Issue(
              IssueCode.GPU_INFERENCE_PLATFORM,
              "OpenGL ES 3.1 or newer is required for the production pose GPU path."));
    }
    return new Assessment(profile, issues);
  }

  static InferenceContract inferenceContract() {
    return new InferenceContract(
        POSE_MODEL,
        POSE_INPUT_WIDTH,
        POSE_INPUT_HEIGHT,
        POSE_CADENCE_HZ,
        "per_node",
        false);
  }

  private static boolean cameraSupports(HardwareSnapshot hardware, CaptureProfile profile) {
    return switch (profile) {
      case HD_240 -> hardware.rearRealtimeCamera720p240();
      case FULL_HD_240 -> hardware.rearRealtimeCamera1080p240();
    };
  }

  private static boolean encoderSupports(HardwareSnapshot hardware, CaptureProfile profile) {
    return switch (profile) {
      case HD_240 -> hardware.hardwareAvc720p240();
      case FULL_HD_240 -> hardware.hardwareAvc1080p240();
    };
  }
}
