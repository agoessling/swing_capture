import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Application, viewFromHash } from "./application.js";
import { HttpStationApi } from "./api.js";
import {
  consumeControlCredentials,
  credentialFreeUrl,
  retainSingleNodeAndroidMode,
} from "./control_credential_bootstrap.js";
import { DualNodeReviewApi } from "./dual_node_review_api.js";
import { HttpNodeSetupApi } from "./node_setup_api.js";
import { HttpReviewApi } from "./review_api.js";
import { isAndroidReviewMode, reviewEventsSupported } from "./review_boot.js";
import "./styles.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("Missing #root application mount");
}

const parameters = new URLSearchParams(window.location.search);
const nodeBaseUrl = parameters.get("node") ?? "";
const downTheLineNode = parameters.get("dtl_node");
const faceOnNode = parameters.get("face_node");
const androidReviewMode = isAndroidReviewMode(parameters);
const eventsSupported = reviewEventsSupported(parameters);
retainSingleNodeAndroidMode(parameters);
const consumedCredentials = consumeControlCredentials(parameters, window.sessionStorage, [
  {
    parameter: "node_token",
    scope: new URL(nodeBaseUrl || window.location.origin, window.location.href).origin,
  },
  {
    parameter: "dtl_token",
    scope:
      downTheLineNode === null
        ? "unconfigured-down-the-line-node"
        : new URL(downTheLineNode, window.location.href).origin,
  },
  {
    parameter: "face_token",
    scope:
      faceOnNode === null
        ? "unconfigured-face-on-node"
        : new URL(faceOnNode, window.location.href).origin,
  },
]);
if (consumedCredentials.removedFromUrl) {
  history.replaceState(null, "", credentialFreeUrl(window.location.href, parameters));
}
const nodeControlCredential = consumedCredentials.credentials.get("node_token");
const downTheLineControlCredential = consumedCredentials.credentials.get("dtl_token");
const faceOnControlCredential = consumedCredentials.credentials.get("face_token");
if (
  nodeControlCredential === undefined ||
  downTheLineControlCredential === undefined ||
  faceOnControlCredential === undefined
) {
  throw new Error("Control credential bootstrap is incomplete");
}
if ((downTheLineNode === null) !== (faceOnNode === null)) {
  throw new Error("Configure both dtl_node and face_node for dual-node Android capture");
}
const reviewApi =
  downTheLineNode !== null && faceOnNode !== null
    ? new DualNodeReviewApi([
        {
          baseUrl: downTheLineNode,
          controlToken: downTheLineControlCredential,
          role: "down_the_line",
        },
        {
          baseUrl: faceOnNode,
          controlToken: faceOnControlCredential,
          role: "face_on",
        },
      ])
    : new HttpReviewApi(nodeBaseUrl, undefined, undefined, nodeControlCredential, eventsSupported);
const nodeSetupApis =
  downTheLineNode !== null && faceOnNode !== null
    ? [
        new HttpNodeSetupApi(downTheLineNode, downTheLineControlCredential),
        new HttpNodeSetupApi(faceOnNode, faceOnControlCredential),
      ]
    : androidReviewMode
      ? [new HttpNodeSetupApi(nodeBaseUrl, nodeControlCredential)]
      : undefined;

createRoot(root).render(
  <StrictMode>
    <Application
      initialView={viewFromHash(window.location.hash)}
      {...(nodeSetupApis === undefined ? {} : { nodeSetupApis })}
      reviewApi={reviewApi}
      setupAvailable
      stationApi={new HttpStationApi()}
    />
  </StrictMode>,
);
