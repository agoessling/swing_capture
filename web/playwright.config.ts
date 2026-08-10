import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./src",
  testMatch: "review.e2e.spec.ts",
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 10_000,
  expect: { timeout: 3_000 },
  use: {
    browserName: "chromium",
    headless: true,
    viewport: { width: 1440, height: 1000 },
  },
});
