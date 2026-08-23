package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.os.PowerManager;
import android.os.StatFs;
import android.util.Range;
import android.util.Size;
import java.time.Instant;
import java.util.Arrays;
import java.util.Comparator;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Inventories capabilities needed by the Android capture acceptance gate. */
public final class CapabilityInventory {
  private static final int AUDIO_SAMPLE_RATE_HZ = 48_000;

  private CapabilityInventory() {}

  public static JSONObject collect(Context context, NodeConfiguration configuration)
      throws Exception {
    CaptureProfile selectedProfile = configuration.captureProfile();
    DeviceCapabilityPolicy.HardwareSnapshot productHardware =
        AndroidDeviceCapabilityProbe.collectOrUnavailable(context);
    JSONObject report = new JSONObject();
    report.put("schema_version", 1);
    report.put("report_type", "android_capability_inventory");
    report.put("created_at_utc", Instant.now().toString());
    report.put("node_id", configuration.nodeId());
    report.put("role", configuration.role().wireName());
    report.put("capture_profile", selectedProfile.wireName());
    report.put("device", deviceJson());
    report.put("permissions", permissionJson(context));
    report.put("power", powerJson(context));
    report.put("storage", storageJson(context));
    report.put("audio", audioJson());
    report.put("encoders", encoderJson());
    report.put("cameras", cameraJson(context));
    report.put("product_floor_assessment", productFloorJson(productHardware, selectedProfile));
    return report;
  }

  private static JSONObject productFloorJson(
      DeviceCapabilityPolicy.HardwareSnapshot hardware, CaptureProfile profile) throws Exception {
    DeviceCapabilityPolicy.Assessment assessment = DeviceCapabilityPolicy.assess(hardware, profile);
    DeviceCapabilityPolicy.InferenceContract inference =
        DeviceCapabilityPolicy.inferenceContract();
    JSONArray issues = new JSONArray();
    for (DeviceCapabilityPolicy.Issue issue : assessment.issues()) {
      issues.put(
          new JSONObject()
              .put("code", issue.code().name().toLowerCase(java.util.Locale.ROOT))
              .put("message", issue.message()));
    }
    return new JSONObject()
        .put("schema_version", 1)
        .put("minimum_api_level", DeviceCapabilityPolicy.MINIMUM_API_LEVEL)
        .put("selected_profile", profile.wireName())
        .put("ready", assessment.ready())
        .put("probe_succeeded", hardware.probeSucceeded())
        .put("probe_diagnostic", hardware.probeSucceeded() ? JSONObject.NULL : hardware.probeFailure())
        .put(
            "measured",
            new JSONObject()
                .put("api_level", hardware.apiLevel())
                .put("camera_permission", hardware.cameraPermission())
                .put("audio_permission", hardware.audioPermission())
                .put("rear_realtime_camera_720p240", hardware.rearRealtimeCamera720p240())
                .put("rear_realtime_camera_1080p240", hardware.rearRealtimeCamera1080p240())
                .put("hardware_avc_720p240", hardware.hardwareAvc720p240())
                .put("hardware_avc_1080p240", hardware.hardwareAvc1080p240())
                .put("pcm16_mono_48khz", hardware.pcm16Mono48Khz())
                .put(
                    "required_opengl_es_version_hex",
                    String.format("0x%08x", hardware.requiredOpenGlEsVersion())))
        .put(
            "production_pose",
            new JSONObject()
                .put("model", inference.model())
                .put("input_width", inference.inputWidth())
                .put("input_height", inference.inputHeight())
                .put("cadence_hz", inference.cadenceHz())
                .put("delegate_selection_scope", inference.delegateSelectionScope())
                .put(
                    "device_fallback_can_affect_peer",
                    inference.deviceFallbackCanAffectPeer()))
        .put("issues", issues);
  }

  private static JSONObject deviceJson() throws JSONException {
    JSONObject device = new JSONObject();
    device.put("manufacturer", Build.MANUFACTURER);
    device.put("brand", Build.BRAND);
    device.put("model", Build.MODEL);
    device.put("device", Build.DEVICE);
    device.put("product", Build.PRODUCT);
    device.put("fingerprint", Build.FINGERPRINT);
    device.put("android_release", Build.VERSION.RELEASE);
    device.put("api_level", Build.VERSION.SDK_INT);
    device.put("security_patch", Build.VERSION.SECURITY_PATCH);
    return device;
  }

  private static JSONObject permissionJson(Context context) throws JSONException {
    JSONObject permissions = new JSONObject();
    permissions.put("camera", granted(context, Manifest.permission.CAMERA));
    permissions.put("record_audio", granted(context, Manifest.permission.RECORD_AUDIO));
    permissions.put(
        "post_notifications", granted(context, Manifest.permission.POST_NOTIFICATIONS));
    return permissions;
  }

  private static boolean granted(Context context, String permission) {
    return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
  }

