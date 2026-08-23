package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import java.util.Objects;

/** Pure authorization and safety policy for replacing one installation control credential. */
final class ControlCredentialRotationPolicy {
  private static final int MAXIMUM_GENERATION_ATTEMPTS = 16;

  interface TokenGenerator {
    String generate();
  }

  record Plan(String nextToken, long nextCredentialGeneration, long nextSetupRevision) {
    Plan {
      if (!BearerAuthorization.isValidToken(nextToken)) {
        throw new IllegalArgumentException("new control credential is invalid");
      }
      if (nextCredentialGeneration < 2 || nextSetupRevision < 1) {
        throw new IllegalArgumentException("new credential metadata is invalid");
      }
    }
  }

  static final class UnauthorizedException extends IllegalArgumentException {
    UnauthorizedException() {
      super("a valid bearer control credential is required");
    }
  }

  static final class StaleRevisionException extends IllegalStateException {
    StaleRevisionException() {
      super("setup configuration changed; reload before rotating the credential");
    }
  }

  private ControlCredentialRotationPolicy() {}

  static Plan authorizeAndPlan(
      String authorizationHeader,
      String currentToken,
      long currentCredentialGeneration,
      long currentSetupRevision,
      long expectedSetupRevision,
      String nodeId,
      String confirmedNodeId,
      boolean setupEditable,
      TokenGenerator generator) {
    Objects.requireNonNull(generator, "generator");
    if (!BearerAuthorization.accepts(authorizationHeader, currentToken)) {
      throw new UnauthorizedException();
    }
    if (!setupEditable) {
      throw new IllegalStateException("Stop capture before rotating the control credential");
    }
    if (expectedSetupRevision != currentSetupRevision) {
      throw new StaleRevisionException();
    }
    if (nodeId == null || nodeId.isBlank() || !nodeId.equals(confirmedNodeId)) {
      throw new IllegalArgumentException(
          "Control credential rotation confirmation must match the full local node ID");
    }
    if (!BearerAuthorization.isValidToken(currentToken) || currentCredentialGeneration < 1) {
      throw new IllegalStateException("Current control credential metadata is invalid");
    }
    if (currentCredentialGeneration == Long.MAX_VALUE) {
      throw new IllegalStateException("Control credential generation is exhausted");
    }
    if (currentSetupRevision == Long.MAX_VALUE) {
      throw new IllegalStateException("Setup revision is exhausted");
    }
    for (int attempt = 0; attempt < MAXIMUM_GENERATION_ATTEMPTS; ++attempt) {
      String generated = generator.generate();
      if (BearerAuthorization.isValidToken(generated) && !generated.equals(currentToken)) {
        return new Plan(
            generated, currentCredentialGeneration + 1, currentSetupRevision + 1);
      }
    }
    throw new IllegalStateException("Unable to generate a distinct control credential");
  }
}
