import { writeFile } from "node:fs/promises";
import path from "node:path";
import axe from "axe-core";
import { expect, test, type Browser, type Locator, type Page } from "@playwright/test";
import {
  cleanupRestored,
  type FieldRecorderCleanupResult,
  freshDecodeMediaAbortEvidence,
  type PrimaryStopEvidence,
  primaryStopEvidence,
  restoreStoppedFieldRecorder,
  reviewMediaAbortEvidence,
  scopedMediaCapability,
  stableMediaIdentity,
} from "./android_field_recording_hil_contract.js";

interface Configuration {
  downTheLineOrigin: string;
  downTheLineToken: string;
  faceOnOrigin: string;
  faceOnToken: string;
}

interface HilReport {
  schema_version: 1;
  report_type: "android_field_recording_browser_hil";
  passed: boolean;
  error: string;
  credential_bootstrap: string;
  credential_parameters_absent: boolean;
  application_origin: string;
  node_origins: string[];
  phone_hosted_assets: Record<string, number>;
  browser: {
    engine: "chromium";
    distribution: "google_chrome";
    version: string;
    user_agent: string;
    h264_decode_required: true;
  };
  api_statuses: Record<string, number[]>;
  camera_stage_elapsed_ms: number | null;
  camera_stage_limit_ms: number;
  startup_convergence_timeout_ms: number;
  page_ready_elapsed_ms: number | null;
  start_convergence_elapsed_ms: number | null;
  stop_convergence_elapsed_ms: number | null;
  post_camera_validation_elapsed_ms: number | null;
  cleanup_elapsed_ms: number | null;
  primary_stop_evidence: PrimaryStopEvidence[];
  watchdog_fired: boolean;
  completed_bundle_origins: string[];
  range_retrievals: Array<{ origin: string; kind: string; status: number; bytes: number }>;
  fresh_media_decode: FreshMediaDecodeEvidence[];
  fresh_decode_media_aborts: Array<{
    role: "down_the_line" | "face_on";
    origin: string;
    media_identity: string;
    error: "net::ERR_ABORTED";
  }>;
  review_media_aborts: Array<{ origin: string; path: string; error: "net::ERR_ABORTED" }>;
  accessibility_violations: string[];
  page_errors: string[];
  failed_requests: string[];
  screenshot: string;
  cleanup: FieldRecorderCleanupResult[];
  completed_bundles_retained: true;
}

interface FreshMediaDecodeEvidence {
  role: "down_the_line" | "face_on";
  origin: string;
  media_identity: string;
  duration_seconds: number;
  video_width: number;
  video_height: number;
  rvfc_media_time_seconds: number;
  rvfc_presented_frames: number;
  nonblack_fraction: number;
  playback_start_seconds: number;
  playback_end_seconds: number;
  playback_advanced_seconds: number;
}

const CAMERA_STAGE_LIMIT_MS = 15_000;
const STARTUP_CONVERGENCE_TIMEOUT_MS = 6_000;
const STOP_CONVERGENCE_TIMEOUT_MS = 8_000;

