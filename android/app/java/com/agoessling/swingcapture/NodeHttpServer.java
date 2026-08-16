package com.agoessling.swingcapture;

import android.content.Context;
import android.os.SystemClock;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONArray;
import org.json.JSONObject;

/** Foreground-service-owned HTTP API for node control, status, and byte-range media. */
public final class NodeHttpServer {
  public static final int DEFAULT_PORT = 8088;
  private static final int MAXIMUM_REQUEST_HEADER_BYTES = 16 * 1024;
  private static final int MAXIMUM_REQUEST_BODY_BYTES =
      CoordinationHttpEndpoint.MAXIMUM_REQUEST_BODY_BYTES;
  private static final String COORDINATION_PATH_PREFIX = "/api/v1/coordination/";

  /** Non-blocking capture commands dispatched onto the service's serialized executor. */
  public interface CaptureControl {
    void setArmed(boolean armed, String sharedSessionId);

    String triggerManual();
  }

  private final Context context;
  private final NodeConfiguration configuration;
  private final CaptureRuntime runtime;
  private final NodeCoordinationState coordination;
  private final CoordinationHttpEndpoint coordinationEndpoint;
  private final CaptureControl captureControl;
  private final int port;
  private final AtomicBoolean stopping = new AtomicBoolean();
  private final ExecutorService clients = Executors.newFixedThreadPool(4);
  private ServerSocket serverSocket;
  private Thread acceptThread;

  public NodeHttpServer(
      Context context,
      NodeConfiguration configuration,
      CaptureRuntime runtime,
      NodeCoordinationState coordination,
      CoordinationRecordStore coordinationRecords,
      CaptureControl captureControl,
      int port) {
    this.context = context.getApplicationContext();
    this.configuration = configuration;
    this.runtime = runtime;
    this.coordination = coordination;
    this.coordinationEndpoint = new CoordinationHttpEndpoint(coordinationRecords);
    this.captureControl = captureControl;
    this.port = port;
  }

  public void start() throws IOException {
    if (serverSocket != null) {
      return;
    }
    ServerSocket socket = new ServerSocket();
    socket.setReuseAddress(true);
    socket.bind(new InetSocketAddress(port));
    serverSocket = socket;
    acceptThread = new Thread(this::acceptConnections, "node-http-accept");
    acceptThread.start();
  }

  public void stop() {
    stopping.set(true);
    ServerSocket socket = serverSocket;
    if (socket != null) {
      try {
        socket.close();
      } catch (IOException ignored) {
        // The listener is already stopping.
      }
    }
    clients.shutdownNow();
    Thread currentAcceptThread = acceptThread;
    if (currentAcceptThread != null) {
      try {
        currentAcceptThread.join(TimeUnit.SECONDS.toMillis(2));
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
      }
    }
  }

  public List<String> advertisedUrls() {
    List<String> urls = new ArrayList<>();
    try {
      Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
      while (interfaces != null && interfaces.hasMoreElements()) {
        NetworkInterface network = interfaces.nextElement();
        if (!network.isUp() || network.isLoopback()) {
          continue;
        }
        Enumeration<InetAddress> addresses = network.getInetAddresses();
        while (addresses.hasMoreElements()) {
          InetAddress address = addresses.nextElement();
          if (address instanceof Inet4Address && !address.isLoopbackAddress()) {
            urls.add("http://" + address.getHostAddress() + ":" + port);
          }
        }
      }
    } catch (IOException ignored) {
      // The wildcard listener is still valid; only the display address is unavailable.
    }
    Collections.sort(urls);
    return urls;
  }

  private void acceptConnections() {
    while (!stopping.get()) {
      try {
        Socket client = serverSocket.accept();
        client.setSoTimeout(5_000);
        clients.execute(() -> handleClient(client));
      } catch (IOException acceptFailure) {
        if (!stopping.get()) {
          stopping.set(true);
        }
      }
    }
  }

  private void handleClient(Socket client) {
    try (client;
        InputStream input = new BufferedInputStream(client.getInputStream());
        OutputStream output = client.getOutputStream()) {
      HttpRequest request = readRequest(input);
      route(request, output);
    } catch (Throwable ignored) {
      // A malformed or disconnected client must not terminate the listener.
    }
  }

