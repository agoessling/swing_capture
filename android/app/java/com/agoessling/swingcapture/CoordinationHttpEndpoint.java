package com.agoessling.swingcapture;

import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore.StoreResult;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore.StoreStatus;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore.VersionedRecord;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Optional;

/** Pure HTTP-domain adapter for strict paired-coordination GET and POST operations. */
public final class CoordinationHttpEndpoint {
  public static final int MAXIMUM_REQUEST_BODY_BYTES =
      PairedCoordinationRecord.MAXIMUM_SERIALIZED_BYTES;

  /** The body is either canonical schema-v1 evidence or a stable JSON error object. */
  public record Response(
      int statusCode, String reason, String body, long revision, String storeStatus) {}

  private final CoordinationRecordStore records;

  public CoordinationHttpEndpoint(CoordinationRecordStore records) {
    if (records == null) {
      throw new NullPointerException("records");
    }
    this.records = records;
  }

  public Response get(String sharedSessionId) throws IOException {
    try {
      PairedCoordinationRecord.validateSharedSessionId(sharedSessionId);
    } catch (IllegalArgumentException | NullPointerException invalid) {
      return invalidRecord();
    }
    Optional<VersionedRecord> stored = records.read(sharedSessionId);
    if (stored.isEmpty()) {
      return new Response(
          404,
          "Not Found",
          "{\"error\":\"coordination record not found\"}",
          0,
          "missing");
    }
    VersionedRecord record = stored.orElseThrow();
    return new Response(
        200, "OK", record.record().toJson(), record.revision(), "present");
  }

  public Response post(String sharedSessionId, byte[] body) throws IOException {
    if (body == null) {
      throw new NullPointerException("body");
    }
    try {
      PairedCoordinationRecord.validateSharedSessionId(sharedSessionId);
    } catch (IllegalArgumentException | NullPointerException invalid) {
      return invalidRecord();
    }
    if (body.length == 0 || body.length > MAXIMUM_REQUEST_BODY_BYTES) {
      return invalidRecord();
    }

    final PairedCoordinationRecord requested;
    try {
      String json =
          StandardCharsets.UTF_8
              .newDecoder()
              .onMalformedInput(CodingErrorAction.REPORT)
              .onUnmappableCharacter(CodingErrorAction.REPORT)
              .decode(ByteBuffer.wrap(body))
              .toString();
      requested = PairedCoordinationRecord.fromJson(json);
    } catch (CharacterCodingException | IllegalArgumentException | ArithmeticException malformed) {
      return invalidRecord();
    }
    if (!requested.sharedSessionId().equals(sharedSessionId)) {
      return new Response(
          400,
          "Bad Request",
          "{\"error\":\"path and record shared_session_id differ\"}",
          0,
          "invalid");
    }

    StoreResult result = records.storeIfAbsent(requested);
    int statusCode;
    String reason;
    String storeStatus;
    if (result.status() == StoreStatus.STORED) {
      statusCode = 201;
      reason = "Created";
      storeStatus = "stored";
    } else if (result.status() == StoreStatus.ALREADY_PRESENT) {
      statusCode = 200;
      reason = "OK";
      storeStatus = "already_present";
    } else {
      statusCode = 409;
      reason = "Conflict";
      storeStatus = "conflict";
    }
    return new Response(
        statusCode,
        reason,
        result.stored().record().toJson(),
        result.stored().revision(),
        storeStatus);
  }

  private static Response invalidRecord() {
    return new Response(
        400,
        "Bad Request",
        "{\"error\":\"invalid paired coordination record\"}",
        0,
        "invalid");
  }
}