  private static JSONObject powerJson(Context context) throws JSONException {
    PowerManager manager = context.getSystemService(PowerManager.class);
    JSONObject power = new JSONObject();
    power.put("interactive", manager.isInteractive());
    power.put("power_save", manager.isPowerSaveMode());
    power.put("thermal_status", manager.getCurrentThermalStatus());
    return power;
  }

  private static JSONObject storageJson(Context context) throws JSONException {
    StatFs stat = new StatFs(context.getFilesDir().getAbsolutePath());
    JSONObject storage = new JSONObject();
    storage.put("available_bytes", stat.getAvailableBytes());
    storage.put("total_bytes", stat.getTotalBytes());
    return storage;
  }

  private static JSONObject audioJson() throws JSONException {
    JSONObject audio = new JSONObject();
    audio.put("sample_rate_hz", AUDIO_SAMPLE_RATE_HZ);
    audio.put("channel_mask", AudioFormat.CHANNEL_IN_MONO);
    audio.put("encoding", AudioFormat.ENCODING_PCM_16BIT);
    audio.put(
        "minimum_buffer_bytes",
        AudioRecord.getMinBufferSize(
            AUDIO_SAMPLE_RATE_HZ, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT));
    return audio;
  }

  private static JSONArray encoderJson() throws JSONException {
    JSONArray encoders = new JSONArray();
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (!info.isEncoder()) {
        continue;
      }
      for (String type : info.getSupportedTypes()) {
        if (!MediaFormat.MIMETYPE_VIDEO_HEVC.equalsIgnoreCase(type)
            && !MediaFormat.MIMETYPE_VIDEO_AVC.equalsIgnoreCase(type)) {
          continue;
        }
        JSONObject encoder = new JSONObject();
        encoder.put("name", info.getName());
        encoder.put("canonical_name", info.getCanonicalName());
        encoder.put("hardware_accelerated", info.isHardwareAccelerated());
        encoder.put("software_only", info.isSoftwareOnly());
        encoder.put("vendor", info.isVendor());
        encoder.put("mime", type);
        MediaCodecInfo.VideoCapabilities video = info.getCapabilitiesForType(type).getVideoCapabilities();
        encoder.put("width_alignment", video.getWidthAlignment());
        encoder.put("height_alignment", video.getHeightAlignment());
        encoder.put("supports_720p240", supports(video, 1280, 720, 240.0));
        encoder.put("supports_1080p240", supports(video, 1920, 1080, 240.0));
        encoders.put(encoder);
      }
    }
    return encoders;
  }

  private static boolean supports(
      MediaCodecInfo.VideoCapabilities capabilities, int width, int height, double rate) {
    try {
      return capabilities.areSizeAndRateSupported(width, height, rate);
    } catch (IllegalArgumentException unsupported) {
      return false;
    }
  }

  private static JSONArray cameraJson(Context context) throws Exception {
    CameraManager manager = context.getSystemService(CameraManager.class);
    JSONArray cameras = new JSONArray();
    for (String cameraId : manager.getCameraIdList()) {
      CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
      JSONObject camera = new JSONObject();
      camera.put("id", cameraId);
      Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
      camera.put("lens_facing", facing == null ? JSONObject.NULL : facing);
      Integer timestampSource =
          characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
      camera.put("timestamp_source", timestampSource == null ? JSONObject.NULL : timestampSource);
      int[] capabilities =
          characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
      camera.put("capabilities", integerArray(capabilities));
      camera.put(
          "constrained_high_speed",
          contains(
              capabilities,
              CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO));
      StreamConfigurationMap streams =
          characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
      camera.put("high_speed_profiles", highSpeedProfiles(streams));
      cameras.put(camera);
    }
    return cameras;
  }

  private static JSONArray highSpeedProfiles(StreamConfigurationMap streams) throws JSONException {
    JSONArray profiles = new JSONArray();
    if (streams == null) {
      return profiles;
    }
    Size[] sizes = streams.getHighSpeedVideoSizes();
    Arrays.sort(
        sizes,
        Comparator.comparingInt(Size::getWidth).thenComparingInt(Size::getHeight));
    for (Size size : sizes) {
      Range<Integer>[] ranges = streams.getHighSpeedVideoFpsRangesFor(size);
      Arrays.sort(
          ranges,
          Comparator.comparingInt((Range<Integer> range) -> range.getLower())
              .thenComparingInt(range -> range.getUpper()));
      for (Range<Integer> range : ranges) {
        JSONObject profile = new JSONObject();
        profile.put("width", size.getWidth());
        profile.put("height", size.getHeight());
        profile.put("minimum_fps", range.getLower());
        profile.put("maximum_fps", range.getUpper());
        profile.put("fixed", range.getLower().equals(range.getUpper()));
        profiles.put(profile);
      }
    }
    return profiles;
  }

  private static JSONArray integerArray(int[] values) {
    JSONArray array = new JSONArray();
    if (values != null) {
      for (int value : values) {
        array.put(value);
      }
    }
    return array;
  }

  private static boolean contains(int[] values, int expected) {
    if (values == null) {
      return false;
    }
    for (int value : values) {
      if (value == expected) {
        return true;
      }
    }
    return false;
  }
}
