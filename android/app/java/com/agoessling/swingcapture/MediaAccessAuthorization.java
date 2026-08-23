package com.agoessling.swingcapture;

import com.agoessling.swingcapture.node.BearerAuthorization;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.Objects;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** Path-scoped read capability for immutable native-browser media requests. */
final class MediaAccessAuthorization {
  static final String QUERY_PARAMETER = "media_access";
  static final String RESPONSE_HEADER = "X-Swing-Capture-Media-Access";

  private static final String HMAC_ALGORITHM = "HmacSHA256";
  private static final String DOMAIN_SEPARATOR = "swing-capture-media-v1\n";

  private MediaAccessAuthorization() {}

  /** Returns the complete raw query carried by every artifact in one immutable collection item. */
  static String query(String collectionDirectory, String identifier, String controlToken) {
    validateScope(collectionDirectory, identifier);
    if (!BearerAuthorization.isValidToken(controlToken)) {
      throw new IllegalArgumentException("control token is invalid");
    }
    String scope = DOMAIN_SEPARATOR + collectionDirectory + "/" + identifier;
    try {
      Mac mac = Mac.getInstance(HMAC_ALGORITHM);
      mac.init(new SecretKeySpec(controlToken.getBytes(StandardCharsets.US_ASCII), HMAC_ALGORITHM));
      String capability =
          Base64.getUrlEncoder()
              .withoutPadding()
              .encodeToString(mac.doFinal(scope.getBytes(StandardCharsets.US_ASCII)));
      return QUERY_PARAMETER + "=" + capability;
    } catch (GeneralSecurityException unavailable) {
      throw new IllegalStateException("HMAC-SHA256 is unavailable", unavailable);
    }
  }

  static boolean accepts(
      String rawQuery, String collectionDirectory, String identifier, String controlToken) {
    Objects.requireNonNull(rawQuery, "rawQuery");
    final String expected;
    try {
      expected = query(collectionDirectory, identifier, controlToken);
    } catch (IllegalArgumentException invalidConfiguration) {
      return false;
    }
    return MessageDigest.isEqual(
        rawQuery.getBytes(StandardCharsets.US_ASCII),
        expected.getBytes(StandardCharsets.US_ASCII));
  }

  private static void validateScope(String collectionDirectory, String identifier) {
    if ((!collectionDirectory.equals("sessions")
            && !collectionDirectory.equals("field_recordings"))
        || !NodeHttpProtocol.safeSegment(identifier)) {
      throw new IllegalArgumentException("media access scope is invalid");
    }
  }
}
