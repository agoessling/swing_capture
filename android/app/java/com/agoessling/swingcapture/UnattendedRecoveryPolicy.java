package com.agoessling.swingcapture;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/** Product policy for the Android OS-reboot boundary and current-boot readiness. */
final class UnattendedRecoveryPolicy {
  static final String MODE = "operator_launch_after_first_unlock";
  static final String PROCESS_RESTART_POLICY = "android_start_sticky_best_effort";
  static final String ACTION_LOCKED_BOOT_COMPLETED =
      "android.intent.action.LOCKED_BOOT_COMPLETED";
  static final String ACTION_BOOT_COMPLETED = "android.intent.action.BOOT_COMPLETED";
  static final String ACTION_USER_UNLOCKED = "android.intent.action.USER_UNLOCKED";

  record Status(
      boolean userUnlocked,
      boolean cameraPermission,
      boolean audioPermission,
      boolean serviceRunning,
      List<String> issues) {
    Status {
      issues = List.copyOf(issues);
    }

    boolean readyThisBoot() {
      return issues.isEmpty();
    }
  }

  private UnattendedRecoveryPolicy() {}

  static Status status(
      boolean userUnlocked,
      boolean cameraPermission,
      boolean audioPermission,
      boolean serviceRunning) {
    ArrayList<String> issues = new ArrayList<>();
    if (!userUnlocked) {
      issues.add("Unlock this phone once after the OS reboot.");
    }
    if (!cameraPermission || !audioPermission) {
      issues.add("Grant camera and microphone permission from the foreground app.");
    }
    if (!serviceRunning) {
      issues.add("Open Swing Capture once after unlock to start the node service.");
    }
    return new Status(
        userUnlocked, cameraPermission, audioPermission, serviceRunning, issues);
  }

  static boolean directBootAware() {
    return false;
  }

  static boolean bootReceiverRegistered() {
    return true;
  }

  static boolean automaticCaptureBeforeFirstUnlock() {
    return false;
  }

  static boolean operatorForegroundLaunchRequiredAfterOsReboot() {
    return true;
  }

  static Optional<String> bootEvent(String action) {
    return switch (action == null ? "" : action) {
      case ACTION_LOCKED_BOOT_COMPLETED -> Optional.of("locked_boot_completed");
      case ACTION_BOOT_COMPLETED -> Optional.of("boot_completed");
      case ACTION_USER_UNLOCKED -> Optional.of("user_unlocked");
      default -> Optional.empty();
    };
  }

  static boolean bootEventMayStartCapture() {
    return false;
  }
}
