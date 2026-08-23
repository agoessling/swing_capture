import { defineConfig } from "@playwright/test";

const chromeExecutable = process.env.SWING_CAPTURE_CHROME_EXECUTABLE;
if (chromeExecutable === undefined || chromeExecutable.length === 0) {
  throw new Error("physical browser release HIL requires installed Google Chrome");
}

export default defineConfig({
  testDir: "./src",
  testMatch: ["android_field_recording_hil.e2e.spec.ts"],
  fullyParallel: false,
  workers: 1,
  retries: 0,
  // Camera ownership remains independently bounded to 15 seconds in the test. Leave enough whole-
  // test time for two-origin catalog setup, Chrome decode/accessibility checks, and fail-closed
  // cleanup without allowing those non-camera stages to consume the physical-stage budget.
  timeout: 20_000,
  expect: { timeout: 3_000 },
  use: {
    browserName: "chromium",
    headless: true,
    launchOptions: { executablePath: chromeExecutable },
    viewport: { width: 1440, height: 1000 },
  },
});
