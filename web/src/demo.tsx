import { StrictMode, useEffect, useState } from "react";
import { createRoot } from "react-dom/client";
import { Application, viewFromHash } from "./application.js";
import { FakeStationApi, FIXTURE_STATUS } from "./fake_api.js";
import { FakeNodeSetupApi } from "./fake_node_setup_api.js";
import {
  FakeFieldRecordingReviewApi,
  FakeReviewApi,
  FakeUiLabReviewApi,
  type UiLabScenario,
  UI_LAB_SCENARIOS,
} from "./fake_review_api.js";
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
const fieldRecordingMode = parameters.get("field_recording") === "1";
const collectionMode = parameters.get("collection") === "1";
const uiLabMode = parameters.get("ui_lab") === "1";
const stationStatus = structuredClone(FIXTURE_STATUS);
if (parameters.get("preview_stall") === "capture_sink") {
  const camera = stationStatus.cameras.find((candidate) => candidate.role === "down_the_line");
  if (camera !== undefined) {
    camera.preview_performance.latest_capture_frame_id = "801";
    camera.preview_performance.latest_sink_frame_id = "800";
    camera.preview_performance.latest_capture_age_ms = 3_200;
    camera.preview_performance.source_age_ms = 3_300;
  }
}
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

const defaultReviewApi = collectionMode
  ? new FakeUiLabReviewApi()
  : new (fieldRecordingMode ? FakeFieldRecordingReviewApi : FakeReviewApi)({
      ...(peerArmState === undefined ? {} : { peerArmState }),
      ...(poseMode === undefined ? {} : { poseMode }),
      ...(posePhase === undefined ? {} : { posePhase }),
    });

const application = (
  <Application
    initialView={viewFromHash(window.location.hash)}
    {...(nodeSetupApis === undefined ? {} : { nodeSetupApis })}
    pollIntervalMs={100}
    reviewApi={defaultReviewApi}
    stationApi={new FakeStationApi(stationStatus)}
  />
);

createRoot(root).render(
  <StrictMode>
    {uiLabMode ? <UiLabApplication stationStatus={stationStatus} /> : application}
  </StrictMode>,
);

function UiLabApplication({ stationStatus: status }: { stationStatus: typeof FIXTURE_STATUS }) {
  const [api] = useState(() => new FakeUiLabReviewApi());
  const [scenario, setScenario] = useState<UiLabScenario>(api.scenario);

  useEffect(() => api.subscribeToScenarioChanges(setScenario), [api]);

  return (
    <>
      <aside aria-label="UI lab scenarios" className="ui-lab-scenario-switcher">
        <fieldset>
          <legend>UI lab scenario</legend>
          {UI_LAB_SCENARIOS.map((candidate) => (
            <button
              aria-pressed={scenario === candidate}
              key={candidate}
              onClick={() => api.setScenario(candidate)}
              type="button"
            >
              {uiLabScenarioLabel(candidate)}
            </button>
          ))}
        </fieldset>
      </aside>
      <Application
        initialView={viewFromHash(window.location.hash)}
        pollIntervalMs={100}
        reviewApi={api}
        setupAvailable={false}
        stationApi={new FakeStationApi(status)}
      />
    </>
  );
}

function uiLabScenarioLabel(scenario: UiLabScenario): string {
  switch (scenario) {
    case "review_ready":
      return "Review ready";
    case "armed_monitoring":
      return "Armed / monitoring";
    case "processing":
      return "Processing";
  }
}
