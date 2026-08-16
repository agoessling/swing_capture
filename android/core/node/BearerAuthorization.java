package com.agoessling.swingcapture.node;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;

/** Generation and constant-time verification for local-node control credentials. */
public final class BearerAuthorization {
  private static final int TOKEN_BYTES = 24;
  private static final String PREFIX = "Bearer ";

  private BearerAuthorization() {}

  public static String generate(SecureRandom random) {
    byte[] bytes = new byte[TOKEN_BYTES];
    random.nextBytes(bytes);
    return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
  }

  public static boolean isValidToken(String token) {
    return token != null && token.matches("[A-Za-z0-9_-]{32}");
  }

  public static boolean accepts(String authorizationHeader, String expectedToken) {
    if (!isValidToken(expectedToken)
        || authorizationHeader == null
        || !authorizationHeader.startsWith(PREFIX)) {
      return false;
    }
    byte[] supplied =
        authorizationHeader.substring(PREFIX.length()).getBytes(StandardCharsets.US_ASCII);
    byte[] expected = expectedToken.getBytes(StandardCharsets.US_ASCII);
    return MessageDigest.isEqual(supplied, expected);
  }
}
