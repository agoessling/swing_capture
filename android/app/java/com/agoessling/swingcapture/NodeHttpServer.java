package com.agoessling.swingcapture;

import android.content.Context;
import android.os.SystemClock;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import com.agoessling.swingcapture.diagnostics.DiagnosticFeedbackRequest;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMark;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMarkKind;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
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
import java.nio.file.Files;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.locks.ReentrantLock;
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

    String triggerMissedShot();
  }

  private final Context context;
  private final NodeConfiguration configuration;
  private final CaptureRuntime runtime;
  private final NodeCoordinationState coordination;
  private final CoordinationRecordStore coordinationRecords;
  private final CoordinationHttpEndpoint coordinationEndpoint;
  private final SessionDiagnosticStore diagnosticStore;
  private final ReentrantLock diagnosticIoLock = new ReentrantLock();
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
    this.coordinationRecords = coordinationRecords;
    this.coordinationEndpoint = new CoordinationHttpEndpoint(coordinationRecords);
    this.diagnosticStore =
        new SessionDiagnosticStore(AndroidDirectorySync::synchronize);
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
      if (artifact.equals("diagnostics.zip")) {
        if (!requireAuthentication(request, output, head)) {
          return;
        }
        if (!new File(session, "manifest.json").isFile()) {
          writeJson(output, 404, "Not Found", errorJson("session not found"), head);
          return;
        }
        if (!diagnosticIoLock.tryLock()) {
          writeJson(
              output,
              409,
              "Conflict",
              errorJson("another diagnostic operation is in progress"),
              head,
              Map.of("Retry-After", "1"));
          return;
        }
        File workspace = null;
        try {
          final File archive;
          try {
            workspace = createDiagnosticExportWorkspace(session, sessionId);
            archive = new File(workspace, "diagnostics.zip");
            SessionDiagnosticArchive.create(
                new File(workspace, sessionId), archive, Instant.now());
          } catch (IOException | IllegalArgumentException exportFailure) {
            writeJson(
                output,
                500,
                "Internal Server Error",
                errorJson("diagnostic export failed"),
                head);
            return;
          }
          serveWholeFile(
              output,
              archive,
              "application/zip",
              head,
              Map.of(
                  "Content-Disposition",
                  diagnosticContentDisposition(sessionId),
                  "Cache-Control",
                  "no-store"));
        } finally {
          try {
            if (workspace != null) {
              deleteDiagnosticExportWorkspace(workspace);
            }
          } finally {
            diagnosticIoLock.unlock();
          }
        }
        return;
      }
    }
    if (serveStaticAsset(request, output, head)) {
      return;
    }
    writeJson(output, 404, "Not Found", errorJson("not found"), head);
  }

  private boolean serveStaticAsset(HttpRequest request, OutputStream output, boolean head)
      throws IOException {
    StaticWebAssetRoute.Result route = StaticWebAssetRoute.resolve(request.path);
    if (route == null) {
      return false;
    }

    final byte[] body;
    try (InputStream input = context.getAssets().open(route.assetPath())) {
      body = input.readAllBytes();
    } catch (FileNotFoundException missingAsset) {
      return false;
    }
    writeHeaders(
        output,
        200,
        "OK",
        route.contentType(),
        body.length,
        Map.of(
            "Cache-Control", route.cacheControl(),
            "X-Content-Type-Options", "nosniff"));
    if (!head) {
      output.write(body);
    }
    return true;
  }

  private File createDiagnosticExportWorkspace(File session, String sessionId)
      throws IOException {
    File exportRoot = new File(context.getCacheDir(), "diagnostic_exports");
    if (Files.isSymbolicLink(exportRoot.toPath())) {
      throw new IOException("diagnostic export root cannot be a symbolic link");
    }
    if (!exportRoot.isDirectory() && !exportRoot.mkdir()) {
      throw new IOException("unable to create diagnostic export root");
    }
    if (!exportRoot
        .getCanonicalFile()
        .getParentFile()
        .equals(context.getCacheDir().getCanonicalFile())) {
      throw new IOException("diagnostic export root escapes the app cache");
    }

    File workspace = Files.createTempDirectory(exportRoot.toPath(), "request-").toFile();
    File stagedSession = new File(workspace, sessionId);
    if (!stagedSession.mkdir()) {
      throw new IOException("unable to create diagnostic export staging directory");
    }
    try {
      ensureDiagnosticIncident(session, sessionId);
      stageDiagnosticTree(session, session, stagedSession);
      stageCoordinationRecord(session, sessionId, stagedSession);
      return workspace;
    } catch (IOException | RuntimeException failure) {
      try {
        deleteDiagnosticExportWorkspace(workspace);
      } catch (IOException cleanupFailure) {
        failure.addSuppressed(cleanupFailure);
      }
      throw failure;
    }
  }

  private static void stageDiagnosticTree(File root, File source, File destination)
      throws IOException {
    if (Files.isSymbolicLink(source.toPath())) {
      throw new IOException("diagnostic session contains a symbolic link");
    }
    if (!source.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator)
        && !source.getCanonicalFile().equals(root.getCanonicalFile())) {
      throw new IOException("diagnostic session entry escapes its root");
    }
    if (source.isDirectory()) {
      File[] children = source.listFiles();
      if (children == null) {
        throw new IOException("unable to enumerate diagnostic session");
      }
      for (File child : children) {
        File stagedChild = new File(destination, child.getName());
        if (child.isDirectory() && !stagedChild.mkdir()) {
          throw new IOException("unable to create diagnostic staging directory");
        }
        stageDiagnosticTree(root, child, stagedChild);
      }
      return;
    }
    if (!source.isFile()) {
      throw new IOException("diagnostic session contains a non-file entry");
    }
    try {
      Files.createLink(destination.toPath(), source.toPath());
    } catch (IOException | UnsupportedOperationException hardLinkUnavailable) {
      Files.copy(source.toPath(), destination.toPath());
    }
  }

  private void stageCoordinationRecord(File session, String sessionId, File stagedSession)
      throws IOException {
    final JSONObject manifest;
    try {
      manifest = new JSONObject(readFile(new File(session, "manifest.json")));
      if (!sessionId.equals(manifest.getString("session_id"))) {
        throw new IOException("diagnostic manifest belongs to another session");
      }
    } catch (org.json.JSONException malformed) {
      throw new IOException("diagnostic manifest is malformed", malformed);
    }
    JSONObject androidCapture = manifest.optJSONObject("android_capture");
    if (androidCapture == null
        || !androidCapture.has("shared_session_id")
        || androidCapture.isNull("shared_session_id")) {
      return;
    }
    String sharedSessionId;
    try {
      sharedSessionId = androidCapture.getString("shared_session_id");
    } catch (org.json.JSONException malformed) {
      throw new IOException("diagnostic shared session identifier is malformed", malformed);
    }
    Optional<CoordinationRecordStore.VersionedRecord> stored =
        coordinationRecords.read(sharedSessionId);
    if (stored.isEmpty()) {
      return;
    }
    PairedCoordinationRecord record = stored.orElseThrow().record();
    String sourceNodeId;
    try {
      sourceNodeId = androidCapture.getString("node_id");
    } catch (org.json.JSONException malformed) {
      throw new IOException("diagnostic node identifier is malformed", malformed);
    }
    boolean matchesLocalEvidence =
        matchesLocalEvidence(record.downTheLine(), sessionId, sourceNodeId)
            || matchesLocalEvidence(record.faceOn(), sessionId, sourceNodeId);
    if (!matchesLocalEvidence) {
      throw new IOException("coordination record does not contain the exported local session");
    }
    File supplemental = new File(stagedSession, "coordination_record.json");
    if (supplemental.exists()) {
      throw new IOException("diagnostic session contains a reserved coordination artifact");
    }
    Files.write(
        supplemental.toPath(), (record.toJson() + "\n").getBytes(StandardCharsets.UTF_8));
  }

  private static boolean matchesLocalEvidence(
      PairedCoordinationRecord.NodeEvidence evidence, String sessionId, String nodeId) {
    return evidence.localSessionId().equals(sessionId) && evidence.nodeId().equals(nodeId);
  }

  private void ensureDiagnosticIncident(File session, String sessionId) throws IOException {
    File incident = new File(session, SessionDiagnosticStore.FILE_NAME);
    if (incident.isFile()) {
      return;
    }
    final JSONObject manifest;
    try {
      manifest = new JSONObject(readFile(new File(session, "manifest.json")));
      if (!sessionId.equals(manifest.getString("session_id"))) {
        throw new IOException("diagnostic manifest belongs to another session");
      }
      JSONObject androidCapture = manifest.optJSONObject("android_capture");
      String sourceNodeId =
          androidCapture == null
              ? configuration.nodeId()
              : androidCapture.optString("node_id", configuration.nodeId());
      diagnosticStore.initialize(
          session,
          sessionId,
          sourceNodeId,
          manifest.getJSONObject("trigger").getString("source"),
          Instant.parse(manifest.getString("created_at_utc")).toEpochMilli());
    } catch (IOException failure) {
      throw failure;
    } catch (Exception malformed) {
      throw new IOException(
          "unable to initialize diagnostics from the session manifest", malformed);
    }
  }

  private static void validateDiagnosticFeedbackTiming(
      File session, DiagnosticFeedbackRequest feedback) throws IOException {
    if (feedback.timingMarks().isEmpty()) {
      return;
    }
    final JSONObject manifest;
    try {
      manifest = new JSONObject(readFile(new File(session, "manifest.json")));
    } catch (org.json.JSONException malformed) {
      throw new IOException("diagnostic manifest is malformed", malformed);
    }

    DiagnosticTimingBounds videoBounds = null;
    DiagnosticTimingBounds audioBounds = null;
    boolean audioBoundsLoaded = false;
    for (TimingMark mark : feedback.timingMarks()) {
      if (mark.kind() == TimingMarkKind.HIGH_SPEED_SHOULD_START
          || mark.kind() == TimingMarkKind.VISUAL_BALL_IMPACT) {
        if (videoBounds == null) {
          videoBounds = diagnosticVideoTimingBounds(manifest);
        }
        videoBounds.requireContains(mark.offsetMicros(), mark.kind().wireName());
      } else if (mark.kind() == TimingMarkKind.AUDIO_IMPACT_TRANSIENT) {
        if (!audioBoundsLoaded) {
          audioBounds = diagnosticAudioTimingBounds(manifest);
          audioBoundsLoaded = true;
        }
        if (audioBounds == null) {
          throw new IllegalArgumentException(
              "audio_impact_us requires retained diagnostic audio evidence");
        }
        audioBounds.requireContains(mark.offsetMicros(), mark.kind().wireName());
      }
    }
  }

  private static DiagnosticTimingBounds diagnosticVideoTimingBounds(JSONObject manifest)
      throws IOException {
    try {
      JSONArray views = manifest.getJSONArray("views");
      if (views.length() == 0) {
        throw new IOException("diagnostic manifest has no video views");
      }
      long commonMinimum = Long.MIN_VALUE;
      long commonMaximum = Long.MAX_VALUE;
      for (int viewIndex = 0; viewIndex < views.length(); ++viewIndex) {
        JSONArray frames = views.getJSONObject(viewIndex).getJSONArray("frames");
        if (frames.length() == 0) {
          throw new IOException("diagnostic manifest video view has no frames");
        }
        long viewMinimum = Long.MAX_VALUE;
        long viewMaximum = Long.MIN_VALUE;
        for (int frameIndex = 0; frameIndex < frames.length(); ++frameIndex) {
          long offset =
              strictManifestInteger(
                  frames.getJSONObject(frameIndex).get("time_from_impact_us"),
                  "time_from_impact_us");
          viewMinimum = Math.min(viewMinimum, offset);
          viewMaximum = Math.max(viewMaximum, offset);
        }
        commonMinimum = Math.max(commonMinimum, viewMinimum);
        commonMaximum = Math.min(commonMaximum, viewMaximum);
      }
      if (commonMinimum > commonMaximum) {
        throw new IOException("diagnostic video views have no common timing window");
      }
      return new DiagnosticTimingBounds(commonMinimum, commonMaximum);
    } catch (org.json.JSONException malformed) {
      throw new IOException("diagnostic video timing metadata is malformed", malformed);
    }
  }

  private static DiagnosticTimingBounds diagnosticAudioTimingBounds(JSONObject manifest)
      throws IOException {
    try {
      JSONObject androidCapture = manifest.optJSONObject("android_capture");
      if (androidCapture == null) {
        return null;
      }
      JSONObject diagnosticEvidence = androidCapture.optJSONObject("diagnostic_evidence");
      if (diagnosticEvidence == null
          || !diagnosticEvidence.has("audio")
          || diagnosticEvidence.isNull("audio")) {
        return null;
      }
      JSONObject audio = diagnosticEvidence.getJSONObject("audio");
      long firstFrame = strictManifestDecimal(audio.getString("first_frame_position"));
      long endFrame = strictManifestDecimal(audio.getString("end_frame_position"));
      long markerFrame = strictManifestDecimal(audio.getString("marker_frame_position"));
      long sampleRate = strictManifestInteger(audio.get("sample_rate_hz"), "sample_rate_hz");
      if (sampleRate <= 0
          || firstFrame < 0
          || endFrame <= firstFrame
          || markerFrame < firstFrame
          || markerFrame >= endFrame) {
        throw new IOException("diagnostic audio timing metadata is invalid");
      }
      long firstOffsetNumerator =
          Math.multiplyExact(Math.subtractExact(firstFrame, markerFrame), 1_000_000L);
      long endOffsetNumerator =
          Math.multiplyExact(Math.subtractExact(endFrame, markerFrame), 1_000_000L);
      long minimumOffsetUs = ceilingDivide(firstOffsetNumerator, sampleRate);
      long endOffsetUs = ceilingDivide(endOffsetNumerator, sampleRate);
      return new DiagnosticTimingBounds(minimumOffsetUs, Math.subtractExact(endOffsetUs, 1));
    } catch (org.json.JSONException | ArithmeticException malformed) {
      throw new IOException("diagnostic audio timing metadata is malformed", malformed);
    }
  }

  private static long strictManifestInteger(Object value, String name) throws IOException {
    if (!(value instanceof Integer) && !(value instanceof Long)) {
      throw new IOException(name + " must be a JSON integer");
    }
    return ((Number) value).longValue();
  }

  private static long strictManifestDecimal(String value) throws IOException {
    if (!value.matches("-?(0|[1-9][0-9]*)") || value.equals("-0")) {
      throw new IOException("diagnostic audio position is not a canonical decimal integer");
    }
    try {
      return Long.parseLong(value);
    } catch (NumberFormatException outOfRange) {
      throw new IOException("diagnostic audio position exceeds signed 64-bit range", outOfRange);
    }
  }

  private static long ceilingDivide(long numerator, long positiveDenominator) {
    long quotient = Math.floorDiv(numerator, positiveDenominator);
    return Math.floorMod(numerator, positiveDenominator) == 0 ? quotient : quotient + 1;
  }

  private record DiagnosticTimingBounds(long minimumUs, long maximumUs) {
    private DiagnosticTimingBounds {
      if (minimumUs > maximumUs) {
        throw new IllegalArgumentException("diagnostic timing bounds are inverted");
      }
    }

    private void requireContains(long offsetUs, String label) {
      if (offsetUs < minimumUs || offsetUs > maximumUs) {
        throw new IllegalArgumentException(
            label
                + " must be within retained evidence ["
                + minimumUs
                + ", "
                + maximumUs
                + "] us");
      }
    }
  }

  private void deleteDiagnosticExportWorkspace(File workspace) throws IOException {
    File exportRoot = new File(context.getCacheDir(), "diagnostic_exports").getCanonicalFile();
    File canonicalWorkspace = workspace.getCanonicalFile();
    if (!canonicalWorkspace.getParentFile().equals(exportRoot)
        || !canonicalWorkspace.getName().startsWith("request-")) {
      throw new IOException("refusing to delete an unexpected diagnostic export path");
    }
    deleteDiagnosticExportEntry(workspace);
  }

  private static void deleteDiagnosticExportEntry(File entry) throws IOException {
    if (Files.isSymbolicLink(entry.toPath())) {
      throw new IOException("refusing to delete a linked diagnostic export entry");
    }
    if (entry.isDirectory()) {
      File[] children = entry.listFiles();
      if (children == null) {
        throw new IOException("unable to enumerate diagnostic export workspace");
      }
      for (File child : children) {
        deleteDiagnosticExportEntry(child);
      }
    }
    Files.deleteIfExists(entry.toPath());
  }

  private static String diagnosticContentDisposition(String sessionId) {
    return "attachment; filename=\"swing-capture-" + sessionId + ".zip\"";
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
      if (request.path.equals("/api/v1/capture/missed-shot")) {
        JSONObject body =
            request.body.length == 0 ? new JSONObject() : new JSONObject(request.bodyText());
        if (body.length() != 0) {
          throw new IllegalArgumentException("missed-shot request body must be empty");
        }
        String sessionId = captureControl.triggerMissedShot();
        JSONObject summary = new JSONObject();
        summary.put("session_id", sessionId);
        summary.put("state", "waiting_post_roll");
        summary.put("created_at_utc", java.time.Instant.now().toString());
        summary.put("error", "");
        writeJson(output, 202, "Accepted", summary, false);
        return;
      }
      String[] segments = request.path.split("/");
      if (segments.length == 6
          && segments[1].equals("api")
          && segments[2].equals("v1")
          && segments[3].equals("sessions")
          && safeSegment(segments[4])
          && segments[5].equals("feedback")) {
        String sessionId = segments[4];
        File session = new File(new File(context.getFilesDir(), "sessions"), sessionId);
        if (!new File(session, "manifest.json").isFile()) {
          writeJson(output, 404, "Not Found", errorJson("session not found"), false);
          return;
        }
        if (!diagnosticIoLock.tryLock()) {
          writeJson(
              output,
              409,
              "Conflict",
              errorJson("another diagnostic operation is in progress"),
              false,
              Map.of("Retry-After", "1"));
          return;
        }
        try {
          com.agoessling.swingcapture.diagnostics.DiagnosticIncident updated;
          DiagnosticFeedbackRequest feedback =
              DiagnosticFeedbackRequest.parseJson(request.bodyText());
          validateDiagnosticFeedbackTiming(session, feedback);
          ensureDiagnosticIncident(session, sessionId);
          updated = diagnosticStore.applyFeedback(session, sessionId, request.body);
          writeRawJson(output, 200, "OK", updated.toCanonicalJson(), false, Map.of());
        } catch (IOException persistenceFailure) {
          writeJson(
              output,
              500,
              "Internal Server Error",
              errorJson("diagnostic feedback storage failed"),
              false);
        } finally {
          diagnosticIoLock.unlock();
        }
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

  private static void serveWholeFile(
      OutputStream output,
      File file,
      String contentType,
      boolean head,
      Map<String, String> responseHeaders)
      throws IOException {
    if (!file.isFile()) {
      writeJson(output, 404, "Not Found", errorJson("artifact not found"), head);
      return;
    }
    long fileLength = file.length();
    if (fileLength <= 0) {
      writeJson(output, 500, "Internal Server Error", errorJson("artifact is empty"), head);
      return;
    }
    writeHeaders(output, 200, "OK", contentType, fileLength, responseHeaders);
    if (head) {
      return;
    }
    try (InputStream input = new BufferedInputStream(new FileInputStream(file))) {
      byte[] buffer = new byte[64 * 1024];
      long remaining = fileLength;
      while (remaining > 0) {
        int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
        if (count < 0) {
          throw new IOException("Artifact ended before its advertised length");
        }
        output.write(buffer, 0, count);
        remaining -= count;
      }
    }
  }

  private void serveFile(
      HttpRequest request,
      OutputStream output,
      File file,
      String contentType,
      boolean head) throws IOException {
    serveFile(request, output, file, contentType, head, Map.of());
  }

  private void serveFile(
      HttpRequest request,
      OutputStream output,
      File file,
      String contentType,
      boolean head,
      Map<String, String> responseHeaders) throws IOException {
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
    Map<String, String> extra = new HashMap<>(responseHeaders);
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
        "Access-Control-Expose-Headers: Accept-Ranges, Content-Disposition, Content-Length, "
            + "Content-Range, Location, "
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
