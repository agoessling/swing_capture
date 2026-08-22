package com.agoessling.swingcapture;

import android.content.Context;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;
import com.agoessling.swingcapture.core.coordination.CoordinationRecordStore;
import com.agoessling.swingcapture.core.coordination.PairedCoordinationRecord;
import com.agoessling.swingcapture.diagnostics.DiagnosticFeedbackRequest;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMark;
import com.agoessling.swingcapture.diagnostics.DiagnosticIncident.TimingMarkKind;
import com.agoessling.swingcapture.node.BearerAuthorization;
import com.agoessling.swingcapture.node.CaptureRuntime;
import com.agoessling.swingcapture.node.NodeCoordinationState;
import com.agoessling.swingcapture.pose.NormalizedHittingRegion;
import com.agoessling.swingcapture.pose.inference.PoseInferenceDelegatePolicy;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
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
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.locks.ReentrantLock;
import org.json.JSONArray;
import org.json.JSONObject;

/** Foreground-service-owned HTTP API for node control, status, and byte-range media. */
public final class NodeHttpServer {
  private static final String TAG = "SwingCaptureHttp";
  public static final int DEFAULT_PORT = 8088;
  private static final int MAXIMUM_REQUEST_HEADER_BYTES = 16 * 1024;
  private static final int MAXIMUM_REQUEST_BODY_BYTES =
      CoordinationHttpEndpoint.MAXIMUM_REQUEST_BODY_BYTES;
  private static final String COORDINATION_PATH_PREFIX = "/api/v1/coordination/";
  private static final int SETUP_PEER_TIMEOUT_MILLIS = 750;
  private static final int SETUP_PEER_MAXIMUM_RESPONSE_BYTES = 16 * 1024;

  /** Non-blocking capture commands dispatched onto the service's serialized executor. */
  public interface CaptureControl {
    record TriggeredSession(String sessionId, String sessionKind) {
      public TriggeredSession {
        if (sessionId == null || !sessionId.matches("[A-Za-z0-9._-]+")) {
          throw new IllegalArgumentException("triggered session identifier is invalid");
        }
        if (!Set.of("capture", "standby_diagnostic").contains(sessionKind)) {
          throw new IllegalArgumentException("triggered session kind is invalid");
        }
      }
    }

    void setArmed(boolean armed, String sharedSessionId);

    String triggerManual();

    TriggeredSession triggerMissedShot();

    void triggerPoseArm(PosePeerArmClient.Candidate candidate);

    boolean poseLeaderHilEnabled();

    String triggerPoseLeaderHil();

    JSONObject poseStatus();
  }