  private void route(HttpRequest request, OutputStream output) throws Exception {
    long requestReceivedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
    if (request.method.equals("OPTIONS")) {
      writeHeaders(output, 204, "No Content", "text/plain", 0, Collections.emptyMap());
      return;
    }
    if (request.method.equals("POST")) {
      routeControl(request, output);
      return;
    }
    boolean head = request.method.equals("HEAD");
    if (!request.method.equals("GET") && !head) {
      writeJson(output, 405, "Method Not Allowed", errorJson("method not allowed"), false);
      return;
    }
    if (isCoordinationPath(request.path)) {
      routeCoordination(request, output, head);
      return;
    }
    if (request.path.equals("/api/v1/node")) {
      JSONObject node = new JSONObject();
      node.put("schema_version", 1);
      node.put("node_id", configuration.nodeId());
      node.put("role", configuration.role().wireName());
      node.put("capture_profile", configuration.captureProfile().wireName());
      node.put("service_urls", new JSONArray(advertisedUrls()));
      node.put("control_authentication", "bearer");
      writeJson(output, 200, "OK", node, head);
      return;
    }
    if (request.path.equals("/api/v1/capture/status")) {
      writeJson(output, 200, "OK", captureStatus(), head);
      return;
    }
    if (request.path.equals("/api/v1/clock")) {
      JSONObject clock = new JSONObject();
      clock.put("schema_version", 1);
      clock.put("node_id", configuration.nodeId());
      clock.put(
          "request_received_elapsed_realtime_ns",
          Long.toString(requestReceivedElapsedRealtimeNanos));
      clock.put(
          "response_prepared_elapsed_realtime_ns",
          Long.toString(SystemClock.elapsedRealtimeNanos()));
      writeJson(output, 200, "OK", clock, head);
      return;
    }
    if (request.path.equals("/api/v1/capture/trigger-report")) {
      NodeCoordinationState.TriggerReport report = coordination.latestTrigger();
      if (report == null) {
        writeJson(output, 404, "Not Found", errorJson("no coordinated trigger report"), head);
      } else {
        writeJson(output, 200, "OK", triggerReportJson(report), head);
      }
      return;
    }
    if (request.path.equals("/api/v1/sessions")) {
      writeJson(output, 200, "OK", sessionList(), head);
      return;
    }

    String[] segments = request.path.split("/");
    if (segments.length == 6
        && segments[1].equals("api")
        && segments[2].equals("v1")
        && segments[3].equals("sessions")
        && safeSegment(segments[4])) {
      String sessionId = segments[4];
      String artifact = segments[5];
      File session = new File(new File(context.getFilesDir(), "sessions"), sessionId);
      if (artifact.equals("manifest")) {
        serveFile(request, output, new File(session, "manifest.json"), "application/json", head);
        return;
      }
      if (safeSegment(artifact) && artifact.endsWith(".mp4")) {
        serveFile(request, output, new File(session, artifact), "video/mp4", head);
        return;
      }
    }
    writeJson(output, 404, "Not Found", errorJson("not found"), head);
  }

  private void routeControl(HttpRequest request, OutputStream output) throws Exception {
    if (!requireAuthentication(request, output, false)) {
      return;
    }
    try {
      if (isCoordinationPath(request.path)) {
        routeCoordinationAuthenticated(request, output, false);
        return;
      }
      if (request.path.equals("/api/v1/capture/arm")) {
        JSONObject body =
            request.body.length == 0 ? new JSONObject() : new JSONObject(request.bodyText());
        if (!body.has("armed")) {
          throw new IllegalArgumentException("armed is required");
        }
        boolean armed = body.getBoolean("armed");
        String sharedSessionId =
            body.has("shared_session_id") && !body.isNull("shared_session_id")
                ? body.getString("shared_session_id")
                : null;
        captureControl.setArmed(armed, sharedSessionId);
        writeJson(output, 202, "Accepted", captureStatus(), false);
        return;
      }
      if (request.path.equals("/api/v1/capture/manual")) {
        String sessionId = captureControl.triggerManual();
        JSONObject summary = new JSONObject();
        summary.put("session_id", sessionId);
        summary.put("state", "waiting_post_roll");
        summary.put("created_at_utc", java.time.Instant.now().toString());
        summary.put("error", "");
        writeJson(output, 202, "Accepted", summary, false);
        return;
      }
      if (request.path.equals("/api/v1/hil/synthetic-swing")) {
        captureControl.triggerManual();
        writeJson(output, 202, "Accepted", captureStatus(), false);
        return;
      }
      writeJson(output, 404, "Not Found", errorJson("not found"), false);
    } catch (org.json.JSONException | IllegalArgumentException malformed) {
      writeJson(output, 400, "Bad Request", errorJson(malformed.getMessage()), false);
    } catch (IllegalStateException rejected) {
      writeJson(output, 409, "Conflict", errorJson(rejected.getMessage()), false);
    }
  }

