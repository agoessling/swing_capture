import { readFile, writeFile } from "node:fs/promises";
import { createServer, type Server } from "node:http";
import path from "node:path";
import { expect, test } from "@playwright/test";

interface HilManifestSummary {
  sessionId: string;
  createdAtUtc: string;
  mediaPaths: string[];
  frameCount: number;
  impactFrameIndex: number;
}

const DUAL_ANDROID_HIL_ENVIRONMENT = [
  "SWING_CAPTURE_ANDROID_DTL_NODE_URL",
  "SWING_CAPTURE_ANDROID_DTL_TOKEN",
  "SWING_CAPTURE_ANDROID_FACE_NODE_URL",
  "SWING_CAPTURE_ANDROID_FACE_TOKEN",
] as const;

interface DualAndroidHilConfiguration {
  downTheLineNodeUrl: string;
  downTheLineToken: string;
  faceOnNodeUrl: string;
  faceOnToken: string;
}

let server: Server;
let stationUrl: string;

test.beforeAll(async () => {
  const fixtureRoot = process.env.SWING_CAPTURE_FIXTURE_APP;
  if (fixtureRoot === undefined) {
    throw new Error("SWING_CAPTURE_FIXTURE_APP is not set");
  }
  const absoluteRoot = path.resolve(fixtureRoot);
  server = createServer((request, response) => {
    void serveFixture(absoluteRoot, request, response);
  });
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (address === null || typeof address === "string") {
    throw new Error("Fixture server did not bind a TCP address");
  }
  stationUrl = `http://127.0.0.1:${address.port}/demo.html#review`;
});

test.afterAll(async () => {
  await new Promise<void>((resolve, reject) => {
    server.close((error) => (error === undefined ? resolve() : reject(error)));
  });
});

test("production document uses root-relative bundle assets", async () => {
  const staticRoot = process.env.SWING_CAPTURE_STATIC_APP;
  if (staticRoot === undefined) {
    throw new Error("SWING_CAPTURE_STATIC_APP is not set");
  }
  const document = await readFile(path.join(path.resolve(staticRoot), "index.html"), "utf8");
  expect(document).toContain('href="/app.css"');
  expect(document).toContain('src="/app.js"');
});

test("plays and steps a synchronized fixture clip", async ({ page }) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));
  await page.goto(stationUrl);
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();

  const videos = page.locator("video");
  await expect(videos).toHaveCount(2);
  await expect
    .poll(async () =>
      videos.evaluateAll((elements) =>
        elements.map((video) => (video as HTMLVideoElement).readyState),
      ),
    )
    .toEqual([4, 4]);
  const pipelinePanel = page.getByRole("region", { name: "Pipeline profile" });
  await expect(pipelinePanel).toContainText("Prepublication analysis");
  await expect(pipelinePanel).toContainText("Publisher planning");
  await expect(pipelinePanel).toContainText("Manifest response → both impact frames displayed");
  await expect(pipelinePanel).toContainText(
    "Audio confirmation → both frames displayed, lower bound",
  );
  await expect(pipelinePanel).toContainText("no clock epochs are assumed to match");
  await expect.poll(async () => pipelinePanel.getAttribute("data-browser-timing")).not.toBeNull();
  const serializedBrowserTiming = await pipelinePanel.getAttribute("data-browser-timing");
  expect(serializedBrowserTiming).not.toBeNull();
  const browserTiming = JSON.parse(serializedBrowserTiming ?? "null") as {
    schema_version: number;
    presentation_method: string;
    manifest_fetch_duration_ms: number;
    manifest_response_to_both_frames_ms: number;
    audio_confirmation_to_both_frames_lower_bound_ms: number;
    audio_confirmation_to_both_frames_upper_bound_ms: number;
  };
  expect(browserTiming.schema_version).toBe(1);
  expect(browserTiming.presentation_method).toBe("requestVideoFrameCallback");
  expect(browserTiming.manifest_fetch_duration_ms).toBeGreaterThanOrEqual(0);
  expect(browserTiming.manifest_response_to_both_frames_ms).toBeGreaterThanOrEqual(0);
  expect(
    browserTiming.audio_confirmation_to_both_frames_upper_bound_ms -
      browserTiming.audio_confirmation_to_both_frames_lower_bound_ms,
  ).toBeCloseTo(browserTiming.manifest_fetch_duration_ms, 6);
  const timingOutputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  if (timingOutputDirectory !== undefined) {
    await writeFile(
      path.join(timingOutputDirectory, "browser-pipeline-profile.json"),
      `${JSON.stringify(browserTiming, null, 2)}\n`,
    );
  }
  expect(pageErrors).toEqual([]);
  expect(
    await videos.evaluateAll((elements) =>
      elements.map((video) => (video as HTMLVideoElement).dataset.reviewSession ?? "missing"),
    ),
  ).toEqual(["fixture-session-001", "fixture-session-001"]);
  expect(
    await videos.evaluateAll((elements) =>
      elements.map((element) => {
        const video = element as HTMLVideoElement;
        return {
          duration: video.duration,
          seekable: video.seekable.length === 0 ? 0 : video.seekable.end(0),
        };
      }),
    ),
  ).toEqual([
    { duration: 3, seekable: 3 },
    { duration: 3, seekable: 3 },
  ]);
  expect(
    await videos.evaluateAll((elements) =>
      elements.map((video) => Number((video as HTMLVideoElement).currentTime.toFixed(6))),
    ),
  ).toEqual([1.499985, 1.499985]);

  await page.getByRole("button", { name: "Next frame" }).click();
  await expect(page.getByText("Frame 47 of 90")).toBeVisible();
  const steppedTimes = await videos.evaluateAll((elements) =>
    elements.map((video) => Number((video as HTMLVideoElement).currentTime.toFixed(6))),
  );
  expect(steppedTimes).toEqual([1.533318, 1.533318]);

  await page.getByRole("combobox", { name: "Playback speed" }).selectOption("0.5");
  expect(
    await videos.evaluateAll((elements) =>
      elements.map((video) => (video as HTMLVideoElement).playbackRate),
    ),
  ).toEqual([0.5, 0.5]);
  await page.getByRole("combobox", { name: "Playback speed" }).selectOption("1");
  expect(
    await videos.evaluateAll((elements) =>
      elements.map((video) => (video as HTMLVideoElement).playbackRate),
    ),
  ).toEqual([1, 1]);

  await page.getByRole("button", { name: "Play" }).click();
  await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
  await expect
    .poll(async () =>
      videos.evaluateAll((elements) =>
        elements.every((video) => (video as HTMLVideoElement).currentTime > 1.54),
      ),
    )
    .toBe(true);
  await videos.nth(1).evaluate((element) => {
    const video = element as HTMLVideoElement;
    video.currentTime = Math.max(0, video.currentTime - 0.1);
  });
  await expect
    .poll(async () => {
      const times = await videos.evaluateAll((elements) =>
        elements.map((video) => (video as HTMLVideoElement).currentTime),
      );
      return Math.abs((times[0] ?? 0) - (times[1] ?? 0));
    })
    .toBeLessThan(0.04);
  const playbackTimes = await videos.evaluateAll((elements) =>
    elements.map((video) => (video as HTMLVideoElement).currentTime),
  );
  expect(Math.abs((playbackTimes[0] ?? 0) - (playbackTimes[1] ?? 0))).toBeLessThan(0.05);
  await page.getByRole("button", { name: "Pause" }).click();
  await page.getByRole("slider", { name: "Review timeline" }).fill("45");
  await expect(page.getByText("Frame 46 of 90")).toBeVisible();
  await page.evaluate(() => window.scrollTo(0, 0));

  const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
  expect(screenshot.subarray(0, 8).toString("hex")).toBe("89504e470d0a1a0a");
  expect({ width: screenshot.readUInt32BE(16), height: screenshot.readUInt32BE(20) }).toEqual({
    width: 1440,
    height: 1000,
  });
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  if (outputDirectory !== undefined) {
    await writeFile(path.join(outputDirectory, "review-fixed-viewport.png"), screenshot);
  }
  const viewport = page.viewportSize();
  expect(viewport).toEqual({ width: 1440, height: 1000 });
});

