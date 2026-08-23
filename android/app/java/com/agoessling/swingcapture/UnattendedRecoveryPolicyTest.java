package com.agoessling.swingcapture;

/** OS-reboot boundary and current-boot readiness coverage. */
public final class UnattendedRecoveryPolicyTest {
  private UnattendedRecoveryPolicyTest() {}

  public static void main(String[] arguments) {
    check(
        UnattendedRecoveryPolicy.MODE.equals("operator_launch_after_first_unlock"),
        "selected reboot mode");
    check(!UnattendedRecoveryPolicy.directBootAware(), "credential-encrypted app boundary");
    check(UnattendedRecoveryPolicy.bootReceiverRegistered(), "boot marker receiver is explicit");
    check(
        !UnattendedRecoveryPolicy.automaticCaptureBeforeFirstUnlock(),
        "camera and microphone never claim direct-boot readiness");
    check(
        UnattendedRecoveryPolicy.operatorForegroundLaunchRequiredAfterOsReboot(),
        "foreground launch is explicit");
    check(
        UnattendedRecoveryPolicy.bootEvent(
                UnattendedRecoveryPolicy.ACTION_LOCKED_BOOT_COMPLETED)
            .orElseThrow()
            .equals("locked_boot_completed"),
        "locked boot event classification");
    check(
        UnattendedRecoveryPolicy.bootEvent(UnattendedRecoveryPolicy.ACTION_BOOT_COMPLETED)
            .orElseThrow()
            .equals("boot_completed"),
        "credential-unlock boot event classification");
    check(
        UnattendedRecoveryPolicy.bootEvent(UnattendedRecoveryPolicy.ACTION_USER_UNLOCKED)
            .orElseThrow()
            .equals("user_unlocked"),
        "unlock event classification");
    check(
        UnattendedRecoveryPolicy.bootEvent("untrusted.action").isEmpty(),
        "unrecognized broadcast is ignored");
    check(!UnattendedRecoveryPolicy.bootEventMayStartCapture(), "boot marker cannot start capture");

    UnattendedRecoveryPolicy.Status beforeUnlock =
        UnattendedRecoveryPolicy.status(false, false, false, false);
    check(!beforeUnlock.readyThisBoot(), "locked boot is not capture-ready");
    check(beforeUnlock.issues().size() == 3, "all distinct operator actions are retained");

    UnattendedRecoveryPolicy.Status unlockedNotLaunched =
        UnattendedRecoveryPolicy.status(true, true, true, false);
    check(!unlockedNotLaunched.readyThisBoot(), "unlock alone does not start the service");
    check(
        unlockedNotLaunched.issues().equals(
            java.util.List.of("Open Swing Capture once after unlock to start the node service.")),
        "post-unlock foreground action is precise");

    UnattendedRecoveryPolicy.Status running =
        UnattendedRecoveryPolicy.status(true, true, true, true);
    check(running.readyThisBoot(), "running post-unlock service is ready this boot");
    check(running.issues().isEmpty(), "ready state has no issue");

    UnattendedRecoveryPolicy.Status missingPermission =
        UnattendedRecoveryPolicy.status(true, false, true, true);
    check(!missingPermission.readyThisBoot(), "revoked camera permission blocks readiness");
    check(missingPermission.issues().size() == 1, "permissions share one operator action");
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
