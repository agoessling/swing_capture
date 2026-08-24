package com.agoessling.swingcapture;

/** Exhaustive host-side checks for pair-network arm admission. */
public final class PairNetworkArmPolicyTest {
  private PairNetworkArmPolicyTest() {}

  public static void main(String[] arguments) {
    completeAdmissionMatrix();
    rejectionMessagesAreStable();
    nullInputsFailClosed();
  }

  private static void completeAdmissionMatrix() {
    for (boolean armed : new boolean[] {false, true}) {
      for (PoseNodeMode mode : PoseNodeMode.values()) {
        for (PairNetworkHealthPolicy.State state : PairNetworkHealthPolicy.State.values()) {
          for (boolean override : new boolean[] {false, true}) {
            boolean expected =
                !armed
                    || mode != PoseNodeMode.LEADER
                    || state == PairNetworkHealthPolicy.State.GOOD
                    || (state == PairNetworkHealthPolicy.State.DEGRADED && override);
            boolean allowed = isAllowed(armed, mode, state, override);
            check(
                allowed == expected,
                "unexpected admission for armed="
                    + armed
                    + ", mode="
                    + mode
                    + ", state="
                    + state
                    + ", override="
                    + override);
          }
        }
      }
    }
  }

  private static void rejectionMessagesAreStable() {
    expectRejection(
        PairNetworkHealthPolicy.State.DEGRADED,
        false,
        PairNetworkArmPolicy.DEGRADED_MESSAGE);
    expectRejection(
        PairNetworkHealthPolicy.State.UNUSABLE,
        false,
        PairNetworkArmPolicy.UNUSABLE_MESSAGE);
    expectRejection(
        PairNetworkHealthPolicy.State.UNUSABLE,
        true,
        PairNetworkArmPolicy.UNUSABLE_MESSAGE);
  }

  private static void nullInputsFailClosed() {
    expectNullPointer(
        () ->
            PairNetworkArmPolicy.requireAllowed(
                true, null, PairNetworkHealthPolicy.State.GOOD, false),
        "null pose mode");
    expectNullPointer(
        () -> PairNetworkArmPolicy.requireAllowed(true, PoseNodeMode.LEADER, null, false),
        "null health state");
  }

  private static boolean isAllowed(
      boolean armed,
      PoseNodeMode mode,
      PairNetworkHealthPolicy.State state,
      boolean override) {
    try {
      PairNetworkArmPolicy.requireAllowed(armed, mode, state, override);
      return true;
    } catch (IllegalStateException rejected) {
      return false;
    }
  }

  private static void expectRejection(
      PairNetworkHealthPolicy.State state, boolean override, String expectedMessage) {
    try {
      PairNetworkArmPolicy.requireAllowed(true, PoseNodeMode.LEADER, state, override);
      throw new AssertionError("expected leader arm request to be rejected");
    } catch (IllegalStateException rejected) {
      check(expectedMessage.equals(rejected.getMessage()), "rejection message changed");
    }
  }

  private static void expectNullPointer(Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError("expected " + label + " to fail");
    } catch (NullPointerException expected) {
      // Expected.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