test("keeps post-capture diagnostics usable on a phone viewport", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(stationUrl);

  const diagnostics = page.getByRole("region", { name: "Capture diagnostics" });
  await diagnostics.scrollIntoViewIfNeeded();
  await expect(diagnostics).toBeVisible();
  await expect(diagnostics.getByRole("option")).toHaveCount(7);
  await diagnostics.getByRole("combobox", { name: "Result" }).selectOption("missed_shot");
  await diagnostics
    .getByRole("textbox", { name: "Note (optional)" })
    .fill("Impact sound was not detected");
  await diagnostics.getByText("Timing marks (optional)").click();
  await diagnostics
    .getByRole("spinbutton", { name: "Desired high-speed start (ms)" })
    .fill("-1200");
  await diagnostics.getByRole("button", { name: "Save diagnostic feedback" }).click();
  await expect(diagnostics.getByText("Diagnostic feedback saved.")).toBeVisible();
  await expect(diagnostics.getByRole("button", { name: "Download diagnostic ZIP" })).toBeVisible();

  expect(
    await page.evaluate(() => ({
      documentWidth: document.documentElement.scrollWidth,
      viewportWidth: document.documentElement.clientWidth,
    })),
  ).toEqual({ documentWidth: 390, viewportWidth: 390 });

  const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
  expect({ width: screenshot.readUInt32BE(16), height: screenshot.readUInt32BE(20) }).toEqual({
    width: 390,
    height: 844,
  });
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  if (outputDirectory !== undefined) {
    await writeFile(path.join(outputDirectory, "review-phone-diagnostics.png"), screenshot);
  }
});