  private void routeCoordination(HttpRequest request, OutputStream output, boolean head)
      throws IOException {
    if (!requireAuthentication(request, output, head)) {
      return;
    }
    routeCoordinationAuthenticated(request, output, head);
  }

  private void routeCoordinationAuthenticated(
      HttpRequest request, OutputStream output, boolean head) throws IOException {
    String sharedSessionId = request.path.substring(COORDINATION_PATH_PREFIX.length());
    final CoordinationHttpEndpoint.Response response;
    try {
      response =
          request.method.equals("POST")
              ? coordinationEndpoint.post(sharedSessionId, request.body)
              : coordinationEndpoint.get(sharedSessionId);
    } catch (IOException persistenceFailure) {
      writeJson(
          output,
          500,
          "Internal Server Error",
          errorJson("coordination evidence storage failed"),
          head);
      return;
    }

    Map<String, String> headers = new HashMap<>();
    headers.put("X-Swing-Capture-Coordination-Status", response.storeStatus());
    if (response.revision() > 0) {
      headers.put(
          "X-Swing-Capture-Coordination-Revision", Long.toString(response.revision()));
    }
    if (response.statusCode() == 201) {
      headers.put("Location", COORDINATION_PATH_PREFIX + sharedSessionId);
    }
    writeRawJson(
        output,
        response.statusCode(),
        response.reason(),
        response.body(),
        head,
        headers);
  }

  private boolean requireAuthentication(HttpRequest request, OutputStream output, boolean head)
      throws IOException {
    if (BearerAuthorization.accepts(
        request.headers.get("authorization"), configuration.controlToken())) {
      return true;
    }
    writeJson(
        output,
        401,
        "Unauthorized",
        errorJson("a valid bearer control credential is required"),
        head,
        Map.of("WWW-Authenticate", "Bearer"));
    return false;
  }

  private JSONObject captureStatus() throws Exception {
    CaptureRuntime.Snapshot snapshot = runtime.snapshot();
    JSONObject hil = new JSONObject();
    hil.put("enabled", false);
    hil.put("busy", false);
    hil.put("stage", "idle");
    hil.put("error", "");
    hil.put("last_run", JSONObject.NULL);
    JSONObject status = new JSONObject();
    status.put("schema_version", 2);
    status.put("state", webState(snapshot));
    status.put("armed", snapshot.armed());
    status.put(
        "active_session_id",
        snapshot.activeSessionId() == null ? JSONObject.NULL : snapshot.activeSessionId());
    status.put("error", snapshot.error());
    status.put("video_frames", snapshot.videoFrames());
    status.put("audio_frames", snapshot.audioFrames());
    status.put("ring_bytes", snapshot.ringBytes());
    status.put("ring_duration_us", snapshot.ringDurationUs());
    status.put(
        "shared_session_id",
        coordination.sharedSessionId() == null
            ? JSONObject.NULL
            : coordination.sharedSessionId());
    status.put("server_elapsed_realtime_ns", Long.toString(SystemClock.elapsedRealtimeNanos()));
    status.put(
        "last_trigger_elapsed_realtime_ns",
        snapshot.lastTriggerElapsedRealtimeNanos() == 0
            ? JSONObject.NULL
            : Long.toString(snapshot.lastTriggerElapsedRealtimeNanos()));
    status.put("hil", hil);
    return status;
  }

  private static JSONObject triggerReportJson(NodeCoordinationState.TriggerReport report)
      throws Exception {
    return new JSONObject()
        .put("schema_version", 1)
        .put("role", report.role())
        .put("node_id", report.nodeId())
        .put("shared_session_id", report.sharedSessionId())
        .put("local_session_id", report.localSessionId())
        .put(
            "trigger_elapsed_realtime_ns",
            Long.toString(report.triggerElapsedRealtimeNanos()))
        .put("timestamp_uncertainty_ns", report.timestampUncertaintyNanos())
        .put("source", report.source());
  }

  private String webState(CaptureRuntime.Snapshot snapshot) {
    switch (snapshot.state()) {
      case STOPPED:
        return publishedSessions().isEmpty() ? "setup" : "ready";
      case STARTING:
        return "arming";
      case ARMED:
        return "armed";
      case POSTROLL:
        return "waiting_post_roll";
      case PUBLISHING:
        return "encoding";
      case ERROR:
        return "error";
    }
    throw new IllegalStateException("unknown capture state");
  }

