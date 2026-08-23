package com.agoessling.swingcapture;

import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.ProtocolException;
import java.util.Objects;

/** Bounded retry policy for the authenticated peer probe that precedes a setup mutation. */
final class PeerSetupVerificationRetrier {
  static final int MAXIMUM_ATTEMPTS = 3;
  static final long RETRY_DELAY_MILLIS = 250;

  enum Disposition {
    VERIFIED,
    TRANSIENT_FAILURE,
    TERMINAL_FAILURE
  }

  record Result<T>(T value, Disposition disposition) {
    Result {
      Objects.requireNonNull(value, "value");
      Objects.requireNonNull(disposition, "disposition");
    }
  }

  @FunctionalInterface
  interface Probe<T> {
    Result<T> execute();
  }

  @FunctionalInterface
  interface Sleeper {
    void sleep(long delayMillis) throws InterruptedException;
  }

  private PeerSetupVerificationRetrier() {}

  static <T> T execute(Probe<T> probe) {
    return execute(probe, Thread::sleep);
  }

  static <T> T execute(Probe<T> probe, Sleeper sleeper) {
    Objects.requireNonNull(probe, "probe");
    Objects.requireNonNull(sleeper, "sleeper");
    for (int attempt = 1; attempt <= MAXIMUM_ATTEMPTS; ++attempt) {
      Result<T> result = Objects.requireNonNull(probe.execute(), "probe result");
      if (result.disposition() != Disposition.TRANSIENT_FAILURE
          || attempt == MAXIMUM_ATTEMPTS) {
        return result.value();
      }
      try {
        sleeper.sleep(RETRY_DELAY_MILLIS);
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        return result.value();
      }
    }
    throw new AssertionError("bounded peer setup retry loop did not return");
  }

  static Disposition dispositionForHttpStatus(int statusCode) {
    if (statusCode == 200) {
      return Disposition.VERIFIED;
    }
    return switch (statusCode) {
      case 408, 425, 429, 500, 502, 503, 504 -> Disposition.TRANSIENT_FAILURE;
      default -> Disposition.TERMINAL_FAILURE;
    };
  }

  static Disposition dispositionForFailure(Exception failure) {
    Objects.requireNonNull(failure, "failure");
    return failure instanceof IOException
        ? Disposition.TRANSIENT_FAILURE
        : Disposition.TERMINAL_FAILURE;
  }

  /**
   * Configures the credentialed identity probe without permitting redirect credential forwarding.
   */
  static void configureAuthenticatedJsonGet(
      HttpURLConnection connection, String authorization, int timeoutMillis)
      throws ProtocolException {
    Objects.requireNonNull(connection, "connection");
    if (authorization == null || authorization.isBlank() || timeoutMillis <= 0) {
      throw new IllegalArgumentException("authenticated peer request configuration is invalid");
    }
    connection.setConnectTimeout(timeoutMillis);
    connection.setReadTimeout(timeoutMillis);
    connection.setInstanceFollowRedirects(false);
    connection.setRequestMethod("GET");
    connection.setRequestProperty("Authorization", authorization);
    connection.setRequestProperty("Accept", "application/json");
  }
}