test("keeps camera setup available and exposes accessible review controls", async ({ page }) => {
  await page.goto(stationUrl);
  await page.getByRole("button", { name: "Camera setup" }).click();
  await expect(page.getByRole("heading", { name: "Camera setup" })).toBeVisible();
  await page.getByRole("button", { name: "Review" }).click();
  await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();

  await expect(page.getByRole("button", { name: "Disarm capture" })).toBeEnabled();
  await expect(page.getByRole("button", { name: "Save missed shot" })).toBeEnabled();
  await page.getByRole("button", { name: "Disarm capture" }).click();
  await expect(page.getByRole("heading", { name: "Not armed" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Save missed shot" })).toBeDisabled();
  await page.getByRole("button", { name: "Arm audio capture" }).click();
  await expect(page.getByRole("heading", { name: "Listening for an audio trigger" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Save missed shot" })).toBeEnabled();
  await page.getByRole("button", { name: "Save missed shot" }).click();
  await expect(page.getByRole("heading", { name: "Audio trigger detected" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeDisabled();
  await expect(page.getByRole("button", { name: "Save missed shot" })).toBeDisabled();
  await expect(page.getByRole("heading", { name: "Ready" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeEnabled();
  await expect(page.getByRole("button", { name: "Save missed shot" })).toBeDisabled();
  await expect(page.getByRole("button", { name: "Run synthetic swing HIL" })).toBeEnabled();
  await page.evaluate(() => {
    const heading = document.querySelector("#synthetic-hil-heading");
    if (!(heading instanceof HTMLHeadingElement)) {
      throw new Error("Synthetic HIL heading is unavailable");
    }
    const trace = [heading.textContent?.trim() ?? ""];
    const observer = new MutationObserver(() => {
      const current = heading.textContent?.trim() ?? "";
      if (trace.at(-1) !== current) {
        trace.push(current);
      }
    });
    observer.observe(heading, { characterData: true, childList: true, subtree: true });
    Object.assign(window, { __syntheticHilHeadingTrace: trace });
  });
  await page.getByRole("button", { name: "Run synthetic swing HIL" }).click();
  await expect(page.getByRole("heading", { name: "Calibrating the optical signal" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Synthetic swing running…" })).toBeDisabled();
  await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeDisabled();
  await expect(page.getByRole("heading", { name: "Synthetic swing ready" })).toBeVisible();
  const lifecycle = await page.evaluate(
    () =>
      (
        window as typeof window & {
          __syntheticHilHeadingTrace: string[];
        }
      ).__syntheticHilHeadingTrace,
  );
  expect(lifecycle).toEqual([
    "Synthetic swing station check",
    "Calibrating the optical signal",
    "Running the LED and audio sequence",
    "Capturing the synthetic swing",
    "Encoding the synchronized review",
    "Synthetic swing ready",
  ]);
  await expect(page.getByLabel("Down-the-line synthetic HIL evidence")).toContainText(
    "Automated white-impact check · passed · frame 45",
  );
  await expect(page.getByLabel("Face-on synthetic HIL evidence")).toContainText(
    "Audio trigger estimate relative to white · −1.8 ms (uncalibrated)",
  );
  await expect(page.getByText(/60 pre-impact and 25 post-impact LED colors/)).toBeVisible();
  await expect(page.getByRole("combobox", { name: "Recorded session" })).toHaveValue(
    "fixture-synthetic-001",
  );
  await expect(page.getByRole("slider", { name: "Review timeline" })).toHaveAttribute(
    "aria-valuetext",
    "Frame 46, Trigger estimate",
  );
  await page.getByLabel("Down-the-line synthetic HIL evidence").scrollIntoViewIfNeeded();
  const hilScreenshot = await page.screenshot({ animations: "disabled", fullPage: false });
  expect({
    width: hilScreenshot.readUInt32BE(16),
    height: hilScreenshot.readUInt32BE(20),
  }).toEqual({ width: 1440, height: 1000 });
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  if (outputDirectory !== undefined) {
    await writeFile(path.join(outputDirectory, "synthetic-hil-ready.png"), hilScreenshot);
  }
});

test("loads the supplied physical HIL session artifact", async ({ page }) => {
  if (process.env.SWING_CAPTURE_REQUIRE_HIL_ARTIFACT !== "1") {
    test.skip();
    return;
  }
  const artifactDirectory = process.env.SWING_CAPTURE_HIL_SESSION_DIR;
  if (artifactDirectory === undefined) {
    throw new Error(
      "SWING_CAPTURE_HIL_SESSION_DIR must name the absolute session artifact directory",
    );
  }
  if (!path.isAbsolute(artifactDirectory)) {
    throw new Error("SWING_CAPTURE_HIL_SESSION_DIR must be absolute");
  }
  const staticRoot = process.env.SWING_CAPTURE_STATIC_APP;
  if (staticRoot === undefined) {
    throw new Error("SWING_CAPTURE_STATIC_APP is not set");
  }

  const manifestJson = await readFile(path.join(artifactDirectory, "manifest.json"), "utf8");
  const manifest = summarizeHilManifest(JSON.parse(manifestJson) as unknown);
  const requestedMedia = new Set<string>();
  const hilServer = createServer((request, response) => {
    void serveHilApplication(
      path.resolve(staticRoot),
      artifactDirectory,
      manifestJson,
      manifest,
      requestedMedia,
      request,
      response,
    );
  });
  const hilUrl = await listen(hilServer);
  try {
    const pageErrors: string[] = [];
    page.on("pageerror", (error) => pageErrors.push(error.message));
    await page.goto(`${hilUrl}/#review`);
    await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
    await expect(page.getByRole("heading", { name: "Ready" })).toBeVisible();
    await expect(page.getByRole("button", { name: "Arm audio capture" })).toBeEnabled();
    await expect(page.getByRole("button", { name: "Save missed shot" })).toBeDisabled();
    await expect(
      page.getByText(`Frame ${manifest.impactFrameIndex + 1} of ${manifest.frameCount}`),
    ).toBeVisible();

    const videos = page.locator("video");
    await expect(videos).toHaveCount(2);
    await expect
      .poll(async () =>
        videos.evaluateAll((elements) =>
          elements.map((video) => (video as HTMLVideoElement).readyState),
        ),
      )
      .toEqual([4, 4]);
    await expect
      .poll(async () =>
        videos.evaluateAll((elements) =>
          elements.every((element) => {
            const video = element as HTMLVideoElement;
            return video.duration > 0 && video.seekable.length > 0;
          }),
        ),
      )
      .toBe(true);
    const physicalPipelinePanel = page.getByRole("region", { name: "Pipeline profile" });
    await expect
      .poll(async () => physicalPipelinePanel.getAttribute("data-browser-timing"))
      .not.toBeNull();
    const physicalBrowserTiming = JSON.parse(
      (await physicalPipelinePanel.getAttribute("data-browser-timing")) ?? "null",
    ) as unknown;
    const physicalTimingOutputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
    if (physicalTimingOutputDirectory !== undefined) {
      await writeFile(
        path.join(physicalTimingOutputDirectory, "hil-browser-pipeline-profile.json"),
        `${JSON.stringify(physicalBrowserTiming, null, 2)}\n`,
      );
    }
    expect(pageErrors).toEqual([]);
    expect([...requestedMedia].sort()).toEqual([...manifest.mediaPaths].sort());

    const canStepForward = manifest.impactFrameIndex + 1 < manifest.frameCount;
    const stepButton = page.getByRole("button", {
      name: canStepForward ? "Next frame" : "Previous frame",
    });
    const steppedIndex = manifest.impactFrameIndex + (canStepForward ? 1 : -1);
    await stepButton.click();
    await expect(
      page.getByText(`Frame ${steppedIndex + 1} of ${manifest.frameCount}`),
    ).toBeVisible();

    await page.getByRole("button", { name: "Play" }).click();
    await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
    const initialTime = await videos
      .nth(0)
      .evaluate((element) => Number((element as HTMLVideoElement).currentTime));
    await expect
      .poll(async () =>
        videos.nth(0).evaluate((element) => Number((element as HTMLVideoElement).currentTime)),
      )
      .toBeGreaterThan(initialTime);
    await page.getByRole("button", { name: "Pause" }).click();
    await page
      .getByRole("slider", { name: "Review timeline" })
      .fill(String(manifest.impactFrameIndex));
    await expect(
      page.getByText(`Frame ${manifest.impactFrameIndex + 1} of ${manifest.frameCount}`),
    ).toBeVisible();
    await expect
      .poll(async () =>
        videos.evaluateAll((elements) =>
          elements.every((element) => !(element as HTMLVideoElement).seeking),
        ),
      )
      .toBe(true);
    await page.evaluate(() => window.scrollTo(0, 0));

    const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
    expect(screenshot.subarray(0, 8).toString("hex")).toBe("89504e470d0a1a0a");
    const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
    if (outputDirectory !== undefined) {
      await writeFile(path.join(outputDirectory, "hil-review-session.png"), screenshot);
    }
  } finally {
    await close(hilServer);
  }
});

test("loads the live Android capture node", async ({ page }) => {
  const nodeUrl = process.env.SWING_CAPTURE_ANDROID_NODE_URL;
  if (nodeUrl === undefined) {
    test.skip();
    return;
  }
  const parsedNodeUrl = new URL(nodeUrl);
  if (parsedNodeUrl.protocol !== "http:" && parsedNodeUrl.protocol !== "https:") {
    throw new Error("SWING_CAPTURE_ANDROID_NODE_URL must be an HTTP(S) URL");
  }
  const staticRoot = process.env.SWING_CAPTURE_STATIC_APP;
  if (staticRoot === undefined) {
    throw new Error("SWING_CAPTURE_STATIC_APP is not set");
  }
  const staticServer = createServer((request, response) => {
    void serveFixture(path.resolve(staticRoot), request, response);
  });
  const staticUrl = await listen(staticServer);
  try {
    const pageErrors: string[] = [];
    const failedRequests: string[] = [];
    const mediaResponseStatuses: number[] = [];
    page.on("pageerror", (error) => pageErrors.push(error.message));
    page.on("requestfailed", (request) =>
      failedRequests.push(`${request.url()}: ${request.failure()?.errorText ?? "unknown failure"}`),
    );
    page.on("response", (response) => {
      if (response.url().endsWith(".mp4")) {
        mediaResponseStatuses.push(response.status());
      }
    });
    await page.goto(
      `${staticUrl}/index.html?node=${encodeURIComponent(parsedNodeUrl.toString())}#review`,
    );
    await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
    await expect(page.getByRole("heading", { name: "Ready" })).toBeVisible();
    const h264Support = await page.evaluate(() =>
      document.createElement("video").canPlayType('video/mp4; codecs="avc1.640034"'),
    );
    expect(h264Support, "the HIL browser must advertise H.264 High Level 5.2").not.toBe("");
    const videos = page.locator("video");
    await expect(videos).toHaveCount(1);
    await expect
      .poll(async () =>
        videos.nth(0).evaluate((element) => {
          const video = element as HTMLVideoElement;
          return {
            readyState: video.readyState,
            duration: video.duration,
            seekable: video.seekable.length === 0 ? 0 : video.seekable.end(0),
          };
        }),
      )
      .toMatchObject({ readyState: 4 });
    const mediaState = await videos.nth(0).evaluate((element) => {
      const video = element as HTMLVideoElement;
      return { duration: video.duration, seekable: video.seekable.end(0) };
    });
    expect(mediaState.duration).toBeGreaterThan(2);
    expect(mediaState.seekable).toBeGreaterThan(2);

    const frameText = await page
      .getByText(/^Frame \d+ of \d+$/)
      .first()
      .textContent();
    expect(frameText).toMatch(/^Frame \d+ of \d+$/);
    await page.getByRole("button", { name: "Next frame" }).click();
    await page.getByRole("button", { name: "Play" }).click();
    await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
    const initialTime = await videos
      .nth(0)
      .evaluate((element) => Number((element as HTMLVideoElement).currentTime));
    await expect
      .poll(async () =>
        videos.nth(0).evaluate((element) => Number((element as HTMLVideoElement).currentTime)),
      )
      .toBeGreaterThan(initialTime);
    await page.getByRole("button", { name: "Pause" }).click();
    expect(pageErrors).toEqual([]);
    expect(mediaResponseStatuses).toContain(206);
    expect(failedRequests.filter((failure) => !failure.endsWith(".mp4: net::ERR_ABORTED"))).toEqual(
      [],
    );

    const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
    expect({ width: screenshot.readUInt32BE(16), height: screenshot.readUInt32BE(20) }).toEqual({
      width: 1440,
      height: 1000,
    });
    const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
    if (outputDirectory !== undefined) {
      await writeFile(path.join(outputDirectory, "android-node-review.png"), screenshot);
    }
  } finally {
    await close(staticServer);
  }
});

test("validates the dual Android HIL environment contract", () => {
  expect(readDualAndroidHilConfiguration({})).toBeNull();
  expect(
    readDualAndroidHilConfiguration({
      SWING_CAPTURE_ANDROID_DTL_NODE_URL: "http://dtl.invalid:4315/",
      SWING_CAPTURE_ANDROID_DTL_TOKEN: "A2345678901234567890123456789012",
      SWING_CAPTURE_ANDROID_FACE_NODE_URL: "https://face.invalid:4316",
      SWING_CAPTURE_ANDROID_FACE_TOKEN: "B2345678901234567890123456789012",
    }),
  ).toEqual({
    downTheLineNodeUrl: "http://dtl.invalid:4315",
    downTheLineToken: "A2345678901234567890123456789012",
    faceOnNodeUrl: "https://face.invalid:4316",
    faceOnToken: "B2345678901234567890123456789012",
  });

  expect(() =>
    readDualAndroidHilConfiguration({
      SWING_CAPTURE_ANDROID_DTL_NODE_URL: "http://dtl.invalid:4315",
    }),
  ).toThrow(/all four variables.*SWING_CAPTURE_ANDROID_DTL_TOKEN/);
  expect(() =>
    readDualAndroidHilConfiguration({
      SWING_CAPTURE_ANDROID_DTL_NODE_URL: "file:///tmp/dtl",
      SWING_CAPTURE_ANDROID_DTL_TOKEN: "A2345678901234567890123456789012",
      SWING_CAPTURE_ANDROID_FACE_NODE_URL: "https://face.invalid:4316",
      SWING_CAPTURE_ANDROID_FACE_TOKEN: "B2345678901234567890123456789012",
    }),
  ).toThrow(/must be an HTTP\(S\) origin/);
  expect(() =>
    readDualAndroidHilConfiguration({
      SWING_CAPTURE_ANDROID_DTL_NODE_URL: "http://dtl.invalid:4315/api",
      SWING_CAPTURE_ANDROID_DTL_TOKEN: "A2345678901234567890123456789012",
      SWING_CAPTURE_ANDROID_FACE_NODE_URL: "https://face.invalid:4316",
      SWING_CAPTURE_ANDROID_FACE_TOKEN: "B2345678901234567890123456789012",
    }),
  ).toThrow(/must be an HTTP\(S\) origin/);
  expect(() =>
    readDualAndroidHilConfiguration({
      SWING_CAPTURE_ANDROID_DTL_NODE_URL: "http://node.invalid:4315",
      SWING_CAPTURE_ANDROID_DTL_TOKEN: "not-a-phone-token",
      SWING_CAPTURE_ANDROID_FACE_NODE_URL: "http://node.invalid:4316",
      SWING_CAPTURE_ANDROID_FACE_TOKEN: "B2345678901234567890123456789012",
    }),
  ).toThrow(/DTL_TOKEN must be a 32-character/);
  expect(() =>
    readDualAndroidHilConfiguration({
      SWING_CAPTURE_ANDROID_DTL_NODE_URL: "http://node.invalid:4315",
      SWING_CAPTURE_ANDROID_DTL_TOKEN: "A2345678901234567890123456789012",
      SWING_CAPTURE_ANDROID_FACE_NODE_URL: "http://node.invalid:4315/",
      SWING_CAPTURE_ANDROID_FACE_TOKEN: "B2345678901234567890123456789012",
    }),
  ).toThrow(/must identify two different origins/);
});

test("loads a durable paired session from two live Android nodes", async ({ page }) => {
  test.setTimeout(30_000);
  const configuration = readDualAndroidHilConfiguration(process.env);
  if (configuration === null) {
    test.skip();
    return;
  }
  const staticRoot = process.env.SWING_CAPTURE_STATIC_APP;
  if (staticRoot === undefined) {
    throw new Error("SWING_CAPTURE_STATIC_APP is not set");
  }

  const expectedNodeOrigins = [
    new URL(configuration.downTheLineNodeUrl).origin,
    new URL(configuration.faceOnNodeUrl).origin,
  ];
  const staticServer = createServer((request, response) => {
    void serveFixture(path.resolve(staticRoot), request, response);
  });
  const staticUrl = await listen(staticServer);
  const pageErrors: string[] = [];
  const failedRequests: string[] = [];
  try {
    const partialContentOrigins = new Set<string>();
    const durableCoordinationOrigins = new Set<string>();
    page.on("pageerror", (error) => pageErrors.push(error.message));
    page.on("requestfailed", (request) => {
      const failure = requestFailure(request);
      if (failure !== null) {
        failedRequests.push(failure);
      }
    });
    page.on("response", (response) => {
      const responseUrl = new URL(response.url());
      if (
        response.status() === 206 &&
        responseUrl.pathname.endsWith(".mp4") &&
        response.headers()["content-type"]?.startsWith("video/mp4") === true
      ) {
        partialContentOrigins.add(responseUrl.origin);
      }
      if (
        response.status() === 200 &&
        response.request().method() === "GET" &&
        /^\/api\/v1\/coordination\/[^/]+$/.test(responseUrl.pathname)
      ) {
        durableCoordinationOrigins.add(responseUrl.origin);
      }
    });

    const applicationUrl = new URL("/index.html", staticUrl);
    applicationUrl.searchParams.set("dtl_node", configuration.downTheLineNodeUrl);
    applicationUrl.searchParams.set("dtl_token", configuration.downTheLineToken);
    applicationUrl.searchParams.set("face_node", configuration.faceOnNodeUrl);
    applicationUrl.searchParams.set("face_token", configuration.faceOnToken);
    applicationUrl.hash = "review";
    await page.goto(applicationUrl.toString());

    await expect(page.getByRole("heading", { name: "Swing review" })).toBeVisible();
    await expect(page.getByRole("heading", { name: "Ready" })).toBeVisible({ timeout: 15_000 });
    const sessionSelector = page.getByRole("combobox", { name: "Recorded session" });
    await expect(sessionSelector).toBeVisible();
    const readySessionIds = await sessionSelector
      .locator("option")
      .evaluateAll((options) =>
        options
          .filter((option) => option.textContent?.trim().endsWith("· Ready") === true)
          .map((option) => (option as HTMLOptionElement).value),
      );
    expect(
      readySessionIds,
      "both phones must retain at least one paired ready session",
    ).not.toEqual([]);
    const selectedSessionId = readySessionIds[0];
    if (selectedSessionId === undefined) {
      throw new Error("Ready session selection was lost");
    }
    if ((await sessionSelector.inputValue()) !== selectedSessionId) {
      await sessionSelector.selectOption(selectedSessionId);
    }

    const h264Support = await page.evaluate(() =>
      document.createElement("video").canPlayType('video/mp4; codecs="avc1.640034"'),
    );
    expect(h264Support, "the HIL browser must advertise H.264 High Level 5.2").not.toBe("");
    const videos = page.locator("video");
    await expect(videos).toHaveCount(2);
    await expect
      .poll(async () =>
        videos.evaluateAll((elements) =>
          elements.map((element) => {
            const video = element as HTMLVideoElement;
            const source = video.querySelector("source");
            return {
              readyState: video.readyState,
              h264: source?.type.startsWith('video/mp4; codecs="avc1.') === true,
              canPlay: source === null ? "" : video.canPlayType(source.type),
              origin: video.currentSrc.length === 0 ? "" : new URL(video.currentSrc).origin,
              sessionId: video.dataset.reviewSession ?? "",
            };
          }),
        ),
      )
      .toEqual([
        expect.objectContaining({
          readyState: 4,
          h264: true,
          origin: expectedNodeOrigins[0],
          sessionId: selectedSessionId,
        }),
        expect.objectContaining({
          readyState: 4,
          h264: true,
          origin: expectedNodeOrigins[1],
          sessionId: selectedSessionId,
        }),
      ]);
    const mediaStates = await videos.evaluateAll((elements) =>
      elements.map((element) => {
        const video = element as HTMLVideoElement;
        const source = video.querySelector("source");
        return {
          duration: video.duration,
          seekable: video.seekable.length === 0 ? 0 : video.seekable.end(0),
          width: video.videoWidth,
          height: video.videoHeight,
          canPlay: source === null ? "" : video.canPlayType(source.type),
        };
      }),
    );
    for (const state of mediaStates) {
      expect(state.duration).toBeGreaterThan(0);
      expect(state.seekable).toBeGreaterThan(0);
      expect(state.width).toBeGreaterThan(0);
      expect(state.height).toBeGreaterThan(0);
      expect(state.canPlay).not.toBe("");
    }
    await expect
      .poll(() => [...durableCoordinationOrigins].sort())
      .toEqual([...expectedNodeOrigins].sort());
    await expect
      .poll(() => [...partialContentOrigins].sort())
      .toEqual([...expectedNodeOrigins].sort());

    const frameLabel = page.getByText(/^Frame \d+ of \d+$/).first();
    const initialFrame = parseFrameLabel(await frameLabel.textContent());
    const stepForward = initialFrame.current < initialFrame.total;
    const timesBeforeStep = await videoTimes(videos);
    await page.getByRole("button", { name: stepForward ? "Next frame" : "Previous frame" }).click();
    const steppedFrame = initialFrame.current + (stepForward ? 1 : -1);
    await expect(frameLabel).toHaveText(`Frame ${steppedFrame} of ${initialFrame.total}`);
    await expect
      .poll(async () =>
        videos.evaluateAll((elements) =>
          elements.every((element) => !(element as HTMLVideoElement).seeking),
        ),
      )
      .toBe(true);
    const timesAfterStep = await videoTimes(videos);
    expect(
      timesAfterStep.every(
        (time, index) => Math.abs(time - (timesBeforeStep[index] ?? time)) > 0.001,
      ),
      "one frame step must seek both encoded views",
    ).toBe(true);

    await page.getByRole("button", { name: "Play" }).click();
    await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
    await expect
      .poll(async () => {
        const current = await videoTimes(videos);
        return current.every((time, index) => time > (timesAfterStep[index] ?? time) + 0.001);
      })
      .toBe(true);
    await page.getByRole("button", { name: "Pause" }).click();
    await page.waitForTimeout(100);

    expect(pageErrors).toEqual([]);
    expect(failedRequests).toEqual([]);
    await expect(page.getByRole("alert")).toHaveCount(0);
    await page.evaluate(() => window.scrollTo(0, 0));
    const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
    expect(screenshot.subarray(0, 8).toString("hex")).toBe("89504e470d0a1a0a");
    expect({ width: screenshot.readUInt32BE(16), height: screenshot.readUInt32BE(20) }).toEqual({
      width: 1440,
      height: 1000,
    });
    const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
    if (outputDirectory !== undefined) {
      await writeFile(path.join(outputDirectory, "android-dual-node-review.png"), screenshot);
    }
  } catch (caught) {
    const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
    if (outputDirectory !== undefined) {
      const screenshot = await page.screenshot({ animations: "disabled", fullPage: false });
      await writeFile(
        path.join(outputDirectory, "android-dual-node-review-failure.png"),
        screenshot,
      );
      await writeFile(
        path.join(outputDirectory, "android-dual-node-review-failure.json"),
        `${JSON.stringify(
          {
            error: caught instanceof Error ? caught.message : String(caught),
            page_errors: pageErrors,
            failed_requests: failedRequests.map((request) => {
              const parsed = new URL(request.split(" ")[0] ?? request);
              return `${parsed.origin}${parsed.pathname}`;
            }),
            body_text: await page.locator("body").innerText(),
          },
          null,
          2,
        )}\n`,
      );
    }
    throw caught;
  } finally {
    await close(staticServer);
  }
});

function readDualAndroidHilConfiguration(
  environment: Readonly<Record<string, string | undefined>>,
): DualAndroidHilConfiguration | null {
  const supplied = DUAL_ANDROID_HIL_ENVIRONMENT.filter(
    (name) => (environment[name] ?? "").length > 0,
  );
  if (supplied.length === 0) {
    return null;
  }
  const missing = DUAL_ANDROID_HIL_ENVIRONMENT.filter(
    (name) => (environment[name] ?? "").length === 0,
  );
  if (missing.length > 0) {
    throw new Error(
      `Dual Android browser HIL requires all four variables; missing: ${missing.join(", ")}`,
    );
  }

  const downTheLineNodeUrl = requireNodeOrigin(
    environment.SWING_CAPTURE_ANDROID_DTL_NODE_URL ?? "",
    "SWING_CAPTURE_ANDROID_DTL_NODE_URL",
  );
  const faceOnNodeUrl = requireNodeOrigin(
    environment.SWING_CAPTURE_ANDROID_FACE_NODE_URL ?? "",
    "SWING_CAPTURE_ANDROID_FACE_NODE_URL",
  );
  if (new URL(downTheLineNodeUrl).origin === new URL(faceOnNodeUrl).origin) {
    throw new Error("Dual Android browser HIL must identify two different origins");
  }
  return {
    downTheLineNodeUrl,
    downTheLineToken: requireNodeToken(
      environment.SWING_CAPTURE_ANDROID_DTL_TOKEN ?? "",
      "SWING_CAPTURE_ANDROID_DTL_TOKEN",
    ),
    faceOnNodeUrl,
    faceOnToken: requireNodeToken(
      environment.SWING_CAPTURE_ANDROID_FACE_TOKEN ?? "",
      "SWING_CAPTURE_ANDROID_FACE_TOKEN",
    ),
  };
}

function requireNodeOrigin(value: string, name: string): string {
  let parsed: URL;
  try {
    parsed = new URL(value);
  } catch {
    throw new Error(`${name} must be an HTTP(S) origin`);
  }
  if (
    (parsed.protocol !== "http:" && parsed.protocol !== "https:") ||
    parsed.username.length > 0 ||
    parsed.password.length > 0 ||
    parsed.pathname !== "/" ||
    parsed.search.length > 0 ||
    parsed.hash.length > 0 ||
    value.trim() !== value
  ) {
    throw new Error(`${name} must be an HTTP(S) origin without credentials, path, query, or hash`);
  }
  return parsed.origin;
}

function requireNodeToken(value: string, name: string): string {
  if (!/^[A-Za-z0-9_-]{32}$/.test(value)) {
    throw new Error(`${name} must be a 32-character URL-safe Android node token`);
  }
  return value;
}

function requestFailure(request: import("@playwright/test").Request): string | null {
  const requestUrl = new URL(request.url());
  requestUrl.searchParams.delete("dtl_token");
  requestUrl.searchParams.delete("face_token");
  const errorText = request.failure()?.errorText ?? "unknown failure";
  if (
    request.resourceType() === "media" &&
    requestUrl.pathname.endsWith(".mp4") &&
    errorText === "net::ERR_ABORTED"
  ) {
    return null;
  }
  return `${requestUrl.toString()}: ${errorText}`;
}

function parseFrameLabel(value: string | null): { current: number; total: number } {
  const match = /^Frame (\d+) of (\d+)$/.exec(value ?? "");
  if (match === null) {
    throw new Error(`Unexpected frame label: ${value ?? "missing"}`);
  }
  return { current: Number(match[1]), total: Number(match[2]) };
}

function videoTimes(videos: import("@playwright/test").Locator): Promise<number[]> {
  return videos.evaluateAll((elements) =>
    elements.map((element) => (element as HTMLVideoElement).currentTime),
  );
}

async function serveFixture(
  root: string,
  request: import("node:http").IncomingMessage,
  response: import("node:http").ServerResponse,
) {
  try {
    const pathname = new URL(request.url ?? "/", "http://fixture.invalid").pathname;
    const relativePath = pathname === "/" ? "demo.html" : pathname.slice(1);
    const requestedPath = path.resolve(root, relativePath);
    if (requestedPath !== root && !requestedPath.startsWith(`${root}${path.sep}`)) {
      response.writeHead(403).end("Forbidden");
      return;
    }
    await serveFile(requestedPath, request, response);
  } catch {
    response.writeHead(404).end("Not found");
  }
}

async function serveHilApplication(
  staticRoot: string,
  artifactDirectory: string,
  manifestJson: string,
  manifest: HilManifestSummary,
  requestedMedia: Set<string>,
  request: import("node:http").IncomingMessage,
  response: import("node:http").ServerResponse,
) {
  try {
    const pathname = new URL(request.url ?? "/", "http://hil.invalid").pathname;
    if (pathname === "/api/v1/capture/status") {
      serveJson(response, {
        schema_version: 2,
        state: "ready",
        armed: false,
        active_session_id: manifest.sessionId,
        error: "",
        hil: {
          enabled: false,
          busy: false,
          stage: "idle",
          error: "",
          last_run: null,
        },
      });
      return;
    }
    if (pathname === "/api/v1/sessions") {
      serveJson(response, {
        schema_version: 1,
        sessions: [
          {
            session_id: manifest.sessionId,
            state: "ready",
            created_at_utc: manifest.createdAtUtc,
            error: "",
          },
        ],
      });
      return;
    }

    const sessionBase = `/api/v1/sessions/${encodeURIComponent(manifest.sessionId)}/`;
    if (pathname === `${sessionBase}manifest`) {
      response.writeHead(200, {
        "Content-Length": Buffer.byteLength(manifestJson),
        "Content-Type": "application/json; charset=utf-8",
      });
      response.end(manifestJson);
      return;
    }
    if (pathname.startsWith(sessionBase)) {
      const mediaPath = pathname.slice(sessionBase.length);
      if (!manifest.mediaPaths.includes(mediaPath)) {
        response.writeHead(404).end("Not found");
        return;
      }
      requestedMedia.add(mediaPath);
      await serveFile(path.join(artifactDirectory, mediaPath), request, response);
      return;
    }

    const relativePath = pathname === "/" ? "index.html" : pathname.slice(1);
    const requestedPath = path.resolve(staticRoot, relativePath);
    if (requestedPath !== staticRoot && !requestedPath.startsWith(`${staticRoot}${path.sep}`)) {
      response.writeHead(403).end("Forbidden");
      return;
    }
    await serveFile(requestedPath, request, response);
  } catch {
    response.writeHead(404).end("Not found");
  }
}

async function serveFile(
  filename: string,
  request: import("node:http").IncomingMessage,
  response: import("node:http").ServerResponse,
) {
  const content = await readFile(filename);
  const range = parseRange(request.headers.range, content.length);
  if (range !== null) {
    response.writeHead(206, {
      "Accept-Ranges": "bytes",
      "Content-Length": range.end - range.start + 1,
      "Content-Range": `bytes ${range.start}-${range.end}/${content.length}`,
      "Content-Type": mediaType(filename),
    });
    response.end(content.subarray(range.start, range.end + 1));
    return;
  }
  response.writeHead(200, {
    "Accept-Ranges": "bytes",
    "Content-Length": content.length,
    "Content-Type": mediaType(filename),
  });
  response.end(content);
}

function serveJson(response: import("node:http").ServerResponse, value: unknown) {
  const body = JSON.stringify(value);
  response.writeHead(200, {
    "Content-Length": Buffer.byteLength(body),
    "Content-Type": "application/json; charset=utf-8",
  });
  response.end(body);
}

function summarizeHilManifest(value: unknown): HilManifestSummary {
  const object = requireObject(value, "manifest");
  const sessionId = requireString(object.session_id, "manifest.session_id");
  const createdAtUtc = requireString(object.created_at_utc, "manifest.created_at_utc");
  if (!Array.isArray(object.views) || object.views.length !== 2) {
    throw new Error("manifest.views must contain two camera views");
  }
  const views = object.views.map((value, index) => {
    const view = requireObject(value, `manifest.views[${index}]`);
    const role = requireString(view.role, `manifest.views[${index}].role`);
    const media = requireObject(view.media, `manifest.views[${index}].media`);
    const mediaPath = requireString(media.path, `manifest.views[${index}].media.path`);
    if (path.basename(mediaPath) !== mediaPath) {
      throw new Error(`HIL media path is not a safe filename: ${mediaPath}`);
    }
    const frameCount = requireInteger(view.frame_count, `manifest.views[${index}].frame_count`);
    const impactFrameIndex = requireInteger(
      view.impact_frame_index,
      `manifest.views[${index}].impact_frame_index`,
    );
    if (frameCount < 2 || impactFrameIndex < 0 || impactFrameIndex >= frameCount) {
      throw new Error(`manifest.views[${index}] has invalid frame bounds`);
    }
    return { role, mediaPath, frameCount, impactFrameIndex };
  });
  const reference = views.find((view) => view.role === "down_the_line");
  if (reference === undefined) {
    throw new Error("manifest is missing its down_the_line reference view");
  }
  return {
    sessionId,
    createdAtUtc,
    mediaPaths: views.map((view) => view.mediaPath),
    frameCount: reference.frameCount,
    impactFrameIndex: reference.impactFrameIndex,
  };
}

function requireObject(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function requireString(value: unknown, label: string): string {
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${label} must be a nonempty string`);
  }
  return value;
}

function requireInteger(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value)) {
    throw new Error(`${label} must be an integer`);
  }
  return value;
}

async function listen(serverToListen: Server): Promise<string> {
  await new Promise<void>((resolve, reject) => {
    serverToListen.once("error", reject);
    serverToListen.listen(0, "127.0.0.1", resolve);
  });
  const address = serverToListen.address();
  if (address === null || typeof address === "string") {
    throw new Error("HIL fixture server did not bind a TCP address");
  }
  return `http://127.0.0.1:${address.port}`;
}

async function close(serverToClose: Server): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    serverToClose.close((error) => (error === undefined ? resolve() : reject(error)));
  });
}

function parseRange(
  header: string | undefined,
  size: number,
): { start: number; end: number } | null {
  const match = /^bytes=(\d+)-(\d*)$/.exec(header ?? "");
  if (match === null) {
    return null;
  }
  const start = Number(match[1]);
  const requestedEnd = match[2] === "" ? size - 1 : Number(match[2]);
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(requestedEnd) || start >= size) {
    return null;
  }
  return { start, end: Math.min(size - 1, requestedEnd) };
}

function mediaType(filename: string): string {
  switch (path.extname(filename)) {
    case ".css":
      return "text/css; charset=utf-8";
    case ".html":
      return "text/html; charset=utf-8";
    case ".js":
      return "text/javascript; charset=utf-8";
    case ".json":
      return "application/json; charset=utf-8";
    case ".svg":
      return "image/svg+xml";
    case ".webm":
      return "video/webm";
    default:
      return "application/octet-stream";
  }
}
