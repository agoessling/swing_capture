package com.agoessling.swingcapture;

public final class StandbyDiagnosticSessionRegistryTest {
  public static void main(String[] args) {
    registersClaimsAndDiscardsExactlyOnce();
    rejectsDuplicateAndUnboundedRegistrations();
    validatesIdentity();
  }

  private static void registersClaimsAndDiscardsExactlyOnce() {
    StandbyDiagnosticSessionRegistry registry = new StandbyDiagnosticSessionRegistry();
    StandbyDiagnosticSessionRegistry.PendingSession first =
        new StandbyDiagnosticSessionRegistry.PendingSession("session-1", "node-1", 1234);
    registry.register(1, first);
    check(registry.size() == 1, "registration should be visible");
    check(registry.claim(1).orElseThrow().equals(first), "claim should preserve identity");
    check(registry.claim(1).isEmpty(), "claim must be exactly once");

    registry.register(
        2, new StandbyDiagnosticSessionRegistry.PendingSession("session-2", "node-1", 1235));
    registry.discard(2);
    check(registry.size() == 0, "discard should remove registration");
  }

  private static void rejectsDuplicateAndUnboundedRegistrations() {
    StandbyDiagnosticSessionRegistry registry = new StandbyDiagnosticSessionRegistry();
    for (int index = 1;
        index <= StandbyDiagnosticSessionRegistry.MAXIMUM_PENDING_SESSIONS;
        ++index) {
      registry.register(
          index,
          new StandbyDiagnosticSessionRegistry.PendingSession(
              "session-" + index, "node-1", 2_000L + index));
    }
    expectFailure(
        () ->
            registry.register(
                1,
                new StandbyDiagnosticSessionRegistry.PendingSession(
                    "duplicate", "node-1", 3_000)),
        "registered twice");
    expectFailure(
        () ->
            registry.register(
                StandbyDiagnosticSessionRegistry.MAXIMUM_PENDING_SESSIONS + 1L,
                new StandbyDiagnosticSessionRegistry.PendingSession("overflow", "node-1", 3_001)),
        "registry is full");
    check(
        registry.clear() == StandbyDiagnosticSessionRegistry.MAXIMUM_PENDING_SESSIONS,
        "clear should report every removed registration");
    check(registry.size() == 0, "clear should remove every registration");
  }

  private static void validatesIdentity() {
    expectFailure(
        () -> new StandbyDiagnosticSessionRegistry.PendingSession("bad/id", "node", 1),
        "sessionId is invalid");
    expectFailure(
        () -> new StandbyDiagnosticSessionRegistry.PendingSession("session", "node", 0),
        "must be positive");
    expectFailure(
        () ->
            new StandbyDiagnosticSessionRegistry()
                .register(
                    0,
                    new StandbyDiagnosticSessionRegistry.PendingSession(
                        "session", "node", 1)),
        "eventSequence must be positive");
  }

  private static void expectFailure(Runnable action, String expectedMessage) {
    try {
      action.run();
      throw new AssertionError("Expected failure containing: " + expectedMessage);
    } catch (IllegalArgumentException | IllegalStateException expected) {
      check(
          expected.getMessage().contains(expectedMessage),
          "unexpected failure: " + expected.getMessage());
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }
}
