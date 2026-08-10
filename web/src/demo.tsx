import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Application, viewFromHash } from "./application.js";
import { FakeStationApi } from "./fake_api.js";
import { FakeReviewApi } from "./fake_review_api.js";
import "./styles.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("Missing #root application mount");
}

createRoot(root).render(
  <StrictMode>
    <Application
      initialView={viewFromHash(window.location.hash)}
      pollIntervalMs={100}
      reviewApi={new FakeReviewApi()}
      stationApi={new FakeStationApi()}
    />
  </StrictMode>,
);
