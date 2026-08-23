package com.agoessling.swingcapture;

import java.util.List;

/** Minimum-device admission and per-node performance-isolation coverage. */
public final class DeviceCapabilityPolicyTest {
  private DeviceCapabilityPolicyTest() {}

  public static void main(String[] arguments) {
    DeviceCapabilityPolicy.HardwareSnapshot pixel6 = nominal(true, true);
    DeviceCapabilityPolicy.Assessment standard =
        DeviceCapabilityPolicy.assess(pixel6, CaptureProfile.HD_240);
    check(standard.ready(), "nominal 720p240 phone is admitted");
    standard.requireReady();
    check(
        DeviceCapabilityPolicy.assess(pixel6, CaptureProfile.FULL_HD_240).ready(),
        "independently capable phone is admitted for 1080p240");

    DeviceCapabilityPolicy.HardwareSnapshot provisionalPixel5a = nominal(true, false);
    check(
        DeviceCapabilityPolicy.assess(provisionalPixel5a, CaptureProfile.HD_240).ready(),
        "720p240 remains independently admissible on the provisional older phone");
    DeviceCapabilityPolicy.Assessment rejected1080 =
        DeviceCapabilityPolicy.assess(provisionalPixel5a, CaptureProfile.FULL_HD_240);
    check(!rejected1080.ready(), "one phone's missing 1080p240 path is rejected locally");
    check(
        issueCodes(rejected1080).equals(
            List.of(
                DeviceCapabilityPolicy.IssueCode.CAMERA_PROFILE,
                DeviceCapabilityPolicy.IssueCode.HARDWARE_AVC)),
        "profile-specific camera and encoder failures are explicit");
    check(
        DeviceCapabilityPolicy.assess(pixel6, CaptureProfile.FULL_HD_240).ready(),
        "an older phone's rejection cannot mutate the Pixel 6 assessment");

    DeviceCapabilityPolicy.HardwareSnapshot unsupported =
        new DeviceCapabilityPolicy.HardwareSnapshot(
            33, false, false, false, false, false, false, false, 0x00030000, "");
    DeviceCapabilityPolicy.Assessment unsupportedAssessment =
        DeviceCapabilityPolicy.assess(unsupported, CaptureProfile.HD_240);
    check(
        issueCodes(unsupportedAssessment)
            .equals(
                List.of(
                    DeviceCapabilityPolicy.IssueCode.API_LEVEL,
                    DeviceCapabilityPolicy.IssueCode.CAMERA_PERMISSION,
                    DeviceCapabilityPolicy.IssueCode.AUDIO_PERMISSION,
                    DeviceCapabilityPolicy.IssueCode.CAMERA_PROFILE,
                    DeviceCapabilityPolicy.IssueCode.HARDWARE_AVC,
                    DeviceCapabilityPolicy.IssueCode.PCM_AUDIO,
                    DeviceCapabilityPolicy.IssueCode.GPU_INFERENCE_PLATFORM)),
        "every independent product-floor failure is retained");
    expectNotReady(unsupportedAssessment, "unsupported assessment");

    DeviceCapabilityPolicy.HardwareSnapshot failedProbe =
        DeviceCapabilityPolicy.HardwareSnapshot.unavailable(
            36, true, true, 0x00030002, "CameraAccessException");
    DeviceCapabilityPolicy.Assessment failedAssessment =
        DeviceCapabilityPolicy.assess(failedProbe, CaptureProfile.HD_240);
    check(
        issueCodes(failedAssessment).get(0) == DeviceCapabilityPolicy.IssueCode.PROBE_FAILED,
        "probe failure is the primary actionable reason");
    check(
        failedProbe.probeFailure().equals("CameraAccessException"),
        "bounded local diagnostic is retained outside the operator message");

    DeviceCapabilityPolicy.InferenceContract inference =
        DeviceCapabilityPolicy.inferenceContract();
    check(inference.model().equals("lite"), "production model floor");
    check(inference.inputWidth() == 640 && inference.inputHeight() == 360, "input floor");
    check(inference.cadenceHz() == 5, "production cadence");
    check(inference.delegateSelectionScope().equals("per_node"), "delegate is local");
    check(!inference.deviceFallbackCanAffectPeer(), "fallback cannot cross phone boundary");

    DeviceCapabilityPolicy.HardwareSnapshot revoked = pixel6.withPermissions(false, true);
    check(
        issueCodes(DeviceCapabilityPolicy.assess(revoked, CaptureProfile.HD_240))
            .equals(List.of(DeviceCapabilityPolicy.IssueCode.CAMERA_PERMISSION)),
        "live camera permission overrides the startup grant");
    check(revoked.rearRealtimeCamera720p240(), "static camera probe is retained");
    check(revoked.hardwareAvc720p240(), "static encoder probe is retained");
    DeviceCapabilityPolicy.HardwareSnapshot restored = revoked.withPermissions(true, true);
    check(
        DeviceCapabilityPolicy.assess(restored, CaptureProfile.HD_240).ready(),
        "restoring live permissions does not require another static probe");

    expectInvalid(
        () ->
            DeviceCapabilityPolicy.HardwareSnapshot.unavailable(
                36, true, true, 0x00030002, ""),
        "empty failed-probe diagnostic");
  }

  private static DeviceCapabilityPolicy.HardwareSnapshot nominal(
      boolean supports720, boolean supports1080) {
    return new DeviceCapabilityPolicy.HardwareSnapshot(
        36,
        true,
        true,
        supports720,
        supports1080,
        supports720,
        supports1080,
        true,
        0x00030002,
        "");
  }

  private static List<DeviceCapabilityPolicy.IssueCode> issueCodes(
      DeviceCapabilityPolicy.Assessment assessment) {
    return assessment.issues().stream().map(DeviceCapabilityPolicy.Issue::code).toList();
  }

  private static void expectNotReady(
      DeviceCapabilityPolicy.Assessment assessment, String label) {
    try {
      assessment.requireReady();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalStateException expected) {
      check(expected.getMessage().contains("product floor"), label + " diagnostic");
    }
  }

  private static void expectInvalid(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
    } catch (IllegalArgumentException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
