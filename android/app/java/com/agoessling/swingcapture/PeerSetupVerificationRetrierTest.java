package com.agoessling.swingcapture;

import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

public final class PeerSetupVerificationRetrierTest {
  private PeerSetupVerificationRetrierTest() {}

  public static void main(String[] args) throws Exception {
    retriesOnlyTransientVerificationBeforeOneCommit();
    doesNotRetryAuthenticationRoleOrIdentityFailures();
    exhaustsTheBoundedTransientBudget();
    classifiesTransportAndHttpFailuresConservatively();
    authenticatedIdentityProbeCannotFollowRedirects();
  }

  private static void retriesOnlyTransientVerificationBeforeOneCommit() {
    AtomicInteger probes = new AtomicInteger();
    List<Long> delays = new ArrayList<>();
    String verified =
        PeerSetupVerificationRetrier.execute(
            () -> {
              int attempt = probes.incrementAndGet();
              if (attempt == 1) {
                return result(
                    "SocketTimeoutException",
                    PeerSetupVerificationRetrier.Disposition.TRANSIENT_FAILURE);
              }
              return result("verified-peer", PeerSetupVerificationRetrier.Disposition.VERIFIED);
            },
            delays::add);

    AtomicInteger setupCommits = new AtomicInteger();
    if (verified.equals("verified-peer")) {
      setupCommits.incrementAndGet();
    }
    check(probes.get() == 2, "one transient probe retry");
    check(
        delays.equals(List.of(PeerSetupVerificationRetrier.RETRY_DELAY_MILLIS)),
        "one bounded retry delay");
    check(setupCommits.get() == 1, "verification retries precede exactly one setup mutation");
  }

  private static void doesNotRetryAuthenticationRoleOrIdentityFailures() {
    for (String terminal : List.of("authentication", "role", "identity")) {
      AtomicInteger probes = new AtomicInteger();
      String result =
          PeerSetupVerificationRetrier.execute(
              () -> {
                probes.incrementAndGet();
                return result(
                    terminal, PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
              },
              delay -> {
                throw new AssertionError("terminal verification failure was delayed");
              });
      check(result.equals(terminal), terminal + " result preserved");
      check(probes.get() == 1, terminal + " failure must not be retried");
    }
  }

  private static void exhaustsTheBoundedTransientBudget() {
    AtomicInteger probes = new AtomicInteger();
    AtomicInteger delays = new AtomicInteger();
    String result =
        PeerSetupVerificationRetrier.execute(
            () -> {
              probes.incrementAndGet();
              return result(
                  "still unavailable",
                  PeerSetupVerificationRetrier.Disposition.TRANSIENT_FAILURE);
            },
            delay -> {
              check(
                  delay == PeerSetupVerificationRetrier.RETRY_DELAY_MILLIS,
                  "bounded retry delay");
              delays.incrementAndGet();
            });
    check(result.equals("still unavailable"), "final transient result preserved");
    check(
        probes.get() == PeerSetupVerificationRetrier.MAXIMUM_ATTEMPTS,
        "transient attempt budget");
    check(delays.get() == PeerSetupVerificationRetrier.MAXIMUM_ATTEMPTS - 1, "retry delay budget");
  }

  private static void classifiesTransportAndHttpFailuresConservatively() {
    check(
        PeerSetupVerificationRetrier.dispositionForFailure(new IOException("timeout"))
            == PeerSetupVerificationRetrier.Disposition.TRANSIENT_FAILURE,
        "transport I/O is transient");
    check(
        PeerSetupVerificationRetrier.dispositionForFailure(new IllegalArgumentException("schema"))
            == PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE,
        "malformed identity is terminal");
    for (int status : List.of(408, 425, 429, 500, 502, 503, 504)) {
      check(
          PeerSetupVerificationRetrier.dispositionForHttpStatus(status)
              == PeerSetupVerificationRetrier.Disposition.TRANSIENT_FAILURE,
          "retryable HTTP " + status);
    }
    for (int status : List.of(300, 301, 302, 307, 308, 400, 401, 403, 404, 409)) {
      check(
          PeerSetupVerificationRetrier.dispositionForHttpStatus(status)
              == PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE,
          "terminal HTTP " + status);
    }
  }

  private static void authenticatedIdentityProbeCannotFollowRedirects() throws Exception {
    FakeHttpConnection connection =
        new FakeHttpConnection(new URL("http://peer.test/api/v1/pairing/identity"));
    PeerSetupVerificationRetrier.configureAuthenticatedJsonGet(
        connection, "Bearer destination-bound-token", 1_500);
    check(!connection.getInstanceFollowRedirects(), "authenticated redirects disabled");
    check(connection.getRequestMethod().equals("GET"), "identity probe remains GET");
    check(connection.getConnectTimeout() == 1_500, "connect timeout bounded");
    check(connection.getReadTimeout() == 1_500, "read timeout bounded");
    check(
        connection.getRequestProperty("Authorization").equals("Bearer destination-bound-token"),
        "destination bearer retained");
    check(
        connection.getRequestProperty("Accept").equals("application/json"),
        "identity response format explicit");
  }

  private static PeerSetupVerificationRetrier.Result<String> result(
      String value, PeerSetupVerificationRetrier.Disposition disposition) {
    return new PeerSetupVerificationRetrier.Result<>(value, disposition);
  }

  private static void check(boolean condition, String message) {
    if (!condition) {
      throw new AssertionError(message);
    }
  }

  private static final class FakeHttpConnection extends HttpURLConnection {
    private FakeHttpConnection(URL url) {
      super(url);
    }

    @Override
    public void disconnect() {}

    @Override
    public boolean usingProxy() {
      return false;
    }

    @Override
    public void connect() {}
  }
}
