package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Queue;

/** Pure coverage for authenticated, confirmed, atomic control-credential rotation planning. */
public final class ControlCredentialRotationPolicyTest {
  private static final String OLD_TOKEN = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  private static final String NEW_TOKEN = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  private ControlCredentialRotationPolicyTest() {}

  public static void main(String[] arguments) {
    acceptsNewAndImmediatelyRejectsOldCredential();
    rejectsUnauthenticatedRotationWithoutGenerating();
    rejectsAbortedAndUnsafeRotationWithoutGenerating();
    rejectsStaleRevisionWithoutGenerating();
    retriesUntilCredentialIsDistinctAndValid();
  }

  private static void acceptsNewAndImmediatelyRejectsOldCredential() {
    QueueGenerator generator = new QueueGenerator(NEW_TOKEN);
    ControlCredentialRotationPolicy.Plan plan =
        ControlCredentialRotationPolicy.authorizeAndPlan(
            "Bearer " + OLD_TOKEN,
            OLD_TOKEN,
            4,
            11,
            11,
            "local-node-id",
            "local-node-id",
            true,
            generator);
    check(plan.nextToken().equals(NEW_TOKEN), "planned new token");
    check(plan.nextCredentialGeneration() == 5, "credential generation advanced once");
    check(plan.nextSetupRevision() == 12, "setup revision advanced once");
    check(
        !BearerAuthorization.accepts("Bearer " + OLD_TOKEN, plan.nextToken()),
        "old token rejected immediately");
    check(
        BearerAuthorization.accepts("Bearer " + NEW_TOKEN, plan.nextToken()),
        "new token accepted immediately");
    check(generator.calls == 1, "one token generated");
  }

  private static void rejectsUnauthenticatedRotationWithoutGenerating() {
    QueueGenerator generator = new QueueGenerator(NEW_TOKEN);
    expectFailure(
        ControlCredentialRotationPolicy.UnauthorizedException.class,
        () ->
            plan(
                "Bearer cccccccccccccccccccccccccccccccc",
                3,
                7,
                7,
                "local-node-id",
                true,
                generator),
        "stale credential");
    check(generator.calls == 0, "unauthorized request generated no token");
  }

  private static void rejectsAbortedAndUnsafeRotationWithoutGenerating() {
    QueueGenerator aborted = new QueueGenerator(NEW_TOKEN);
    expectFailure(
        IllegalArgumentException.class,
        () -> plan("Bearer " + OLD_TOKEN, 3, 7, 7, "wrong-node-id", true, aborted),
        "unconfirmed rotation");
    check(aborted.calls == 0, "unconfirmed request generated no token");

    QueueGenerator unsafe = new QueueGenerator(NEW_TOKEN);
    expectFailure(
        IllegalStateException.class,
        () -> plan("Bearer " + OLD_TOKEN, 3, 7, 7, "local-node-id", false, unsafe),
        "rotation while capture active");
    check(unsafe.calls == 0, "unsafe request generated no token");
  }

  private static void rejectsStaleRevisionWithoutGenerating() {
    QueueGenerator generator = new QueueGenerator(NEW_TOKEN);
    expectFailure(
        ControlCredentialRotationPolicy.StaleRevisionException.class,
        () -> plan("Bearer " + OLD_TOKEN, 3, 8, 7, "local-node-id", true, generator),
        "stale setup revision");
    check(generator.calls == 0, "stale request generated no token");
  }

  private static void retriesUntilCredentialIsDistinctAndValid() {
    QueueGenerator generator = new QueueGenerator("invalid", OLD_TOKEN, NEW_TOKEN);
    ControlCredentialRotationPolicy.Plan plan =
        plan("Bearer " + OLD_TOKEN, 9, 10, 10, "local-node-id", true, generator);
    check(plan.nextToken().equals(NEW_TOKEN), "invalid and duplicate candidates skipped");
    check(generator.calls == 3, "all candidate attempts observed");
  }

  private static ControlCredentialRotationPolicy.Plan plan(
      String authorizationHeader,
      long generation,
      long revision,
      long expectedRevision,
      String confirmation,
      boolean editable,
      ControlCredentialRotationPolicy.TokenGenerator generator) {
    return ControlCredentialRotationPolicy.authorizeAndPlan(
        authorizationHeader,
        OLD_TOKEN,
        generation,
        revision,
        expectedRevision,
        "local-node-id",
        confirmation,
        editable,
        generator);
  }

  private static void expectFailure(
      Class<? extends Throwable> expected, Runnable operation, String label) {
    try {
      operation.run();
      throw new AssertionError(label + " did not fail");
    } catch (Throwable failure) {
      if (!expected.isInstance(failure)) {
        throw new AssertionError(label + " failed with " + failure, failure);
      }
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static final class QueueGenerator
      implements ControlCredentialRotationPolicy.TokenGenerator {
    private final Queue<String> values;
    private int calls;

    QueueGenerator(String... values) {
      this.values = new ArrayDeque<>(Arrays.asList(values));
    }

    @Override
    public String generate() {
      ++calls;
      return values.remove();
    }
  }
}