test("records and reviews a field bundle through the phone-hosted production app", async ({
  browser,
  page,
}) => {
  const configuration = readConfiguration();
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  const screenshotName = "android-field-recording-browser-hil.png";
  const report: HilReport = {
    schema_version: 1,
    report_type: "android_field_recording_browser_hil",
    passed: false,
    error: "test did not complete",
    credential_bootstrap: "tab_scoped_session_storage_from_adb_injected_environment",
    credential_parameters_absent: false,
    application_origin: configuration.downTheLineOrigin,
    node_origins: [configuration.downTheLineOrigin, configuration.faceOnOrigin],
    phone_hosted_assets: {},
    browser: {
      engine: "chromium",
      distribution: "google_chrome",
      version: "",
      user_agent: "",
      h264_decode_required: true,
    },
    api_statuses: {},
    camera_stage_elapsed_ms: null,
    camera_stage_limit_ms: CAMERA_STAGE_LIMIT_MS,
    startup_convergence_timeout_ms: STARTUP_CONVERGENCE_TIMEOUT_MS,
    page_ready_elapsed_ms: null,
    start_convergence_elapsed_ms: null,
    stop_convergence_elapsed_ms: null,
    post_camera_validation_elapsed_ms: null,
    cleanup_elapsed_ms: null,
    primary_stop_evidence: [],
    watchdog_fired: false,
    completed_bundle_origins: [],
    range_retrievals: [],
    fresh_media_decode: [],
    fresh_decode_media_aborts: [],
    review_media_aborts: [],
    accessibility_violations: [],
    page_errors: [],
    failed_requests: [],
    screenshot: screenshotName,
    cleanup: [],
    completed_bundles_retained: true,
  };
  const pageErrors: string[] = [];
  const failedRequests: string[] = [];
  const releasableFreshMedia = new Map<string, "down_the_line" | "face_on">();
  let caught: unknown = null;
  let watchdog: ReturnType<typeof setTimeout> | null = null;
  let watchdogCleanup: Promise<HilReport["cleanup"]> | null = null;
  let cameraStageStarted: number | null = null;
  const pageStarted = Date.now();

  page.on("pageerror", (error) => pageErrors.push(error.message));
  page.on("requestfailed", (request) => {
    const url = new URL(request.url());
    const freshDecodeAbort = freshDecodeMediaAbortEvidence(
      request.url(),
      request.failure()?.errorText,
      releasableFreshMedia,
    );
    if (freshDecodeAbort !== null) {
      report.fresh_decode_media_aborts.push(freshDecodeAbort);
      return;
    }
    const expectedAbort = reviewMediaAbortEvidence(
      request.url(),
      request.failure()?.errorText,
      configurationOrigins(configuration),
    );
    if (expectedAbort !== null) {
      report.review_media_aborts.push(expectedAbort);
      return;
    }
    failedRequests.push(
      `${url.origin}${url.pathname}: ${request.failure()?.errorText ?? "unknown failure"}`,
    );
  });
  page.on("response", (response) => {
    const url = new URL(response.url());
    if (
      url.origin === configuration.downTheLineOrigin &&
      (url.pathname === "/app.js" || url.pathname === "/app.css")
    ) {
      report.phone_hosted_assets[url.pathname] = response.status();
    }
    if (url.pathname.startsWith("/api/v1/")) {
      const key = `${response.request().method()} ${url.origin}${url.pathname}`;
      const statuses = report.api_statuses[key] ?? [];
      statuses.push(response.status());
      report.api_statuses[key] = statuses;
    }
  });

  try {
    await injectCredentials(page, configuration);
    const applicationUrl = new URL("/", configuration.downTheLineOrigin);
    applicationUrl.searchParams.set("dtl_node", configuration.downTheLineOrigin);
    applicationUrl.searchParams.set("face_node", configuration.faceOnOrigin);
    applicationUrl.hash = "review";
    const documentResponse = await page.goto(applicationUrl.toString());
    expect(documentResponse?.status()).toBe(200);
    expect(documentResponse?.headers()["content-type"] ?? "").toContain("text/html");
    expect(new URL(documentResponse?.url() ?? "").origin).toBe(configuration.downTheLineOrigin);
    report.browser = await selectedBrowserEvidence(browser, page);
    const sanitizedUrl = new URL(page.url());
    report.credential_parameters_absent =
      !sanitizedUrl.searchParams.has("dtl_token") &&
      !sanitizedUrl.searchParams.has("face_token") &&
      !sanitizedUrl.searchParams.has("node_token");
    expect(report.credential_parameters_absent).toBe(true);

    await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
    const panel = page.getByRole("region", { name: "Continuous test recording" });
    await expect(panel).toBeVisible();
    await expect(panel).toContainText("Down the line");
    await expect(panel).toContainText("Face on");
    await expect(panel.getByRole("button", { name: "Start field recording" })).toBeEnabled();
    // Live control readiness intentionally does not wait for immutable recording history. The
    // baseline below does, so explicitly synchronize at that narrower evidence boundary.
    await expect(panel).toHaveAttribute("data-recording-catalog-state", "ready", {
      timeout: STARTUP_CONVERGENCE_TIMEOUT_MS,
    });
    expect(report.phone_hosted_assets).toEqual({ "/app.css": 200, "/app.js": 200 });
    report.accessibility_violations = await accessibilityViolations(page);
    expect(report.accessibility_violations).toEqual([]);

    // Role locators intentionally exclude links inside the closed recording-library <details>.
    // Collect the underlying anchors directly so every historical bundle is in the baseline.
    const priorVideoIdentities = new Set(
      (await linkHrefs(panel.locator('details.field-recording-library a[href*="/video.mp4"]'))).map(
        stableMediaIdentity,
      ),
    );
    report.page_ready_elapsed_ms = Date.now() - pageStarted;
    cameraStageStarted = Date.now();
    watchdog = setTimeout(() => {
      report.watchdog_fired = true;
      watchdogCleanup = stopBoth(page, configuration);
    }, 12_000);
    await panel.getByRole("button", { name: "Start field recording" }).click();
    await expect(panel.getByRole("button", { name: "Stop both phones" })).toBeVisible({
      timeout: STARTUP_CONVERGENCE_TIMEOUT_MS,
    });
    report.start_convergence_elapsed_ms = Date.now() - cameraStageStarted;
    await expect(panel.getByText("Recording", { exact: true })).toHaveCount(3);
    await page.waitForTimeout(750);
    const stopStarted = Date.now();
    await panel.getByRole("button", { name: "Stop both phones" }).click();
    await expect(panel.getByRole("button", { name: "Start field recording" })).toBeVisible({
      timeout: STOP_CONVERGENCE_TIMEOUT_MS,
    });
    report.stop_convergence_elapsed_ms = Date.now() - stopStarted;
    report.primary_stop_evidence = primaryStopEvidence(
      report.api_statuses,
      configurationOrigins(configuration),
    );
    expect(
      report.primary_stop_evidence.every((item) => item.statuses.includes(202)),
      "the primary UI stop must be accepted by both phone origins",
    ).toBe(true);
    clearTimeout(watchdog);
    watchdog = null;
    report.camera_stage_elapsed_ms = Date.now() - cameraStageStarted;
    expect(report.camera_stage_elapsed_ms).toBeLessThan(CAMERA_STAGE_LIMIT_MS);
    const postCameraValidationStarted = Date.now();

    const library = panel.locator("details.field-recording-library");
    await expect(library).toBeVisible();
    if (!(await library.evaluate((element) => (element as HTMLDetailsElement).open))) {
      await library.getByText("Completed recordings").click();
    }
    const videoLinks = panel.getByRole("link", { name: "Video" });
    await expect
      .poll(() => videoLinks.count())
      .toBeGreaterThanOrEqual(priorVideoIdentities.size + 2);
    const newVideoUrls = (await linkHrefs(videoLinks)).filter(
      (value) => !priorVideoIdentities.has(stableMediaIdentity(value)),
    );
    expect(newVideoUrls).toHaveLength(2);
    for (const url of newVideoUrls) {
      expectScopedMediaCapability(url);
    }
    const newOrigins = [...new Set(newVideoUrls.map((value) => new URL(value).origin))].sort();
    report.completed_bundle_origins = newOrigins;
    expect(newOrigins).toEqual(
      [configuration.downTheLineOrigin, configuration.faceOnOrigin].sort(),
    );
    await expect(panel.getByRole("link", { name: "Audio" })).toHaveCount(await videoLinks.count());
    await expect(panel.getByRole("link", { name: "Manifest" })).toHaveCount(
      await videoLinks.count(),
    );

    const freshDecodeInputs = newVideoUrls.map(
      (url) =>
        ({
          url,
          role:
            new URL(url).origin === configuration.downTheLineOrigin ? "down_the_line" : "face_on",
        }) as const,
    );
    for (const item of freshDecodeInputs) {
      releasableFreshMedia.set(stableMediaIdentity(item.url), item.role);
    }
    report.fresh_media_decode = await decodeFreshPhoneMedia(page, freshDecodeInputs);
    // Give Chrome's requestfailed notification from deliberate decoder release one task turn to
    // reach the host before the fail-closed unexpected-request assertion below.
    await page.waitForTimeout(50);
    expect(report.fresh_media_decode.map((item) => item.role).sort()).toEqual([
      "down_the_line",
      "face_on",
    ]);
    report.range_retrievals = await rangeRetrieveInBrowser(page, panel, priorVideoIdentities);
    expect(report.range_retrievals).toHaveLength(4);
    expect(report.range_retrievals.every((item) => item.status === 206 && item.bytes > 0)).toBe(
      true,
    );
    report.accessibility_violations = await accessibilityViolations(page);
    expect(report.accessibility_violations).toEqual([]);
    expect(pageErrors).toEqual([]);
    expect(failedRequests).toEqual([]);
    report.post_camera_validation_elapsed_ms = Date.now() - postCameraValidationStarted;
    report.passed = true;
    report.error = "";
  } catch (failure) {
    caught = failure;
    report.error = failure instanceof Error ? failure.message : String(failure);
  } finally {
    const cleanupStarted = Date.now();
    if (cameraStageStarted !== null && report.camera_stage_elapsed_ms === null) {
      // Snapshot the partial physical stage before cleanup so an accepted-but-slow startup is not
      // incorrectly reported as though no camera work occurred.
      report.camera_stage_elapsed_ms = Date.now() - cameraStageStarted;
    }
    if (watchdog !== null) clearTimeout(watchdog);
    try {
      report.cleanup =
        watchdogCleanup === null ? await stopBoth(page, configuration) : await watchdogCleanup;
    } catch (cleanupFailure) {
      report.cleanup = configurationOrigins(configuration).map((origin) => ({
        origin,
        stop_status: null,
        final_state: "cleanup_failed",
      }));
      report.passed = false;
      report.error ||=
        cleanupFailure instanceof Error ? cleanupFailure.message : String(cleanupFailure);
      caught ??= cleanupFailure;
    }
    if (!cleanupRestored(report.cleanup)) {
      report.passed = false;
      report.error ||= "authenticated field-recorder cleanup did not converge";
      caught ??= new Error(report.error);
    }
    report.cleanup_elapsed_ms = Date.now() - cleanupStarted;
    report.page_errors = pageErrors;
    report.failed_requests = failedRequests;
    try {
      const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
      const validScreenshot =
        screenshot.subarray(0, 8).toString("hex") === "89504e470d0a1a0a" &&
        screenshot.readUInt32BE(16) === 1440 &&
        screenshot.readUInt32BE(20) === 1000;
      if (!validScreenshot) {
        const screenshotFailure = new Error("fixed-viewport screenshot contract failed");
        report.passed = false;
        report.error ||= screenshotFailure.message;
        caught ??= screenshotFailure;
      } else if (outputDirectory !== undefined) {
        await writeFile(path.join(outputDirectory, screenshotName), screenshot);
      }
    } catch (screenshotFailure) {
      report.passed = false;
      report.error ||=
        screenshotFailure instanceof Error ? screenshotFailure.message : String(screenshotFailure);
      caught ??= screenshotFailure;
    }
    if (outputDirectory !== undefined) {
      await writeFile(
        path.join(outputDirectory, "report.json"),
        `${JSON.stringify(report, null, 2)}\n`,
      );
    }
  }
  if (caught !== null) {
    throw caught;
  }
});

