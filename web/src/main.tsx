import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Application, viewFromHash } from "./application.js";
import { HttpStationApi } from "./api.js";
import { DualNodeReviewApi } from "./dual_node_review_api.js";
import { HttpReviewApi } from "./review_api.js";
import { isAndroidReviewMode, reviewEventsSupported } from "./review_boot.js";
import "./styles.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("Missing #root application mount");
}

const parameters = new URLSearchParams(window.location.search);
const nodeBaseUrl = parameters.get("node") ?? "";
const nodeControlToken = parameters.get("node_token") ?? "";
const downTheLineNode = parameters.get("dtl_node");
const faceOnNode = parameters.get("face_node");
const androidReviewMode = isAndroidReviewMode(parameters);
if ((downTheLineNode === null) !== (faceOnNode === null)) {
  throw new Error("Configure both dtl_node and face_node for dual-node Android capture");
}
const reviewApi =
  downTheLineNode !== null && faceOnNode !== null
    ? new DualNodeReviewApi([
        {
          baseUrl: downTheLineNode,
          controlToken: parameters.get("dtl_token") ?? "",
          role: "down_the_line",
        },
        {
          baseUrl: faceOnNode,
          controlToken: parameters.get("face_token") ?? "",
          role: "face_on",
        },
      ])
    : new HttpReviewApi(
        nodeBaseUrl,
        undefined,
        undefined,
        nodeControlToken,
        reviewEventsSupported(parameters),
      );

createRoot(root).render(
  <StrictMode>
    <Application
      initialView={viewFromHash(window.location.hash)}
      reviewApi={reviewApi}
      setupAvailable={!androidReviewMode}
      stationApi={new HttpStationApi()}
    />
  </StrictMode>,
);