  private final Context context;
  private final NodeConfiguration configuration;
  private final CaptureRuntime runtime;
  private final NodeCoordinationState coordination;
  private final CoordinationRecordStore coordinationRecords;
  private final CoordinationHttpEndpoint coordinationEndpoint;
  private final SessionDiagnosticStore diagnosticStore;
  private final DiagnosticExportWorkspaceManager diagnosticExportWorkspaces;
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
    this.diagnosticExportWorkspaces =
        new DiagnosticExportWorkspaceManager(this.context.getCacheDir());
    this.captureControl = captureControl;
    this.port = port;
  }

  public void start() throws IOException {
    if (serverSocket != null) {
      return;
    }
    diagnosticExportWorkspaces.cleanupStaleWorkspaces();
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
    } catch (Throwable failure) {
      // A malformed or disconnected client must not terminate the listener, but retain enough
      // evidence to diagnose device-specific failures without logging request contents or tokens.
      Log.e(TAG, "Node HTTP client failed", failure);
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
    if (request.method.equals("PUT")) {
      if (request.path.equals("/api/v1/setup")) {
        routeSetupUpdate(request, output);
      } else {
        writeJson(output, 405, "Method Not Allowed", errorJson("method not allowed"), false);
      }
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
    if (request.path.equals("/api/v1/setup")) {
      if (!requireAuthentication(request, output, head)) {
        return;
      }
      writeJson(output, 200, "OK", setupResponse(configuration.stationConfiguration()), head);
      return;
    }
    if (request.path.equals("/api/v1/node")) {
      JSONObject node = new JSONObject();
      node.put("schema_version", 1);
      node.put("node_id", configuration.nodeId());
      node.put("role", configuration.role().wireName());
      node.put("capture_profile", configuration.captureProfile().wireName());
      PoseStationConfigurationSnapshot poseConfiguration =
          configuration.poseConfigurationSnapshot();
      node.put(
          "pose",
          new JSONObject()
              .put("mode", poseConfiguration.mode().wireName())
              .put("delegate", poseConfiguration.delegateWireName())
              .put("debug_evidence_enabled", poseConfiguration.debugEvidenceEnabled())
              .put("peer_configured", poseConfiguration.hasPeer()));
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
    diagnosticExportWorkspaces.cleanupStaleWorkspaces();
    File workspace = diagnosticExportWorkspaces.createWorkspace();
    try {
      File stagedSession = new File(workspace, sessionId);
      if (!stagedSession.mkdir()) {
        throw new IOException("unable to create diagnostic export staging directory");
      }
      ensureDiagnosticIncident(session, sessionId);
      stageDiagnosticTree(session, session, stagedSession);
      stageCoordinationRecord(session, sessionId, stagedSession);
      return workspace;
    } catch (IOException | RuntimeException | Error failure) {
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
      if (manifest.optString("session_kind", "capture").equals("standby_diagnostic")) {
        throw new IllegalArgumentException(
            "standby diagnostic sessions do not contain high-speed video timing");
      }
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
      if (manifest.optString("session_kind", "capture").equals("standby_diagnostic")) {
        JSONObject evidence = manifest.getJSONObject("evidence");
        return diagnosticAudioObjectTimingBounds(evidence.getJSONObject("audio"));
      }
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
      return diagnosticAudioObjectTimingBounds(diagnosticEvidence.getJSONObject("audio"));
    } catch (org.json.JSONException | ArithmeticException malformed) {
      throw new IOException("diagnostic audio timing metadata is malformed", malformed);
    }
  }

  private static DiagnosticTimingBounds diagnosticAudioObjectTimingBounds(JSONObject audio)
      throws IOException {
    try {
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
    diagnosticExportWorkspaces.releaseWorkspace(workspace);
  }

  private static String diagnosticContentDisposition(String sessionId) {
    return "attachment; filename=\"swing-capture-" + sessionId + ".zip\"";
  }

  private record SetupUpdate(
      long expectedRevision,
      CaptureRole role,
      CaptureProfile captureProfile,
      PoseStationConfigurationSnapshot pose) {}

  private record PeerSetupProbe(
      boolean reachable, String nodeId, String role, String poseMode, String diagnostic) {
    private PeerSetupProbe {
      nodeId = nodeId == null ? "" : nodeId;
      role = role == null ? "" : role;
      poseMode = poseMode == null ? "" : poseMode;
      diagnostic = diagnostic == null ? "" : diagnostic;
    }
  }

  private void routeSetupUpdate(HttpRequest request, OutputStream output) throws Exception {
    if (!requireAuthentication(request, output, false)) {
      return;
    }
    try {
      CaptureRuntime.State captureState = runtime.snapshot().state();
      if (!CaptureConfigurationPolicy.mayChange(captureState)) {
        throw new IllegalStateException("Stop capture before editing phone setup");
      }
      NodeConfiguration.StationConfiguration current = configuration.stationConfiguration();
      SetupUpdate update = parseSetupUpdate(request.bodyText(), current.pose());
      PeerSetupProbe peer =
          update.pose().hasPeer()
              ? probePeer(update.pose().peerOrigin())
              : new PeerSetupProbe(false, "", "", "", "peer is not configured");
      if (update.pose().hasPeer()
          && (isDirectSelfOrigin(update.pose().peerOrigin())
              || NodeSetupPolicy.isSameNode(
                  current.capture().nodeId(), peer.reachable(), peer.nodeId()))) {
        throw new IllegalArgumentException("Peer origin resolves to this phone");
      }
      NodeSetupPolicy.PeerTopologyIssue topologyIssue =
          NodeSetupPolicy.peerTopologyIssue(
              current.capture().nodeId(),
              update.role().wireName(),
              update.pose().mode().wireName(),
              update.pose().hasPeer(),
              peer.reachable(),
              peer.nodeId(),
              peer.role(),
              peer.poseMode());
      if (topologyIssue != NodeSetupPolicy.PeerTopologyIssue.NONE) {
        throw new IllegalArgumentException(
            peerTopologyMessage(topologyIssue, update.pose().peerOrigin(), peer.diagnostic()));
      }
      NodeConfiguration.StationConfiguration committed =
          configuration.updateSetupConfiguration(
              update.expectedRevision(),
              update.role(),
              update.captureProfile(),
              update.pose());
      writeJson(output, 200, "OK", setupResponse(committed), false);
    } catch (NodeConfiguration.RevisionMismatchException stale) {
      writeJson(output, 409, "Conflict", errorJson(stale.getMessage()), false);
    } catch (org.json.JSONException | IllegalArgumentException malformed) {
      writeJson(output, 400, "Bad Request", errorJson(malformed.getMessage()), false);
    } catch (IllegalStateException rejected) {
      writeJson(output, 409, "Conflict", errorJson(rejected.getMessage()), false);
    }
  }

  private static SetupUpdate parseSetupUpdate(
      String bodyText, PoseStationConfigurationSnapshot currentPose) throws Exception {
    JSONObject body = new JSONObject(bodyText);
    requireExactFields(body, Set.of("schema_version", "expected_revision", "configuration"), "setup");
    if (strictJsonInteger(body, "schema_version") != NodeSetupPolicy.SCHEMA_VERSION) {
      throw new IllegalArgumentException("schema_version must be 1");
    }
    long expectedRevision = strictJsonInteger(body, "expected_revision");
    if (expectedRevision < 0) {
      throw new IllegalArgumentException("expected_revision cannot be negative");
    }

    JSONObject requested = body.getJSONObject("configuration");
    requireExactFields(requested, Set.of("role", "capture_profile", "pose"), "configuration");
    CaptureRole role = CaptureRole.parse(strictString(requested, "role"));
    CaptureProfile profile = CaptureProfile.parse(strictString(requested, "capture_profile"));

    JSONObject pose = requested.getJSONObject("pose");
    requireExactFields(
        pose,
        Set.of(
            "mode",
            "inference_delegate",
            "debug_evidence_enabled",
            "hitting_region",
            "peer_update"),
        "pose configuration");
    PoseNodeMode mode = PoseNodeMode.parse(strictString(pose, "mode"));
    PoseInferenceDelegatePolicy delegate =
        PoseStationConfigurationSnapshot.parseDelegatePolicy(
            strictString(pose, "inference_delegate"));
    boolean debugEvidence = strictBoolean(pose, "debug_evidence_enabled");
    JSONObject region = pose.getJSONObject("hitting_region");
    requireExactFields(region, Set.of("left", "top", "right", "bottom"), "hitting region");
    NormalizedHittingRegion hittingRegion =
        new NormalizedHittingRegion(
            strictFiniteNumber(region, "left"),
            strictFiniteNumber(region, "top"),
            strictFiniteNumber(region, "right"),
            strictFiniteNumber(region, "bottom"));

    JSONObject peerUpdate = pose.getJSONObject("peer_update");
    String operation = strictString(peerUpdate, "operation");
    NodeSetupPolicy.PeerCredentials currentPeer =
        new NodeSetupPolicy.PeerCredentials(currentPose.peerOrigin(), currentPose.peerControlToken());
    NodeSetupPolicy.PeerCredentials updatedPeer;
    switch (operation) {
      case "keep" -> {
        requireExactFields(peerUpdate, Set.of("operation"), "peer keep update");
        updatedPeer = NodeSetupPolicy.updatePeer(currentPeer, operation, null, null);
      }
      case "clear" -> {
        requireExactFields(peerUpdate, Set.of("operation"), "peer clear update");
        updatedPeer = NodeSetupPolicy.updatePeer(currentPeer, operation, null, null);
      }
      case "replace" -> {
        requireExactFields(
            peerUpdate,
            Set.of("operation", "origin", "control_token"),
            "peer replacement");
        updatedPeer =
            NodeSetupPolicy.updatePeer(
                currentPeer,
                operation,
                strictString(peerUpdate, "origin"),
                strictString(peerUpdate, "control_token"));
      }
      default -> throw new IllegalArgumentException("Unknown peer update operation: " + operation);
    }
    NodeSetupPolicy.requireOutboundPeerAllowed(mode.wireName(), updatedPeer);
    return new SetupUpdate(
        expectedRevision,
        role,
        profile,
        new PoseStationConfigurationSnapshot(
            mode,
            delegate,
            hittingRegion,
            debugEvidence,
            updatedPeer.origin(),
            updatedPeer.controlToken()));
  }

  private JSONObject setupResponse(NodeConfiguration.StationConfiguration station)
      throws Exception {
    CaptureConfigurationSnapshot capture = station.capture();
    PoseStationConfigurationSnapshot pose = station.pose();
    JSONObject response =
        new JSONObject()
            .put("schema_version", NodeSetupPolicy.SCHEMA_VERSION)
            .put("revision", station.revision())
            .put(
                "node",
                new JSONObject()
                    .put("node_id", capture.nodeId())
                    .put("service_urls", new JSONArray(advertisedUrls()))
                    .put("device_model", Build.MODEL))
            .put("capabilities", setupCapabilities())
            .put("configuration", setupConfiguration(capture, pose));

    CaptureRuntime.Snapshot runtimeSnapshot = runtime.snapshot();
    boolean editable = CaptureConfigurationPolicy.mayChange(runtimeSnapshot.state());
    JSONArray issues = new JSONArray();
    if (!editable) {
      issues.put("Stop capture before editing phone setup.");
    }
    if (capture.role() == CaptureRole.UNASSIGNED) {
      issues.put("Assign this phone a camera role before arming capture.");
    }
    PeerSetupProbe peer =
        pose.hasPeer()
            ? probePeer(pose.peerOrigin())
            : new PeerSetupProbe(false, "", "", "", "peer is not configured");
    boolean directSelf = pose.hasPeer() && isDirectSelfOrigin(pose.peerOrigin());
    NodeSetupPolicy.PeerTopologyIssue topologyIssue =
        directSelf
            ? NodeSetupPolicy.PeerTopologyIssue.SAME_NODE
            : NodeSetupPolicy.peerTopologyIssue(
                capture.nodeId(),
                capture.role().wireName(),
                pose.mode().wireName(),
                pose.hasPeer(),
                peer.reachable(),
                peer.nodeId(),
                peer.role(),
                peer.poseMode());
    if (topologyIssue != NodeSetupPolicy.PeerTopologyIssue.NONE) {
      issues.put(peerTopologyMessage(topologyIssue, pose.peerOrigin(), peer.diagnostic()));
    }
    response
        .put(
            "readiness",
            new JSONObject()
                .put("editable", editable)
                .put("capture_state", webState(runtimeSnapshot))
                .put("issues", issues))
        .put(
            "preview",
            new JSONObject().put("available", false).put("url", JSONObject.NULL));
    return response;
  }

  private static JSONObject setupCapabilities() throws Exception {
    JSONArray roles = new JSONArray();
    for (CaptureRole role : CaptureRole.values()) {
      roles.put(new JSONObject().put("value", role.wireName()).put("label", role.displayName()));
    }
    JSONArray profiles = new JSONArray();
    for (CaptureProfile profile : CaptureProfile.values()) {
      profiles.put(
          new JSONObject()
              .put("value", profile.wireName())
              .put("label", profile.displayName())
              .put("width", profile.width())
              .put("height", profile.height())
              .put("fps", 240));
    }
    JSONArray modes = new JSONArray();
    for (PoseNodeMode mode : PoseNodeMode.values()) {
      modes.put(new JSONObject().put("value", mode.wireName()).put("label", mode.displayName()));
    }
    JSONArray delegates = new JSONArray();
    for (PoseInferenceDelegatePolicy delegate : PoseInferenceDelegatePolicy.values()) {
      delegates.put(
          new JSONObject()
              .put("value", delegate.name().toLowerCase(Locale.ROOT))
              .put("label", inferenceDelegateLabel(delegate)));
    }
    return new JSONObject()
        .put("roles", roles)
        .put("capture_profiles", profiles)
        .put("pose_modes", modes)
        .put("inference_delegates", delegates);
  }

  private static String inferenceDelegateLabel(PoseInferenceDelegatePolicy delegate) {
    return switch (delegate) {
      case CPU_ONLY -> "CPU only";
      case GPU_PREFERRED -> "GPU preferred";
      case GPU_REQUIRED -> "GPU required";
    };
  }

  private static JSONObject setupConfiguration(
      CaptureConfigurationSnapshot capture, PoseStationConfigurationSnapshot pose)
      throws Exception {
    NodeSetupPolicy.RedactedPeer redactedPeer =
        NodeSetupPolicy.redact(
                new NodeSetupPolicy.PeerCredentials(
                    pose.peerOrigin(), pose.peerControlToken()))
            .orElse(null);
    JSONObject peer =
        redactedPeer == null ? null : new JSONObject().put("origin", redactedPeer.origin());
    return new JSONObject()
        .put("role", capture.role().wireName())
        .put("capture_profile", capture.profile().wireName())
        .put(
            "pose",
            new JSONObject()
                .put("mode", pose.mode().wireName())
                .put("inference_delegate", pose.delegateWireName())
                .put("debug_evidence_enabled", pose.debugEvidenceEnabled())
                .put(
                    "hitting_region",
                    new JSONObject()
                        .put("left", pose.hittingRegion().left())
                        .put("top", pose.hittingRegion().top())
                        .put("right", pose.hittingRegion().right())
                        .put("bottom", pose.hittingRegion().bottom()))
                .put("peer", peer == null ? JSONObject.NULL : peer));
  }

  private boolean isDirectSelfOrigin(String origin) {
    return NodeSetupPolicy.isDirectSelfOrigin(origin, port, advertisedUrls());
  }

  private static PeerSetupProbe probePeer(String origin) {
    HttpURLConnection connection = null;
    try {
      URI endpoint = URI.create(origin).resolve("/api/v1/node");
      connection = (HttpURLConnection) endpoint.toURL().openConnection();
      connection.setConnectTimeout(SETUP_PEER_TIMEOUT_MILLIS);
      connection.setReadTimeout(SETUP_PEER_TIMEOUT_MILLIS);
      connection.setRequestMethod("GET");
      int status = connection.getResponseCode();
      if (status != 200) {
        return new PeerSetupProbe(false, "", "", "", "peer returned HTTP " + status);
      }
      try (InputStream input = connection.getInputStream()) {
        byte[] body = input.readNBytes(SETUP_PEER_MAXIMUM_RESPONSE_BYTES + 1);
        if (body.length > SETUP_PEER_MAXIMUM_RESPONSE_BYTES) {
          return new PeerSetupProbe(false, "", "", "", "peer response is too large");
        }
        JSONObject node = new JSONObject(new String(body, StandardCharsets.UTF_8));
        if (strictJsonInteger(node, "schema_version") != 1) {
          return new PeerSetupProbe(false, "", "", "", "peer schema is unsupported");
        }
        String nodeId = strictString(node, "node_id");
        String role = CaptureRole.parse(strictString(node, "role")).wireName();
        JSONObject pose = node.getJSONObject("pose");
        String poseMode = PoseNodeMode.parse(strictString(pose, "mode")).wireName();
        return new PeerSetupProbe(true, nodeId, role, poseMode, "");
      }
    } catch (Exception failure) {
      return new PeerSetupProbe(false, "", "", "", failure.getClass().getSimpleName());
    } finally {
      if (connection != null) {
        connection.disconnect();
      }
    }
  }

  private static String peerTopologyMessage(
      NodeSetupPolicy.PeerTopologyIssue issue, String peerOrigin, String diagnostic) {
    return switch (issue) {
      case NON_LEADER_HAS_PEER ->
          "Only pose leader mode may configure an outbound peer association.";
      case LEADER_MISSING_PEER ->
          "Pose leader mode needs a peer phone origin and control token.";
      case PEER_UNREACHABLE ->
          "Pose leader peer is unavailable at "
              + peerOrigin
              + (diagnostic.isEmpty() ? "." : " (" + diagnostic + ").");
      case SAME_NODE -> "Peer origin resolves to this phone.";
      case LOCAL_ROLE_UNASSIGNED -> "Assign this pose leader a camera role.";
      case PEER_ROLE_UNASSIGNED -> "Assign the peer phone a camera role.";
      case SAME_ROLE -> "Pose leader and shadow phone must use distinct camera roles.";
      case PEER_NOT_SHADOW -> "Pose leader peer must use shadow mode.";
      case NONE -> throw new IllegalArgumentException("No peer topology error is present");
    };
  }

  private static void requireExactFields(JSONObject object, Set<String> fields, String label) {
    if (object.length() != fields.size()) {
      throw new IllegalArgumentException(label + " fields do not match schema 1");
    }
    for (String field : fields) {
      if (!object.has(field) || object.isNull(field)) {
        throw new IllegalArgumentException(label + " is missing " + field);
      }
    }
  }

  private static String strictString(JSONObject object, String field) throws Exception {
    Object value = object.get(field);
    if (!(value instanceof String text) || text.isBlank()) {
      throw new IllegalArgumentException(field + " must be a nonempty string");
    }
    return text;
  }

  private static long strictJsonInteger(JSONObject object, String field) throws Exception {
    Object value = object.get(field);
    if (!(value instanceof Integer) && !(value instanceof Long)) {
      throw new IllegalArgumentException(field + " must be a JSON integer");
    }
    return ((Number) value).longValue();
  }

  private static boolean strictBoolean(JSONObject object, String field) throws Exception {
    Object value = object.get(field);
    if (!(value instanceof Boolean result)) {
      throw new IllegalArgumentException(field + " must be a boolean");
    }
    return result;
  }

  private static double strictFiniteNumber(JSONObject object, String field) throws Exception {
    Object value = object.get(field);
    if (!(value instanceof Number number) || !Double.isFinite(number.doubleValue())) {
      throw new IllegalArgumentException(field + " must be a finite number");
    }
    return number.doubleValue();
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
        CaptureControl.TriggeredSession triggered = captureControl.triggerMissedShot();
        JSONObject summary = new JSONObject();
        summary.put("session_id", triggered.sessionId());
        summary.put("state", "waiting_post_roll");
        summary.put("created_at_utc", java.time.Instant.now().toString());
        summary.put("session_kind", triggered.sessionKind());
        summary.put("error", "");
        writeJson(output, 202, "Accepted", summary, false);
        return;
      }
      if (request.path.equals("/api/v1/capture/pose-arm")) {
        PosePeerArmClient.Candidate candidate = parsePoseArmCandidate(request.bodyText());
        captureControl.triggerPoseArm(candidate);
        writeJson(output, 202, "Accepted", captureStatus(), false);
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
      if (request.path.equals("/api/v1/hil/pose-arm")) {
        PoseHilEndpointAccess.requireEnabled(captureControl.poseLeaderHilEnabled());
        if (request.body.length != 0) {
          throw new IllegalArgumentException("HIL pose-arm request body must be empty");
        }
        String sharedSessionId = captureControl.triggerPoseLeaderHil();
        writeJson(
            output,
            202,
            "Accepted",
            new JSONObject()
                .put("schema_version", 1)
                .put("shared_session_id", sharedSessionId)
                .put("state", "transitioning_to_high_speed"),
            false);
        return;
      }
      writeJson(output, 404, "Not Found", errorJson("not found"), false);
    } catch (org.json.JSONException | IllegalArgumentException malformed) {
      writeJson(output, 400, "Bad Request", errorJson(malformed.getMessage()), false);
    } catch (IllegalStateException rejected) {
      writeJson(output, 409, "Conflict", errorJson(rejected.getMessage()), false);
    }
  }

  private static PosePeerArmClient.Candidate parsePoseArmCandidate(String bodyText)
      throws org.json.JSONException {
    JSONObject body = new JSONObject(bodyText);
    Set<String> expected =
        Set.of(
            "schema_version",
            "shared_session_id",
            "leader_node_id",
            "candidate_elapsed_realtime_ns",
            "person_confidence",
            "address_confidence");
    if (body.length() != expected.size()) {
      throw new IllegalArgumentException("pose-arm request fields do not match schema 1");
    }
    for (String field : expected) {
      if (!body.has(field) || body.isNull(field)) {
        throw new IllegalArgumentException("pose-arm request is missing " + field);
      }
    }
    if (body.getInt("schema_version") != 1) {
      throw new IllegalArgumentException("pose-arm schema_version must be 1");
    }
    String timestampText = body.getString("candidate_elapsed_realtime_ns");
    final long timestamp;
    try {
      timestamp = Long.parseLong(timestampText);
    } catch (NumberFormatException malformed) {
      throw new IllegalArgumentException("candidate timestamp is not a signed 64-bit integer");
    }
    if (!Long.toString(timestamp).equals(timestampText)) {
      throw new IllegalArgumentException("candidate timestamp is not canonical decimal");
    }
    return new PosePeerArmClient.Candidate(
        body.getString("shared_session_id"),
        body.getString("leader_node_id"),
        timestamp,
        body.getDouble("person_confidence"),
        body.getDouble("address_confidence"));
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
    status.put("pose", captureControl.poseStatus());
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
      json.put("session_kind", summary.sessionKind);
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
        String sessionKind = manifest.optString("session_kind", "capture");
        String createdAtUtc;
        if (sessionKind.equals("standby_diagnostic")) {
          String createdAtEpochMillis = manifest.getString("created_at_epoch_ms");
          long epochMillis = Long.parseLong(createdAtEpochMillis);
          if (!Long.toString(epochMillis).equals(createdAtEpochMillis) || epochMillis <= 0) {
            continue;
          }
          createdAtUtc = Instant.ofEpochMilli(epochMillis).toString();
        } else if (sessionKind.equals("capture")) {
          createdAtUtc = Instant.parse(manifest.getString("created_at_utc")).toString();
        } else {
          continue;
        }
        summaries.add(
            new SessionSummary(
                manifest.getString("session_id"), createdAtUtc, sessionKind));
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
    headers.append("Access-Control-Allow-Methods: GET, HEAD, POST, PUT, OPTIONS\r\n");
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
    private final String sessionKind;

    private SessionSummary(String sessionId, String createdAtUtc, String sessionKind) {
      this.sessionId = sessionId;
      this.createdAtUtc = createdAtUtc;
      this.sessionKind = sessionKind;
    }
  }
}
