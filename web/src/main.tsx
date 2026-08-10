import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Application, viewFromHash } from "./application.js";
import { HttpStationApi } from "./api.js";
import { HttpReviewApi } from "./review_api.js";
import "./styles.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("Missing #root application mount");
}

createRoot(root).render(
  <StrictMode>
    <Application
      initialView={viewFromHash(window.location.hash)}
      reviewApi={new HttpReviewApi()}
      stationApi={new HttpStationApi()}
    />
  </StrictMode>,
);
