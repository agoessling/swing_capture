import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { Application, viewFromHash } from "./application.js";
import { FakeStationApi } from "./fake_api.js";
import { FakeNodeSetupApi } from "./fake_node_setup_api.js";
import { FakeReviewApi } from "./fake_review_api.js";
import {
  PEER_ARM_STATES,
  type PeerArmState,
  POSE_MODES,
  type PoseMode,
  POSE_PHASES,
  type PosePhase,
} from "./review_api.js";
import "./styles.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("Missing #root application mount");
}

const parameters = new URLSearchParams(window.location.search);
const phoneSetupMode = parameters.get("phone_setup");
const requestedPeerArmState = parameters.get("peer_arm");
const peerArmState =
  requestedPeerArmState !== null &&
  (PEER_ARM_STATES as readonly string[]).includes(requestedPeerArmState)
    ? (requestedPeerArmState as PeerArmState)
    : undefined;
const requestedPoseMode = parameters.get("pose_mode");
const poseMode =
  requestedPoseMode !== null && (POSE_MODES as readonly string[]).includes(requestedPoseMode)
    ? (requestedPoseMode as PoseMode)
    : undefined;
const requestedPosePhase = parameters.get("pose_phase");
const posePhase =
  requestedPosePhase !== null && (POSE_PHASES as readonly string[]).includes(requestedPosePhase)
    ? (requestedPosePhase as PosePhase)
    : undefined;
const nodeSetupApis =
  phoneSetupMode === null
    ? undefined
    : phoneSetupMode === "dual" || phoneSetupMode === "dual_invalid_pose"
      ? phoneSetupMode === "dual_invalid_pose"
        ? [
            new FakeNodeSetupApi(
              "http://pixel-6-pro.test:8088",
              "down_the_line",
              "Pixel 6 Pro",
              "leader",
              "http://different-shadow.test:8088",
            ),
            new FakeNodeSetupApi("http://pixel-5a.test:8088", "face_on", "Pixel 5a", "shadow"),
          ]
        : [
            new FakeNodeSetupApi("http://pixel-6-pro.test:8088", "down_the_line", "Pixel 6 Pro"),
            new FakeNodeSetupApi("http://pixel-5a.test:8088", "face_on", "Pixel 5a"),
          ]
      : [new FakeNodeSetupApi("http://pixel-6-pro.test:8088")];

createRoot(root).render(
  <StrictMode>
    <Application
      initialView={viewFromHash(window.location.hash)}
      {...(nodeSetupApis === undefined ? {} : { nodeSetupApis })}
      pollIntervalMs={100}
      reviewApi={
        new FakeReviewApi({
          ...(peerArmState === undefined ? {} : { peerArmState }),
          ...(poseMode === undefined ? {} : { poseMode }),
          ...(posePhase === undefined ? {} : { posePhase }),
        })
      }
      stationApi={new FakeStationApi()}
    />
  </StrictMode>,
);
