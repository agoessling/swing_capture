package com.agoessling.swingcapture;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.nsd.NsdManager;
import android.os.Build;
import android.os.PowerManager;
import android.os.SystemClock;
import android.os.UserManager;
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
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
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
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
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
  // Match arm/clock transport tolerance: screen-off Wi-Fi can show sub-second wake latency.
  private static final int SETUP_PEER_TIMEOUT_MILLIS = 1_500;
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

    record SetupPreview(
        String state,
        String reason,
        long generation,
        long frameAgeNanos,
        int imageRotationDegrees,
        byte[] jpeg) {
      public SetupPreview {
        if (jpeg == null) {
          throw new IllegalArgumentException("setup preview JPEG is required");
        }
        if (!Set.of("available", "stale", "unavailable").contains(state)) {
          throw new IllegalArgumentException("setup preview state is invalid");
        }
        if (reason == null || reason.isBlank() || generation < 0 || frameAgeNanos < -1) {
          throw new IllegalArgumentException("setup preview metadata is invalid");
        }
        CameraImageRotation.fromSensorOrientation(imageRotationDegrees);
        if ((state.equals("available") || state.equals("stale")) != (jpeg.length > 0)) {
          throw new IllegalArgumentException("setup preview state and JPEG disagree");
        }
        jpeg = jpeg.clone();
      }

      @Override
      public byte[] jpeg() {
        return jpeg.clone();
      }

      boolean hasFrame() {
        return jpeg.length > 0;
      }

      boolean available() {
        return state.equals("available");
      }
    }

    void setArmed(boolean armed, String sharedSessionId);

    String triggerManual();

    TriggeredSession triggerMissedShot();

    boolean triggerPoseArm(PosePeerArmClient.Candidate candidate);

    void triggerPoseImpact(PosePeerArmClient.ImpactTrigger trigger);

    boolean poseLeaderHilEnabled();

    String triggerPoseLeaderHil();

    JSONObject poseStatus();

    SetupPreview setupPreview();

    JSONObject fieldRecordingStatus();

    JSONObject startFieldRecording(String sharedRecordingId);

    JSONObject stopFieldRecording();
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
  private final DeviceCapabilityPolicy.HardwareSnapshot deviceCapabilities;
  private final int port;
  private final Optional<LanNodeDiscovery> lanDiscovery;
  private final AtomicBoolean stopping = new AtomicBoolean();
  private final String liveStatusStreamId = UUID.randomUUID().toString();
  private final AtomicLong liveStatusRevision = new AtomicLong();
  // Chrome held seven or eight independent Range requests for the paired review player during the
  // physical release gate. Four workers—and then eight—let slow media readers occupy the entire
  // server, starving status and immutable-catalog responses. Twelve retains four control-plane
  // slots under that observed player workload while remaining a small, fixed per-phone bound.
  private final ExecutorService clients = Executors.newFixedThreadPool(12);
  private ServerSocket serverSocket;
  private Thread acceptThread;

  public NodeHttpServer(
      Context context,
      NodeConfiguration configuration,
      CaptureRuntime runtime,
      NodeCoordinationState coordination,
      CoordinationRecordStore coordinationRecords,
      CaptureControl captureControl,
      DeviceCapabilityPolicy.HardwareSnapshot deviceCapabilities,
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
    this.deviceCapabilities = Objects.requireNonNull(deviceCapabilities, "deviceCapabilities");
    this.port = port;
    NsdManager nsdManager = this.context.getSystemService(NsdManager.class);
    this.lanDiscovery =
        nsdManager == null
            ? Optional.empty()
            : Optional.of(
                new AndroidNsdLanNodeDiscovery(
                    nsdManager, this.context.getMainExecutor(), new LanNodeDiscoveryRegistry()));
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
    lanDiscovery.ifPresent(
        discovery -> {
          try {
            discovery.start(discoveryAdvertisement());
          } catch (RuntimeException unavailable) {
            Log.w(TAG, "LAN discovery is unavailable", unavailable);
          }
        });
    acceptThread = new Thread(this::acceptConnections, "node-http-accept");
    acceptThread.start();
  }

  public void stop() {
    stopping.set(true);
    lanDiscovery.ifPresent(
        discovery -> {
          try {
            discovery.close();
          } catch (RuntimeException unavailable) {
            Log.w(TAG, "Unable to stop LAN discovery cleanly", unavailable);
          }
        });
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

  private LanNodeDiscovery.Advertisement discoveryAdvertisement() {
    CaptureConfigurationSnapshot capture = configuration.captureSnapshot();
    return new LanNodeDiscovery.Advertisement(
        capture.nodeId(), capture.role().wireName(), Build.MODEL, port);
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
      NodeHttpRequest request =
          NodeHttpRequestParser.read(
              input, MAXIMUM_REQUEST_HEADER_BYTES, MAXIMUM_REQUEST_BODY_BYTES);
      route(request, output);
    } catch (Throwable failure) {
      // A malformed or disconnected client must not terminate the listener, but retain enough
      // evidence to diagnose device-specific failures without logging request contents or tokens.
      Log.e(TAG, "Node HTTP client failed", failure);
    }
  }

  private void route(NodeHttpRequest request, OutputStream output) throws Exception {
    long requestReceivedElapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos();
    boolean head = request.method.equals("HEAD");
    if (request.method.equals("OPTIONS")) {
      writeHeaders(output, 204, "No Content", "text/plain", 0, Collections.emptyMap());
      return;
    }
    if (NodeHttpProtocol.requiresControlCredential(request.method, request.path)
        && !requireAuthentication(request, output, head)) {
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
    if (!request.method.equals("GET") && !head) {
      writeJson(output, 405, "Method Not Allowed", errorJson("method not allowed"), false);
      return;
    }
    if (isCoordinationPath(request.path)) {
      routeCoordination(request, output, head);
      return;
    }
    if (request.path.equals("/api/v1/setup/preview")) {
      CaptureControl.SetupPreview preview = captureControl.setupPreview();
      if (!preview.available()) {
        writeJson(
            output,
            503,
            "Service Unavailable",
            errorJson("setup preview is " + preview.reason().replace('_', ' ')),
            head,
            Map.of("Cache-Control", "no-store, max-age=0"));
        return;
      }
      writeBytes(
          output,
          200,
          "OK",
          "image/jpeg",
          preview.jpeg(),
          head,
          Map.of(
              "Cache-Control", "no-store, max-age=0",
              "X-Content-Type-Options", "nosniff"));
      return;
    }
    if (request.path.equals("/api/v1/setup")) {
      writeJson(output, 200, "OK", setupResponse(configuration.stationConfiguration()), head);
      return;
    }
    if (request.path.equals("/api/v1/pairing/identity")) {
      PoseStationConfigurationSnapshot pose = configuration.poseConfigurationSnapshot();
      writeJson(
          output,
          200,
          "OK",
          new JSONObject()
              .put("schema_version", 1)
              .put("node_id", configuration.nodeId())
              .put("role", configuration.role().wireName())
              .put("pose_mode", pose.mode().wireName())
              .put(
                  "control_credential_generation",
                  configuration.stationConfiguration().controlCredentialGeneration())
              .put("label", Build.MODEL)
              .put("service_urls", new JSONArray(advertisedUrls())),
          head);
      return;
    }
    if (request.path.equals("/api/v1/discovery")) {
      JSONArray observations = new JSONArray();
      long now = System.currentTimeMillis();
      for (LanNodeDiscovery.Observation observation :
          lanDiscovery.map(value -> value.snapshot(now)).orElseGet(List::of)) {
        if (isDirectSelfOrigin(observation.origin())) {
          continue;
        }
        observations.put(
            new JSONObject()
                .put("service_instance", observation.serviceInstance())
                .put("node_id", observation.nodeId())
                .put("role", observation.role())
                .put("label", observation.label())
                .put("origin", observation.origin())
                .put("observed_at_epoch_ms", observation.observedAtEpochMillis())
                .put("expires_at_epoch_ms", observation.expiresAtEpochMillis())
                .put("trusted", false));
      }
      writeJson(
          output,
          200,
          "OK",
          new JSONObject()
              .put("schema_version", 1)
              .put("authentication_required_for_pairing", true)
              .put("observations", observations),
          head);
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
    if (request.path.equals(NodeHttpProtocol.CAPTURE_STATUS_PATH)) {
      writeJson(
          output,
          200,
          "OK",
          captureStatus(),
          head,
          Map.of("Cache-Control", "no-store, max-age=0"));
      return;
    }
    if (request.path.equals("/api/v1/field-recording/status")) {
      writeJson(output, 200, "OK", captureControl.fieldRecordingStatus(), head);
      return;
    }
    if (request.path.equals("/api/v1/field-recordings")) {
      writeJson(
          output,
          200,
          "OK",
          fieldRecordingList(),
          head,
          Map.of("Cache-Control", "private, no-store"));
      return;
    }
    if (request.path.equals(NodeHttpProtocol.PUBLIC_CLOCK_HINT_PATH)) {
      JSONObject clock = new JSONObject();
      clock.put("schema_version", 1);
      // This value lets a caller match the hint to an identity learned through the authenticated
      // pairing endpoint. It is not itself authenticated merely because it appears here.
      clock.put("node_id", configuration.nodeId());
      clock.put(
          "request_received_elapsed_realtime_ns",
          Long.toString(requestReceivedElapsedRealtimeNanos));
      clock.put(
          "response_prepared_elapsed_realtime_ns",
          Long.toString(SystemClock.elapsedRealtimeNanos()));
      writeJson(
          output,
          200,
          "OK",
          clock,
          head,
          NodeHttpProtocol.publicClockHintResponseHeaders());
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

    NodeHttpProtocol.MediaRoute mediaRoute =
        NodeHttpProtocol.mediaRoute(request.method, request.path);
    if (mediaRoute != null) {
      if (!NodeHttpProtocol.hasValidMediaCredential(
          request.headers.get("authorization"),
          request.query,
          mediaRoute,
          configuration.controlToken())) {
        NodeHttpResponseWriter.writeMediaUnauthorized(output, head);
        return;
      }
      File collection = new File(context.getFilesDir(), mediaRoute.collectionDirectory());
      File artifact = new File(new File(collection, mediaRoute.identifier()), mediaRoute.fileName());
      Map<String, String> mediaHeaders =
          mediaRoute.kind() == NodeHttpProtocol.MediaKind.SESSION_MANIFEST
              ? Map.of(
                  MediaAccessAuthorization.RESPONSE_HEADER,
                  NodeHttpProtocol.mediaAccessQuery(mediaRoute, configuration.controlToken()),
                  "Cache-Control",
                  "private, no-store")
              : Map.of();
      serveFile(request, output, artifact, mediaRoute.contentType(), head, mediaHeaders);
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
      if (artifact.equals("diagnostics.zip")) {
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

  private boolean serveStaticAsset(NodeHttpRequest request, OutputStream output, boolean head)
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
      PoseStationConfigurationSnapshot pose,
      String peerOperation) {}

  private record PeerSetupProbe(
      boolean reachable,
      String nodeId,
      String role,
      String poseMode,
      String label,
      long controlCredentialGeneration,
      String credentialStatus,
      String diagnostic,
      PeerSetupVerificationRetrier.Disposition disposition) {
    private PeerSetupProbe {
      nodeId = nodeId == null ? "" : nodeId;
      role = role == null ? "" : role;
      poseMode = poseMode == null ? "" : poseMode;
      label = label == null ? "" : label;
      if (controlCredentialGeneration < 0) {
        throw new IllegalArgumentException("peer credential generation cannot be negative");
      }
      credentialStatus = credentialStatus == null ? "unavailable" : credentialStatus;
      diagnostic = diagnostic == null ? "" : diagnostic;
      java.util.Objects.requireNonNull(disposition, "disposition");
      if (reachable != (disposition == PeerSetupVerificationRetrier.Disposition.VERIFIED)) {
        throw new IllegalArgumentException("peer probe reachability and disposition disagree");
      }
    }
  }

  /** Rejects a downgrade before the replacement credential could be exposed by a probe. */
  private static void requireSafePeerTransportTransition(
      NodeConfiguration.StationConfiguration current, SetupUpdate update) {
    if (!update.pose().hasPeer() || current.pairing().isEmpty()) {
      return;
    }
    PeerPairingBinding binding = current.pairing().orElseThrow();
    if (binding.state() == PeerPairingBinding.State.ACTIVE) {
      PeerTransportSecurityPolicy.requireNoActiveProtectionDowngrade(
          binding.origin(), update.pose().peerOrigin());
    }
  }

  private void routeSetupUpdate(NodeHttpRequest request, OutputStream output) throws Exception {
    try {
      CaptureRuntime.State captureState = runtime.snapshot().state();
      if (!CaptureConfigurationPolicy.mayChange(captureState)) {
        throw new IllegalStateException("Stop capture before editing phone setup");
      }
      NodeConfiguration.StationConfiguration current = configuration.stationConfiguration();
      SetupUpdate update = parseSetupUpdate(request.bodyText(), current.pose());
      requireSafePeerTransportTransition(current, update);
      PeerSetupProbe peer =
          update.pose().hasPeer()
              ? probePeerForSetup(update.pose().peerOrigin(), update.pose().peerControlToken())
              : new PeerSetupProbe(
                  false,
                  "",
                  "",
                  "",
                  "",
                  0,
                  "not_configured",
                  "peer is not configured",
                  PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
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
      Optional<PeerPairingBinding> pairing =
          pairingAfterVerifiedUpdate(current, update, peer, System.currentTimeMillis());
      NodeConfiguration.StationConfiguration committed =
          configuration.updateSetupConfiguration(
              update.expectedRevision(),
              update.role(),
              update.captureProfile(),
              update.pose(),
              pairing);
      lanDiscovery.ifPresent(
          discovery -> {
            try {
              discovery.updateAdvertisement(discoveryAdvertisement());
            } catch (RuntimeException unavailable) {
              Log.w(TAG, "Unable to refresh LAN advertisement", unavailable);
            }
          });
      writeJson(output, 200, "OK", setupResponse(committed, peer), false);
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
            updatedPeer.controlToken()),
        operation);
  }

  private static Optional<PeerPairingBinding> pairingAfterVerifiedUpdate(
      NodeConfiguration.StationConfiguration current,
      SetupUpdate update,
      PeerSetupProbe peer,
      long nowEpochMillis) {
    Optional<PeerPairingBinding> existing = current.pairing();
    if (update.peerOperation().equals("clear")) {
      return existing.map(value -> value.revoke(nowEpochMillis));
    }
    if (!update.pose().hasPeer()) {
      return existing;
    }
    if (!peer.reachable()) {
      throw new IllegalArgumentException("Peer identity could not be authenticated");
    }
    if (update.peerOperation().equals("keep") && existing.isPresent()) {
      PeerPairingBinding bound = existing.orElseThrow();
      if (!bound.matchesVerifiedIdentity(peer.nodeId(), peer.role())
          || !bound.origin().equals(update.pose().peerOrigin())) {
        throw new IllegalStateException(
            "Peer identity or address changed; verify and replace the peer credential");
      }
      return existing;
    }
    return Optional.of(
        PeerPairingBinding.verifyAndActivate(
            existing.orElse(null),
            current.capture().nodeId(),
            peer.nodeId(),
            peer.role(),
            peer.label(),
            update.pose().peerOrigin(),
            nowEpochMillis));
  }

  private JSONObject setupResponse(NodeConfiguration.StationConfiguration station)
      throws Exception {
    PoseStationConfigurationSnapshot pose = station.pose();
    PeerSetupProbe peer =
        pose.hasPeer()
            ? probePeer(pose.peerOrigin(), pose.peerControlToken())
            : new PeerSetupProbe(
                false,
                "",
                "",
                "",
                "",
                0,
                "not_configured",
                "peer is not configured",
                PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
    return setupResponse(station, peer);
  }

  private JSONObject setupResponse(
      NodeConfiguration.StationConfiguration station, PeerSetupProbe peer) throws Exception {
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
                    .put(
                        "control_credential_generation",
                        station.controlCredentialGeneration())
                    .put("service_urls", new JSONArray(advertisedUrls()))
                    .put("device_model", Build.MODEL))
            .put("capabilities", setupCapabilities())
            .put("configuration", setupConfiguration(capture, pose))
            .put("pairing", pairingResponse(station.pairing(), pose, peer));
    response
        .put("device_admission", deviceAdmissionJson(capture.profile()))
        .put("reboot_recovery", unattendedRecoveryJson());

    CaptureRuntime.Snapshot runtimeSnapshot = runtime.snapshot();
    boolean editable = CaptureConfigurationPolicy.mayChange(runtimeSnapshot.state());
    JSONArray issues = new JSONArray();
    if (!editable) {
      issues.put("Stop capture before editing phone setup.");
    }
    if (capture.role() == CaptureRole.UNASSIGNED) {
      issues.put("Assign this phone a camera role before arming capture.");
    }
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
    if (pose.hasPeer()
        && topologyIssue == NodeSetupPolicy.PeerTopologyIssue.NONE
        && (station.pairing().isEmpty()
            || !station.pairing().orElseThrow().matchesVerifiedIdentity(
                peer.nodeId(), peer.role())
            || !station.pairing().orElseThrow().origin().equals(pose.peerOrigin()))) {
      issues.put("Peer address is not covered by an active authenticated identity binding.");
    }
    NodeSetupPolicy.OperationalHealth operationalHealth = operationalHealth();
    for (String issue : operationalHealth.readinessIssues()) {
      issues.put(issue);
    }
    for (DeviceCapabilityPolicy.Issue issue :
        DeviceCapabilityPolicy.assess(currentDeviceCapabilities(), capture.profile()).issues()) {
      issues.put(issue.message());
    }
    CaptureControl.SetupPreview preview = captureControl.setupPreview();
    response
        .put(
            "readiness",
            new JSONObject()
                .put("editable", editable)
                .put("capture_state", webState(runtimeSnapshot))
                .put("issues", issues))
        .put("operational_health", operationalHealthJson(operationalHealth))
        .put(
            "preview",
            new JSONObject()
                .put("available", preview.available())
                .put(
                    "url",
                    preview.available() ? "/api/v1/setup/preview" : JSONObject.NULL)
                .put("state", preview.state())
                .put("reason", preview.reason())
                .put("generation", preview.generation())
                .put("image_rotation_degrees", preview.imageRotationDegrees())
                .put(
                    "frame_age_ms",
                    preview.frameAgeNanos() < 0
                        ? JSONObject.NULL
                        : preview.frameAgeNanos() / 1_000_000L));
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
        .put("inference_delegates", delegates)
        .put("product_floor", productFloorJson());
  }

  private static JSONObject productFloorJson() throws Exception {
    DeviceCapabilityPolicy.InferenceContract inference =
        DeviceCapabilityPolicy.inferenceContract();
    return new JSONObject()
        .put("minimum_api_level", DeviceCapabilityPolicy.MINIMUM_API_LEVEL)
        .put("minimum_opengl_es", "3.1")
        .put("audio", "48000_hz_mono_pcm16")
        .put("camera", "realtime_rear_fixed_240fps_with_yuv_standby")
        .put("encoder", "hardware_h264_240fps")
        .put(
            "pose",
            new JSONObject()
                .put("model", inference.model())
                .put("input_width", inference.inputWidth())
                .put("input_height", inference.inputHeight())
                .put("cadence_hz", inference.cadenceHz())
                .put("delegate_selection_scope", inference.delegateSelectionScope())
                .put(
                    "device_fallback_can_affect_peer",
                    inference.deviceFallbackCanAffectPeer()));
  }

  private static String inferenceDelegateLabel(PoseInferenceDelegatePolicy delegate) {
    return switch (delegate) {
      case CPU_ONLY -> "CPU only";
      case GPU_PREFERRED -> "GPU preferred";
      case GPU_REQUIRED -> "GPU required";
      case NPU_PREFERRED -> "NPU preferred (experimental)";
      case NPU_REQUIRED -> "NPU required (experimental)";
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
        redactedPeer == null
            ? null
            : new JSONObject()
                .put("origin", redactedPeer.origin())
                .put("transport_security", redactedPeer.transportSecurity());
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

  private static Object pairingResponse(
      Optional<PeerPairingBinding> pairing,
      PoseStationConfigurationSnapshot pose,
      PeerSetupProbe peer)
      throws Exception {
    if (pairing.isEmpty()) {
      return JSONObject.NULL;
    }
    PeerPairingBinding binding = pairing.orElseThrow();
    String credentialStatus = pairingCredentialStatus(binding, pose, peer);
    return new JSONObject()
        .put("state", binding.state().name().toLowerCase(Locale.ROOT))
        .put("peer_node_id", binding.peerNodeId())
        .put("expected_role", binding.expectedRole())
        .put("label", binding.label())
        .put("origin", binding.origin())
        .put(
            "transport_security",
            PeerTransportSecurityPolicy.classify(binding.origin()).wireName())
        .put("credential_generation", binding.credentialGeneration())
        .put("credential_status", credentialStatus)
        .put("verified_at_epoch_ms", binding.verifiedAtEpochMillis())
        .put("revoked_at_epoch_ms", binding.revokedAtEpochMillis());
  }

  private static String pairingCredentialStatus(
      PeerPairingBinding binding,
      PoseStationConfigurationSnapshot pose,
      PeerSetupProbe peer) {
    return PeerCredentialStatusPolicy.evaluate(
            binding.state() == PeerPairingBinding.State.REVOKED,
            pose.hasPeer(),
            pose.hasPeer() && binding.origin().equals(pose.peerOrigin()),
            peer.reachable(),
            peer.reachable() && binding.matchesVerifiedIdentity(peer.nodeId(), peer.role()),
            peer.credentialStatus().equals("authentication_failed"))
        .wireName();
  }

  private boolean isDirectSelfOrigin(String origin) {
    return NodeSetupPolicy.isDirectSelfOrigin(origin, port, advertisedUrls());
  }

  private static PeerSetupProbe probePeerForSetup(String origin, String controlToken) {
    // The revisioned configuration write happens only after this bounded probe sequence returns.
    // Retrying here cannot duplicate a setup mutation, and the later optimistic revision check
    // still rejects a concurrent update that commits while the peer is becoming reachable.
    return PeerSetupVerificationRetrier.execute(
        () -> {
          PeerSetupProbe peer = probePeer(origin, controlToken);
          return new PeerSetupVerificationRetrier.Result<>(peer, peer.disposition());
        });
  }

  private static PeerSetupProbe probePeer(String origin, String controlToken) {
    HttpURLConnection connection = null;
    try {
      if (!BearerAuthorization.isValidToken(controlToken)) {
        return new PeerSetupProbe(
            false,
            "",
            "",
            "",
            "",
            0,
            "authentication_failed",
            "peer credential is invalid",
            PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
      }
      URI endpoint = URI.create(origin).resolve("/api/v1/pairing/identity");
      connection = (HttpURLConnection) endpoint.toURL().openConnection();
      PeerSetupVerificationRetrier.configureAuthenticatedJsonGet(
          connection, "Bearer " + controlToken, SETUP_PEER_TIMEOUT_MILLIS);
      int status = connection.getResponseCode();
      if (status != 200) {
        PeerSetupVerificationRetrier.Disposition disposition =
            PeerSetupVerificationRetrier.dispositionForHttpStatus(status);
        return new PeerSetupProbe(
            false,
            "",
            "",
            "",
            "",
            0,
            status == 401 || status == 403 ? "authentication_failed" : "unavailable",
            "peer returned HTTP " + status,
            disposition);
      }
      try (InputStream input = connection.getInputStream()) {
        byte[] body = input.readNBytes(SETUP_PEER_MAXIMUM_RESPONSE_BYTES + 1);
        if (body.length > SETUP_PEER_MAXIMUM_RESPONSE_BYTES) {
          return new PeerSetupProbe(
              false,
              "",
              "",
              "",
              "",
              0,
              "unavailable",
              "peer response is too large",
              PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
        }
        JSONObject node = new JSONObject(new String(body, StandardCharsets.UTF_8));
        if (strictJsonInteger(node, "schema_version") != 1) {
          return new PeerSetupProbe(
              false,
              "",
              "",
              "",
              "",
              0,
              "unavailable",
              "peer schema is unsupported",
              PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
        }
        String nodeId = strictString(node, "node_id");
        String role = CaptureRole.parse(strictString(node, "role")).wireName();
        String poseMode = PoseNodeMode.parse(strictString(node, "pose_mode")).wireName();
        long controlCredentialGeneration =
            strictJsonInteger(node, "control_credential_generation");
        if (controlCredentialGeneration < 1) {
          return new PeerSetupProbe(
              false,
              "",
              "",
              "",
              "",
              0,
              "unavailable",
              "peer credential generation is invalid",
              PeerSetupVerificationRetrier.Disposition.TERMINAL_FAILURE);
        }
        String label = strictString(node, "label");
        return new PeerSetupProbe(
            true,
            nodeId,
            role,
            poseMode,
            label,
            controlCredentialGeneration,
            "verified",
            "",
            PeerSetupVerificationRetrier.Disposition.VERIFIED);
      }
    } catch (Exception failure) {
      return new PeerSetupProbe(
          false,
          "",
          "",
          "",
          "",
          0,
          "unavailable",
          failure.getClass().getSimpleName(),
          PeerSetupVerificationRetrier.dispositionForFailure(failure));
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

  private void routeControl(NodeHttpRequest request, OutputStream output) throws Exception {
    if (request.path.equals("/api/v1/control-credential/rotate")) {
      routeControlCredentialRotation(request, output);
      return;
    }
    try {
      if (isCoordinationPath(request.path)) {
        routeCoordinationAuthenticated(request, output, false);
        return;
      }
      if (request.path.equals("/api/v1/pairing/reset")) {
        JSONObject body = new JSONObject(request.bodyText());
        requireExactFields(
            body,
            Set.of("schema_version", "expected_revision", "peer_node_id"),
            "pairing reset");
        if (strictJsonInteger(body, "schema_version") != 1) {
          throw new IllegalArgumentException("schema_version must be 1");
        }
        NodeConfiguration.StationConfiguration reset =
            configuration.resetRevokedPairing(
                strictJsonInteger(body, "expected_revision"),
                strictString(body, "peer_node_id"));
        writeJson(output, 200, "OK", setupResponse(reset), false);
        return;
      }
      NodeHttpProtocol.ControlRoute captureRoute =
          NodeHttpProtocol.controlRoute(request.method, request.path);
      if (captureRoute != null
          && captureRoute.operation() == NodeHttpProtocol.ControlOperation.CAPTURE_ARM) {
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
        writeJson(output, captureRoute.successStatus(), "Accepted", captureStatus(), false);
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
      if (captureRoute != null
          && captureRoute.operation() == NodeHttpProtocol.ControlOperation.MISSED_SHOT) {
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
        writeJson(output, captureRoute.successStatus(), "Accepted", summary, false);
        return;
      }
      if (request.path.equals("/api/v1/capture/pose-arm")) {
        PosePeerArmClient.Candidate candidate = parsePoseArmCandidate(request.bodyText());
        boolean poseReady = captureControl.triggerPoseArm(candidate);
        writeJson(
            output,
            202,
            "Accepted",
            captureStatus(),
            false,
            Map.of(PosePeerArmClient.POSE_READY_HEADER, Boolean.toString(poseReady)));
        return;
      }
      if (request.path.equals("/api/v1/capture/pose-impact")) {
        PosePeerArmClient.ImpactTrigger trigger = parsePoseImpactTrigger(request.bodyText());
        captureControl.triggerPoseImpact(trigger);
        writeJson(output, 202, "Accepted", captureStatus(), false);
        return;
      }
      if (request.path.equals("/api/v1/field-recording/start")) {
        JSONObject body = new JSONObject(request.bodyText());
        Set<String> expected = Set.of("schema_version", "shared_recording_id");
        if (body.length() != expected.size()
            || body.optInt("schema_version", -1) != 1
            || !body.has("shared_recording_id")
            || body.isNull("shared_recording_id")) {
          throw new IllegalArgumentException("field-recording start fields do not match schema 1");
        }
        String sharedRecordingId = body.getString("shared_recording_id");
        if (!safeSegment(sharedRecordingId)) {
          throw new IllegalArgumentException("shared_recording_id is invalid");
        }
        writeJson(
            output,
            202,
            "Accepted",
            captureControl.startFieldRecording(sharedRecordingId),
            false);
        return;
      }
      if (request.path.equals("/api/v1/field-recording/stop")) {
        JSONObject body =
            request.body.length == 0 ? new JSONObject() : new JSONObject(request.bodyText());
        if (body.length() != 0) {
          throw new IllegalArgumentException("field-recording stop request body must be empty");
        }
        writeJson(
            output, 202, "Accepted", captureControl.stopFieldRecording(), false);
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

  private void routeControlCredentialRotation(NodeHttpRequest request, OutputStream output)
      throws Exception {
    try {
      JSONObject body = new JSONObject(request.bodyText());
      requireExactFields(
          body,
          Set.of("schema_version", "expected_revision", "confirmed_node_id"),
          "control credential rotation");
      if (strictJsonInteger(body, "schema_version") != 1) {
        throw new IllegalArgumentException("schema_version must be 1");
      }
      long expectedRevision = strictJsonInteger(body, "expected_revision");
      if (expectedRevision < 0) {
        throw new IllegalArgumentException("expected_revision cannot be negative");
      }
      NodeConfiguration.ControlCredentialRotation rotated =
          configuration.rotateControlCredential(
              request.headers.get("authorization"),
              expectedRevision,
              strictString(body, "confirmed_node_id"),
              CaptureConfigurationPolicy.mayChange(runtime.snapshot().state()));
      writeJson(
          output,
          200,
          "OK",
          new JSONObject()
              .put("schema_version", 1)
              .put("node_id", rotated.stationConfiguration().capture().nodeId())
              .put("setup_revision", rotated.stationConfiguration().revision())
              .put(
                  "control_credential_generation",
                  rotated.controlCredentialGeneration())
              .put("control_token", rotated.controlToken())
              .put("remote_peer_bindings_require_re_pair", true),
          false);
    } catch (ControlCredentialRotationPolicy.UnauthorizedException unauthorized) {
      writeJson(
          output,
          401,
          "Unauthorized",
          errorJson(unauthorized.getMessage()),
          false,
          Map.of("WWW-Authenticate", "Bearer"));
    } catch (ControlCredentialRotationPolicy.StaleRevisionException stale) {
      writeJson(output, 409, "Conflict", errorJson(stale.getMessage()), false);
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

  private static PosePeerArmClient.ImpactTrigger parsePoseImpactTrigger(String bodyText)
      throws org.json.JSONException {
    JSONObject body = new JSONObject(bodyText);
    int schemaVersion = body.optInt("schema_version", -1);
    Set<String> expected;
    if (schemaVersion == 1) {
      expected =
          Set.of(
              "schema_version",
              "shared_session_id",
              "leader_node_id",
              "leader_trigger_elapsed_realtime_ns");
    } else if (schemaVersion == 2) {
      expected =
          Set.of(
              "schema_version",
              "shared_session_id",
              "leader_node_id",
              "leader_trigger_elapsed_realtime_ns",
              "target_peer_node_id",
              "mapped_peer_trigger_elapsed_realtime_ns",
              "mapping_uncertainty_ns",
              "mapping_age_at_send_ns",
              "minimum_round_trip_ns",
              "maximum_round_trip_ns",
              "sample_count");
    } else {
      throw new IllegalArgumentException("pose-impact schema_version must be 1 or 2");
    }
    if (body.length() != expected.size()) {
      throw new IllegalArgumentException(
          "pose-impact request fields do not match schema " + schemaVersion);
    }
    for (String field : expected) {
      if (!body.has(field) || body.isNull(field)) {
        throw new IllegalArgumentException("pose-impact request is missing " + field);
      }
    }
    long leaderTimestamp =
        canonicalDecimalString(body, "leader_trigger_elapsed_realtime_ns");
    if (schemaVersion == 1) {
      return new PosePeerArmClient.ImpactTrigger(
          body.getString("shared_session_id"),
          body.getString("leader_node_id"),
          leaderTimestamp);
    }
    return PosePeerArmClient.ImpactTrigger.mapped(
        body.getString("shared_session_id"),
        body.getString("leader_node_id"),
        leaderTimestamp,
        body.getString("target_peer_node_id"),
        canonicalDecimalString(body, "mapped_peer_trigger_elapsed_realtime_ns"),
        canonicalDecimalString(body, "mapping_uncertainty_ns"),
        canonicalDecimalString(body, "mapping_age_at_send_ns"),
        canonicalDecimalString(body, "minimum_round_trip_ns"),
        canonicalDecimalString(body, "maximum_round_trip_ns"),
        body.getInt("sample_count"));
  }

  private static long canonicalDecimalString(JSONObject body, String field)
      throws org.json.JSONException {
    String text = body.getString(field);
    final long parsed;
    try {
      parsed = Long.parseLong(text);
    } catch (NumberFormatException malformed) {
      throw new IllegalArgumentException(field + " is not a signed 64-bit integer");
    }
    if (!Long.toString(parsed).equals(text)) {
      throw new IllegalArgumentException(field + " is not canonical decimal");
    }
    return parsed;
  }

  private void routeCoordination(NodeHttpRequest request, OutputStream output, boolean head)
      throws IOException {
    if (!requireAuthentication(request, output, head)) {
      return;
    }
    routeCoordinationAuthenticated(request, output, head);
  }

  private void routeCoordinationAuthenticated(
      NodeHttpRequest request, OutputStream output, boolean head) throws IOException {
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

  private boolean requireAuthentication(NodeHttpRequest request, OutputStream output, boolean head)
      throws IOException {
    if (NodeHttpProtocol.hasValidControlCredential(
        request.headers.get("authorization"), configuration.controlToken())) {
      return true;
    }
    NodeHttpResponseWriter.writeUnauthorized(output, head);
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
    status.put("operational_health", operationalHealthJson(operationalHealth()));
    status.put("device_admission", deviceAdmissionJson(configuration.captureProfile()));
    status.put("reboot_recovery", unattendedRecoveryJson());
    status.put(
        "live_status",
        new JSONObject()
            .put("schema_version", 1)
            .put("stream_id", liveStatusStreamId)
            .put("revision", Long.toString(liveStatusRevision.incrementAndGet()))
            .put(
                "generated_elapsed_realtime_ns",
                Long.toString(SystemClock.elapsedRealtimeNanos())));
    return status;
  }

  private JSONObject deviceAdmissionJson(CaptureProfile profile) throws Exception {
    DeviceCapabilityPolicy.HardwareSnapshot currentCapabilities = currentDeviceCapabilities();
    DeviceCapabilityPolicy.Assessment assessment =
        DeviceCapabilityPolicy.assess(currentCapabilities, profile);
    JSONArray issues = new JSONArray();
    for (DeviceCapabilityPolicy.Issue issue : assessment.issues()) {
      issues.put(
          new JSONObject()
              .put("code", issue.code().name().toLowerCase(Locale.ROOT))
              .put("message", issue.message()));
    }
    return new JSONObject()
        .put("schema_version", 1)
        .put("profile", profile.wireName())
        .put("ready", assessment.ready())
        .put("probe_succeeded", currentCapabilities.probeSucceeded())
        .put(
            "probe_diagnostic",
            currentCapabilities.probeSucceeded()
                ? JSONObject.NULL
                : currentCapabilities.probeFailure())
        .put("api_level", currentCapabilities.apiLevel())
        .put("issues", issues);
  }

  private DeviceCapabilityPolicy.HardwareSnapshot currentDeviceCapabilities() {
    return deviceCapabilities.withPermissions(
        context.checkSelfPermission(Manifest.permission.CAMERA)
            == PackageManager.PERMISSION_GRANTED,
        context.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            == PackageManager.PERMISSION_GRANTED);
  }

  private JSONObject unattendedRecoveryJson() throws Exception {
    UserManager user = context.getSystemService(UserManager.class);
    UnattendedRecoveryPolicy.Status recovery =
        UnattendedRecoveryPolicy.status(
            user != null && user.isUserUnlocked(),
            context.checkSelfPermission(Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED,
            context.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED,
            true);
    JSONObject bootObservation;
    try {
      BootRecoveryStateStore.Snapshot marker = BootRecoveryStateStore.snapshot(context);
      bootObservation =
          new JSONObject()
              .put("available", true)
              .put("observation_count", Long.toString(marker.observationCount()))
              .put("observed", marker.observed())
              .put(
                  "last_event", marker.observed() ? marker.lastEvent() : JSONObject.NULL)
              .put(
                  "last_observed_epoch_ms",
                  marker.observed() ? marker.lastObservedEpochMillis() : JSONObject.NULL)
              .put(
                  "last_observed_elapsed_realtime_ns",
                  marker.observed()
                      ? Long.toString(marker.lastObservedElapsedRealtimeNanos())
                      : JSONObject.NULL)
              .put("diagnostic", JSONObject.NULL);
    } catch (RuntimeException unavailable) {
      bootObservation =
          new JSONObject()
              .put("available", false)
              .put("observation_count", JSONObject.NULL)
              .put("observed", false)
              .put("last_event", JSONObject.NULL)
              .put("last_observed_epoch_ms", JSONObject.NULL)
              .put("last_observed_elapsed_realtime_ns", JSONObject.NULL)
              .put("diagnostic", unavailable.getClass().getSimpleName());
    }
    return new JSONObject()
        .put("schema_version", 1)
        .put("mode", UnattendedRecoveryPolicy.MODE)
        .put("direct_boot_aware", UnattendedRecoveryPolicy.directBootAware())
        .put("boot_receiver_registered", UnattendedRecoveryPolicy.bootReceiverRegistered())
        .put(
            "automatic_capture_before_first_unlock",
            UnattendedRecoveryPolicy.automaticCaptureBeforeFirstUnlock())
        .put(
            "operator_foreground_launch_required_after_os_reboot",
            UnattendedRecoveryPolicy.operatorForegroundLaunchRequiredAfterOsReboot())
        .put("process_restart_policy", UnattendedRecoveryPolicy.PROCESS_RESTART_POLICY)
        .put("boot_observation", bootObservation)
        .put("user_unlocked", recovery.userUnlocked())
        .put("service_running", recovery.serviceRunning())
        .put("ready_this_boot", recovery.readyThisBoot())
        .put("issues", new JSONArray(recovery.issues()));
  }

  private NodeSetupPolicy.OperationalHealth operationalHealth() {
    PowerManager power = context.getSystemService(PowerManager.class);
    int thermalStatus = power == null ? -1 : power.getCurrentThermalStatus();
    float thermalHeadroom = power == null ? Float.NaN : power.getThermalHeadroom(0);
    boolean powerSaveMode = power != null && power.isPowerSaveMode();
    return NodeSetupPolicy.operationalHealth(
        thermalStatus,
        thermalHeadroom,
        powerSaveMode,
        context.getFilesDir().getUsableSpace(),
        SessionStorage.MINIMUM_FREE_BYTES);
  }

  private static JSONObject operationalHealthJson(NodeSetupPolicy.OperationalHealth health)
      throws Exception {
    JSONObject thermal =
        new JSONObject()
            .put(
                "status",
                health.thermalStatus() < 0 ? JSONObject.NULL : health.thermalStatus())
            .put("ready", health.thermalReady())
            .put("power_save_mode", health.powerSaveMode());
    thermal.put(
        "headroom",
        health.thermalHeadroom().isPresent()
            ? health.thermalHeadroom().orElseThrow()
            : JSONObject.NULL);
    return new JSONObject()
        .put("ready_for_capture", health.readyForCapture())
        .put("thermal", thermal)
        .put(
            "storage",
            new JSONObject()
                .put("usable_bytes", health.storageUsableBytes())
                .put("minimum_free_bytes", health.storageMinimumFreeBytes())
                .put("ready", health.storageReady()))
        .put("issues", new JSONArray(health.readinessIssues()));
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
      if (summary.sharedSessionId != null) {
        json.put(
            "android_capture",
            new JSONObject()
                .put("node_id", summary.nodeId)
                .put("shared_session_id", summary.sharedSessionId)
                .put("role", summary.role)
                .put("coordination_available", summary.coordinationAvailable));
      }
      summaries.put(json);
    }
    JSONObject response = new JSONObject();
    response.put("schema_version", 1);
    response.put("sessions", summaries);
    return response;
  }

  private JSONObject fieldRecordingList() throws Exception {
    List<JSONObject> summaries = new ArrayList<>();
    File root = new File(context.getFilesDir(), "field_recordings");
    File[] children = root.listFiles();
    if (children != null) {
      for (File child : children) {
        if (!child.isDirectory()
            || !safeSegment(child.getName())
            || Files.isSymbolicLink(child.toPath())) {
          continue;
        }
        try {
          JSONObject manifest = new JSONObject(readFile(new File(child, "manifest.json")));
          if (manifest.getInt("schema_version") != 1
              || !"field_recording".equals(manifest.getString("session_kind"))
              || !child.getName().equals(manifest.getString("recording_id"))) {
            continue;
          }
          String recordingId = manifest.getString("recording_id");
          String prefix = "/api/v1/field-recordings/" + recordingId + "/";
          String mediaQuery =
              "?"
                  + MediaAccessAuthorization.query(
                      "field_recordings", recordingId, configuration.controlToken());
          JSONObject summary = new JSONObject();
          summary.put("recording_id", recordingId);
          summary.put("shared_recording_id", manifest.getString("shared_recording_id"));
          summary.put(
              "created_at_utc", Instant.parse(manifest.getString("created_at_utc")).toString());
          summary.put("role", manifest.getString("role"));
          summary.put("duration_us", manifest.getString("duration_us"));
          summary.put("video_bytes", manifest.getJSONObject("video").getString("bytes"));
          summary.put("audio_frames", manifest.getJSONObject("audio").getString("frames"));
          summary.put("video_url", prefix + "video.mp4" + mediaQuery);
          summary.put("audio_url", prefix + "audio.wav" + mediaQuery);
          summary.put("manifest_url", prefix + "manifest" + mediaQuery);
          summaries.add(summary);
        } catch (Exception ignored) {
          // Incomplete or malformed recordings are never advertised as ready.
        }
      }
    }
    summaries.sort(
        Comparator.comparing(
                (JSONObject value) -> value.optString("created_at_utc", ""))
            .reversed());
    JSONArray recordings = new JSONArray();
    for (JSONObject summary : summaries) {
      recordings.put(summary);
    }
    return new JSONObject().put("schema_version", 1).put("recordings", recordings);
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
        String nodeId = null;
        String sharedSessionId = null;
        String role = null;
        boolean coordinationAvailable = false;
        if (sessionKind.equals("capture")) {
          try {
            JSONObject androidCapture = manifest.getJSONObject("android_capture");
            String candidateNodeId = androidCapture.getString("node_id");
            String candidateSharedSessionId = androidCapture.getString("shared_session_id");
            JSONArray views = manifest.getJSONArray("views");
            String candidateRole = views.length() == 1
                ? views.getJSONObject(0).getString("role")
                : "";
            if (!candidateNodeId.isEmpty()
                && !candidateSharedSessionId.isEmpty()
                && (candidateRole.equals("down_the_line") || candidateRole.equals("face_on"))) {
              nodeId = candidateNodeId;
              sharedSessionId = candidateSharedSessionId;
              role = candidateRole;
              coordinationAvailable =
                  coordinationRecords.read(candidateSharedSessionId).isPresent();
            }
          } catch (Exception ignored) {
            // Older or uncoordinated manifests stay compatible through browser-side lazy fallback.
          }
        }
        summaries.add(
            new SessionSummary(
                manifest.getString("session_id"),
                createdAtUtc,
                sessionKind,
                nodeId,
                sharedSessionId,
                role,
                coordinationAvailable));
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
    NodeHttpResponseWriter.serveWholeFile(
        output, file, contentType, head, responseHeaders);
  }

  private void serveFile(
      NodeHttpRequest request,
      OutputStream output,
      File file,
      String contentType,
      boolean head) throws IOException {
    serveFile(request, output, file, contentType, head, Map.of());
  }

  private void serveFile(
      NodeHttpRequest request,
      OutputStream output,
      File file,
      String contentType,
      boolean head,
      Map<String, String> responseHeaders) throws IOException {
    NodeHttpResponseWriter.serveFile(
        request, output, file, contentType, head, responseHeaders);
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

  private static void writeBytes(
      OutputStream output,
      int status,
      String reason,
      String contentType,
      byte[] body,
      boolean head,
      Map<String, String> extraHeaders)
      throws IOException {
    writeHeaders(output, status, reason, contentType, body.length, extraHeaders);
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
    NodeHttpResponseWriter.writeHeaders(
        output, status, reason, contentType, contentLength, extraHeaders);
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
    return NodeHttpProtocol.safeSegment(value);
  }

  private static boolean isCoordinationPath(String path) {
    return path.startsWith(COORDINATION_PATH_PREFIX);
  }

  private static String readFile(File file) throws IOException {
    try (FileInputStream input = new FileInputStream(file)) {
      return new String(input.readAllBytes(), StandardCharsets.UTF_8);
    }
  }

  private static final class SessionSummary {
    private final String sessionId;
    private final String createdAtUtc;
    private final String sessionKind;
    private final String nodeId;
    private final String sharedSessionId;
    private final String role;
    private final boolean coordinationAvailable;

    private SessionSummary(
        String sessionId,
        String createdAtUtc,
        String sessionKind,
        String nodeId,
        String sharedSessionId,
        String role,
        boolean coordinationAvailable) {
      this.sessionId = sessionId;
      this.createdAtUtc = createdAtUtc;
      this.sessionKind = sessionKind;
      this.nodeId = nodeId;
      this.sharedSessionId = sharedSessionId;
      this.role = role;
      this.coordinationAvailable = coordinationAvailable;
    }
  }
}