  private JSONObject sessionList() throws Exception {
    JSONArray summaries = new JSONArray();
    for (SessionSummary summary : publishedSessions()) {
      JSONObject json = new JSONObject();
      json.put("session_id", summary.sessionId);
      json.put("state", "ready");
      json.put("created_at_utc", summary.createdAtUtc);
      json.put("error", "");
      summaries.put(json);
    }
    JSONObject response = new JSONObject();
    response.put("schema_version", 1);
    response.put("sessions", summaries);
    return response;
  }

  private List<SessionSummary> publishedSessions() {
    List<SessionSummary> summaries = new ArrayList<>();
    File root = new File(context.getFilesDir(), "sessions");
    File[] children = root.listFiles();
    if (children == null) {
      return summaries;
    }
    for (File child : children) {
      if (!child.isDirectory() || child.getName().endsWith(".tmp")) {
        continue;
      }
      File manifestFile = new File(child, "manifest.json");
      try {
        JSONObject manifest = new JSONObject(readFile(manifestFile));
        if (!child.getName().equals(manifest.getString("session_id"))) {
          continue;
        }
        summaries.add(
            new SessionSummary(
                manifest.getString("session_id"), manifest.getString("created_at_utc")));
      } catch (Exception ignored) {
        // An incomplete or malformed directory is never advertised as ready.
      }
    }
    summaries.sort(Comparator.comparing((SessionSummary value) -> value.createdAtUtc).reversed());
    return summaries;
  }

