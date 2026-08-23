import { defineConfig } from "@playwright/test";

type BrowserName = "chromium" | "firefox" | "webkit";

function isBrowserName(value: string): value is BrowserName {
  return value === "chromium" || value === "firefox" || value === "webkit";
}

const configuredBrowserName = process.env.SWING_CAPTURE_BROWSER_NAME ?? "chromium";
if (!isBrowserName(configuredBrowserName)) {
  throw new Error(
    `SWING_CAPTURE_BROWSER_NAME must be chromium, firefox, or webkit; got ${configuredBrowserName}`,
  );
}
const browserName = configuredBrowserName;
const legacyChromeExecutable = process.env.SWING_CAPTURE_CHROME_EXECUTABLE;
if (legacyChromeExecutable !== undefined && browserName !== "chromium") {
  throw new Error("SWING_CAPTURE_CHROME_EXECUTABLE may only be used with the Chromium engine");
}
const browserExecutable = process.env.SWING_CAPTURE_BROWSER_EXECUTABLE ?? legacyChromeExecutable;

export default defineConfig({
  testDir: "./src",
  testMatch: ["review.e2e.spec.ts", "production_integration.e2e.spec.ts"],
  fullyParallel: false,
  // The review fixture stays serial, while the production matrix opts into per-worker isolated
  // harnesses and ephemeral ports. Four workers reduce the checkpoint critical path without
  // approaching the repository's eight-job build ceiling.
  workers: 4,
  retries: 0,
  // The pinned WPE MiniBrowser cold-starts its GStreamer pipeline on demand;
  // keep the fast Chromium loop unchanged while giving the opt-in WebKit gate
  // enough time to establish and seek both H.264 decoders.
  timeout: browserName === "webkit" ? 30_000 : 10_000,
  expect: { timeout: browserName === "webkit" ? 10_000 : 3_000 },
  use: {
    browserName,
    headless: true,
    ...(browserExecutable === undefined
      ? {}
      : { launchOptions: { executablePath: browserExecutable } }),
    viewport: { width: 1440, height: 1000 },
  },
});