async function selectedBrowserEvidence(
  browser: Browser,
  page: Page,
): Promise<HilReport["browser"]> {
  if (process.env.SWING_CAPTURE_REQUIRE_H264_DECODE !== "1") {
    throw new Error("fresh phone media decode must be mandatory in the browser release HIL");
  }
  expect(browser.browserType().name()).toBe("chromium");
  const version = browser.version();
  const userAgent = await page.evaluate(() => navigator.userAgent);
  expect(version).toMatch(/^\d+(?:\.\d+)+$/);
  expect(userAgent).toMatch(/(?:Headless)?Chrome\//);
  return {
    engine: "chromium",
    distribution: "google_chrome",
    version,
    user_agent: userAgent,
    h264_decode_required: true,
  };
}

async function decodeFreshPhoneMedia(
  page: Page,
  items: Array<{ url: string; role: FreshMediaDecodeEvidence["role"] }>,
): Promise<FreshMediaDecodeEvidence[]> {
  expect(items).toHaveLength(2);
  return page.evaluate(async (inputs) => {
    const waitForEvent = (video: HTMLVideoElement, event: "loadedmetadata" | "timeupdate") =>
      new Promise<void>((resolve, reject) => {
        const timeout = window.setTimeout(() => {
          cleanup();
          reject(new Error(`fresh phone video timed out waiting for ${event}`));
        }, 4_000);
        const cleanup = () => {
          window.clearTimeout(timeout);
          video.removeEventListener(event, success);
          video.removeEventListener("error", failure);
        };
        const success = () => {
          cleanup();
          resolve();
        };
        const failure = () => {
          cleanup();
          reject(
            new Error(`fresh phone video failed to decode: ${video.error?.message ?? "error"}`),
          );
        };
        video.addEventListener(event, success, { once: true });
        video.addEventListener("error", failure, { once: true });
      });

    return Promise.all(
      inputs.map(async ({ url, role }) => {
        const video = document.createElement("video");
        video.crossOrigin = "anonymous";
        video.muted = true;
        video.playsInline = true;
        video.preload = "auto";
        // Keep the test-only decoder offscreen, then explicitly release it after proving playback.
        // Chrome may report cancellation of a follow-up Range request; the host records that only
        // for this exact allowlisted fresh-media identity.
        video.style.position = "fixed";
        video.style.left = "-10000px";
        video.style.width = "1px";
        video.style.height = "1px";
        video.src = url;
        (document.querySelector("main") ?? document.body).append(video);
        try {
          await waitForEvent(video, "loadedmetadata");
          if (
            !Number.isFinite(video.duration) ||
            video.duration <= 0 ||
            video.videoWidth <= 0 ||
            video.videoHeight <= 0
          ) {
            throw new Error("fresh phone video has invalid decoded metadata");
          }
          if (typeof video.requestVideoFrameCallback !== "function") {
            throw new Error("fresh phone video decode lacks requestVideoFrameCallback");
          }
          const playbackStart = video.currentTime;
          const frame = new Promise<{ mediaTime: number; presentedFrames: number }>(
            (resolve, reject) => {
              const timeout = window.setTimeout(
                () => reject(new Error("fresh phone video did not present a decoded frame")),
                4_000,
              );
              video.requestVideoFrameCallback((_now, metadata) => {
                window.clearTimeout(timeout);
                resolve({
                  mediaTime: metadata.mediaTime,
                  presentedFrames: metadata.presentedFrames,
                });
              });
            },
          );
          await video.play();
          const frameEvidence = await frame;
          const playbackDeadline = performance.now() + 4_000;
          while (video.currentTime - playbackStart < 0.1) {
            if (performance.now() >= playbackDeadline) {
              throw new Error("fresh phone video playback did not advance");
            }
            await waitForEvent(video, "timeupdate");
          }
          video.pause();
          const playbackEnd = video.currentTime;
          const canvas = document.createElement("canvas");
          canvas.width = 160;
          canvas.height = 90;
          const context = canvas.getContext("2d", { willReadFrequently: true });
          if (context === null) throw new Error("fresh phone video canvas is unavailable");
          context.drawImage(video, 0, 0, canvas.width, canvas.height);
          const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
          let nonblack = 0;
          for (let index = 0; index < pixels.length; index += 4) {
            if ((pixels[index] ?? 0) + (pixels[index + 1] ?? 0) + (pixels[index + 2] ?? 0) > 24) {
              nonblack += 1;
            }
          }
          const mediaUrl = new URL(url);
          return {
            role,
            origin: mediaUrl.origin,
            media_identity: `${mediaUrl.origin}${mediaUrl.pathname}`,
            duration_seconds: video.duration,
            video_width: video.videoWidth,
            video_height: video.videoHeight,
            rvfc_media_time_seconds: frameEvidence.mediaTime,
            rvfc_presented_frames: frameEvidence.presentedFrames,
            nonblack_fraction: nonblack / (canvas.width * canvas.height),
            playback_start_seconds: playbackStart,
            playback_end_seconds: playbackEnd,
            playback_advanced_seconds: playbackEnd - playbackStart,
          };
        } finally {
          video.pause();
          video.removeAttribute("src");
          video.load();
          video.remove();
        }
      }),
    );
  }, items);
}

async function injectCredentials(page: Page, configuration: Configuration): Promise<void> {
  const bindings = [
    {
      parameter: "dtl_token",
      scope: configuration.downTheLineOrigin,
      token: configuration.downTheLineToken,
    },
    {
      parameter: "face_token",
      scope: configuration.faceOnOrigin,
      token: configuration.faceOnToken,
    },
  ];
  await page.addInitScript((values) => {
    for (const value of values) {
      sessionStorage.setItem(
        `swing_capture.control_credential.v1.${encodeURIComponent(value.parameter)}.${encodeURIComponent(value.scope)}`,
        value.token,
      );
    }
  }, bindings);
}

async function accessibilityViolations(page: Page): Promise<string[]> {
  await page.addScriptTag({ content: axe.source });
  return page.evaluate(async () => {
    const result = await (window as typeof window & { axe: typeof axe }).axe.run(document, {
      rules: { "color-contrast": { enabled: false } },
    });
    return result.violations.map((violation) => violation.id).sort();
  });
}

async function rangeRetrieveInBrowser(
  page: Page,
  panel: Locator,
  priorVideoIdentities: ReadonlySet<string>,
): Promise<Array<{ origin: string; kind: string; status: number; bytes: number }>> {
  const videoUrls = (await linkHrefs(panel.getByRole("link", { name: "Video" }))).filter(
    (value) => !priorVideoIdentities.has(stableMediaIdentity(value)),
  );
  const recordingIds = new Set(videoUrls.map((value) => new URL(value).pathname.split("/")[4]));
  const audioUrls = (await linkHrefs(panel.getByRole("link", { name: "Audio" }))).filter((value) =>
    recordingIds.has(new URL(value).pathname.split("/")[4]),
  );
  for (const url of [...videoUrls, ...audioUrls]) {
    expectScopedMediaCapability(url);
  }
  return page.evaluate(
    async (items) => {
      return Promise.all(
        items.map(async (item) => {
          const response = await fetch(item.url, { headers: { Range: "bytes=0-127" } });
          return {
            origin: new URL(item.url).origin,
            kind: item.kind,
            status: response.status,
            bytes: (await response.arrayBuffer()).byteLength,
          };
        }),
      );
    },
    [
      ...videoUrls.map((url) => ({ url, kind: "video" })),
      ...audioUrls.map((url) => ({ url, kind: "audio" })),
    ],
  );
}

function expectScopedMediaCapability(value: string): void {
  expect(scopedMediaCapability(value)).toMatch(/^[A-Za-z0-9_-]{43}$/);
}

function linkHrefs(links: Locator): Promise<string[]> {
  return links.evaluateAll((elements) =>
    elements.map((element) => (element as HTMLAnchorElement).href),
  );
}

async function stopBoth(page: Page, configuration: Configuration): Promise<HilReport["cleanup"]> {
  return Promise.all(
    (
      [
        [configuration.downTheLineOrigin, configuration.downTheLineToken],
        [configuration.faceOnOrigin, configuration.faceOnToken],
      ] as const
    ).map(([origin, token]) =>
      restoreStoppedFieldRecorder(origin, token, {
        stop: async (targetOrigin, targetToken) =>
          (
            await page.request.post(`${targetOrigin}/api/v1/field-recording/stop`, {
              headers: { Authorization: `Bearer ${targetToken}` },
              data: {},
              timeout: 2_000,
            })
          ).status(),
        status: async (targetOrigin, targetToken) => {
          const status = await page.request.get(`${targetOrigin}/api/v1/field-recording/status`, {
            headers: { Authorization: `Bearer ${targetToken}` },
            timeout: 1_000,
          });
          return ((await status.json()) as { state?: string }).state ?? "unknown";
        },
        wait: (milliseconds) => page.waitForTimeout(milliseconds),
      }),
    ),
  );
}

function configurationOrigins(configuration: Configuration): string[] {
  return [configuration.downTheLineOrigin, configuration.faceOnOrigin];
}

function readConfiguration(): Configuration {
  const required = (name: string): string => {
    const value = process.env[name];
    if (value === undefined || value.length === 0) throw new Error(`${name} is required`);
    return value;
  };
  const downTheLineOrigin = new URL(required("SWING_CAPTURE_ANDROID_DTL_NODE_URL")).origin;
  const faceOnOrigin = new URL(required("SWING_CAPTURE_ANDROID_FACE_NODE_URL")).origin;
  const downTheLineToken = required("SWING_CAPTURE_ANDROID_DTL_TOKEN");
  const faceOnToken = required("SWING_CAPTURE_ANDROID_FACE_TOKEN");
  if (!/^[A-Za-z0-9_-]{32}$/.test(downTheLineToken) || !/^[A-Za-z0-9_-]{32}$/.test(faceOnToken)) {
    throw new Error("Android control credentials must be 32-character URL-safe tokens");
  }
  return { downTheLineOrigin, downTheLineToken, faceOnOrigin, faceOnToken };
}