  private void serveFile(
      HttpRequest request,
      OutputStream output,
      File file,
      String contentType,
      boolean head) throws IOException {
    if (!file.isFile()) {
      writeJson(output, 404, "Not Found", errorJson("artifact not found"), head);
      return;
    }
    long fileLength = file.length();
    if (fileLength <= 0) {
      writeJson(output, 500, "Internal Server Error", errorJson("artifact is empty"), head);
      return;
    }
    String rangeHeader = request.headers.get("range");
    HttpByteRange range;
    boolean partial = rangeHeader != null;
    try {
      range = partial ? HttpByteRange.parse(rangeHeader, fileLength) : HttpByteRange.entireFile(fileLength);
    } catch (IllegalArgumentException invalidRange) {
      writeHeaders(
          output,
          416,
          "Range Not Satisfiable",
          "text/plain",
          0,
          Map.of("Content-Range", "bytes */" + fileLength, "Accept-Ranges", "bytes"));
      return;
    }
    Map<String, String> extra = new HashMap<>();
    extra.put("Accept-Ranges", "bytes");
    if (partial) {
      extra.put(
          "Content-Range",
          "bytes " + range.start() + "-" + range.end() + "/" + fileLength);
    }
    writeHeaders(
        output,
        partial ? 206 : 200,
        partial ? "Partial Content" : "OK",
        contentType,
        range.length(),
        extra);
    if (head) {
      return;
    }
    try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
      input.seek(range.start());
      byte[] buffer = new byte[64 * 1024];
      long remaining = range.length();
      while (remaining > 0) {
        int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
        if (count < 0) {
          throw new IOException("Artifact ended before its advertised range");
        }
        output.write(buffer, 0, count);
        remaining -= count;
      }
    }
  }

  private static HttpRequest readRequest(InputStream input) throws IOException {
    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
    int state = 0;
    while (bytes.size() < MAXIMUM_REQUEST_HEADER_BYTES) {
      int value = input.read();
      if (value < 0) {
        throw new IOException("Client disconnected before sending request headers");
      }
      bytes.write(value);
      if ((state == 0 || state == 2) && value == '\r') {
        ++state;
      } else if ((state == 1 || state == 3) && value == '\n') {
        ++state;
        if (state == 4) {
          break;
        }
      } else {
        state = 0;
      }
    }
    if (state != 4) {
      throw new IOException("Request headers exceed the configured limit");
    }
    String[] lines = bytes.toString(StandardCharsets.ISO_8859_1).split("\\r\\n");
    String[] requestLine = lines[0].split(" ");
    if (requestLine.length != 3 || !requestLine[2].startsWith("HTTP/1.")) {
      throw new IOException("Malformed HTTP request line");
    }
    String target = requestLine[1];
    int query = target.indexOf('?');
    String path = query < 0 ? target : target.substring(0, query);
    Map<String, String> headers = new HashMap<>();
    for (int index = 1; index < lines.length; ++index) {
      int separator = lines[index].indexOf(':');
      if (separator > 0) {
        headers.put(
            lines[index].substring(0, separator).trim().toLowerCase(Locale.ROOT),
            lines[index].substring(separator + 1).trim());
      }
    }
    if (headers.containsKey("transfer-encoding")) {
      throw new IOException("Transfer-Encoding is not supported");
    }
    int contentLength = 0;
    if (headers.containsKey("content-length")) {
      try {
        contentLength = Integer.parseInt(headers.get("content-length"));
      } catch (NumberFormatException invalidLength) {
        throw new IOException("Invalid Content-Length", invalidLength);
      }
      if (contentLength < 0 || contentLength > MAXIMUM_REQUEST_BODY_BYTES) {
        throw new IOException("Request body exceeds the configured limit");
      }
    }
    byte[] body = input.readNBytes(contentLength);
    if (body.length != contentLength) {
      throw new IOException("Client disconnected before sending the request body");
    }
    return new HttpRequest(requestLine[0], path, headers, body);
  }

  private static void writeJson(
      OutputStream output, int status, String reason, JSONObject json, boolean head)
      throws IOException {
    writeJson(output, status, reason, json, head, Collections.emptyMap());
  }

  private static void writeJson(
      OutputStream output,
      int status,
      String reason,
      JSONObject json,
      boolean head,
      Map<String, String> extraHeaders)
      throws IOException {
    byte[] body = (json.toString() + "\n").getBytes(StandardCharsets.UTF_8);
    writeHeaders(
        output,
        status,
        reason,
        "application/json; charset=utf-8",
        body.length,
        extraHeaders);
    if (!head) {
      output.write(body);
    }
  }

  private static void writeRawJson(
      OutputStream output,
      int status,
      String reason,
      String json,
      boolean head,
      Map<String, String> extraHeaders)
      throws IOException {
    byte[] body = (json + "\n").getBytes(StandardCharsets.UTF_8);
    writeHeaders(
        output,
        status,
        reason,
        "application/json; charset=utf-8",
        body.length,
        extraHeaders);
    if (!head) {
      output.write(body);
    }
  }

  private static void writeHeaders(
      OutputStream output,
      int status,
      String reason,
      String contentType,
      long contentLength,
      Map<String, String> extraHeaders) throws IOException {
    StringBuilder headers = new StringBuilder();
    headers.append("HTTP/1.1 ").append(status).append(' ').append(reason).append("\r\n");
    headers.append("Content-Type: ").append(contentType).append("\r\n");
    headers.append("Content-Length: ").append(contentLength).append("\r\n");
    headers.append("Access-Control-Allow-Origin: *\r\n");
    headers.append("Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS\r\n");
    headers.append("Access-Control-Allow-Headers: Authorization, Range, Content-Type\r\n");
    headers.append(
        "Access-Control-Expose-Headers: Accept-Ranges, Content-Length, Content-Range, Location, "
            + "X-Swing-Capture-Coordination-Revision, "
            + "X-Swing-Capture-Coordination-Status\r\n");
    headers.append("Connection: close\r\n");
    for (Map.Entry<String, String> entry : extraHeaders.entrySet()) {
      headers.append(entry.getKey()).append(": ").append(entry.getValue()).append("\r\n");
    }
    headers.append("\r\n");
    output.write(headers.toString().getBytes(StandardCharsets.ISO_8859_1));
  }

  private static JSONObject errorJson(String message) {
    JSONObject error = new JSONObject();
    try {
      error.put("error", message);
    } catch (Exception impossible) {
      throw new IllegalStateException("Unable to build an error response", impossible);
    }
    return error;
  }

  private static boolean safeSegment(String value) {
    return !value.isEmpty()
        && !value.equals(".")
        && !value.equals("..")
        && !value.endsWith(".tmp")
        && value.matches("[A-Za-z0-9._-]+");
  }

  private static boolean isCoordinationPath(String path) {
    return path.startsWith(COORDINATION_PATH_PREFIX);
  }

  private static String readFile(File file) throws IOException {
    try (FileInputStream input = new FileInputStream(file)) {
      return new String(input.readAllBytes(), StandardCharsets.UTF_8);
    }
  }

  private static final class HttpRequest {
    private final String method;
    private final String path;
    private final Map<String, String> headers;
    private final byte[] body;

    private HttpRequest(String method, String path, Map<String, String> headers, byte[] body) {
      this.method = method;
      this.path = path;
      this.headers = headers;
      this.body = body;
    }

    private String bodyText() {
      return new String(body, StandardCharsets.UTF_8);
    }
  }

  private static final class SessionSummary {
    private final String sessionId;
    private final String createdAtUtc;

    private SessionSummary(String sessionId, String createdAtUtc) {
      this.sessionId = sessionId;
      this.createdAtUtc = createdAtUtc;
    }
  }
}
