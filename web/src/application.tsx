import { useState } from "react";
import type { StationApi } from "./api.js";
import { App as CameraSetupApp } from "./app.js";
import type { NodeSetupApi } from "./node_setup_api.js";
import { NodeSetupApp } from "./node_setup_app.js";
import type { ReviewApi } from "./review_api.js";
import { ReviewApp } from "./review_app.js";

export type ApplicationView = "review" | "setup";

export interface ApplicationProps {
  stationApi: StationApi;
  reviewApi: ReviewApi;
  initialView?: ApplicationView;
  pollIntervalMs?: number;
  setupAvailable?: boolean;
  nodeSetupApis?: readonly NodeSetupApi[];
}

export function Application({
  stationApi,
  reviewApi,
  initialView = "review",
  pollIntervalMs,
  setupAvailable = true,
  nodeSetupApis,
}: ApplicationProps) {
  const [view, setView] = useState(
    initialView === "setup" && !setupAvailable ? "review" : initialView,
  );
  const polling = pollIntervalMs === undefined ? {} : { pollIntervalMs };

  const selectView = (nextView: ApplicationView) => {
    setView(nextView);
    if (typeof history !== "undefined") {
      history.replaceState(null, "", `#${nextView}`);
    }
  };

  return (
    <div
      className={`application-shell application-${view}${setupAvailable ? "" : " application-single-view"}`}
    >
      {setupAvailable ? (
        <nav aria-label="Primary" className="application-nav">
          <strong>
            <span aria-hidden="true" className="application-mark">
              S
            </span>
            <span>Swing Capture</span>
          </strong>
          <div>
            <button
              aria-current={view === "review" ? "page" : undefined}
              className={view === "review" ? "active" : ""}
              onClick={() => selectView("review")}
              type="button"
            >
              Review
            </button>
            <button
              aria-current={view === "setup" ? "page" : undefined}
              className={view === "setup" ? "active" : ""}
              onClick={() => selectView("setup")}
              type="button"
            >
              {nodeSetupApis === undefined ? "Camera setup" : "Phone setup"}
            </button>
          </div>
        </nav>
      ) : null}
      {view === "review" ? (
        <ReviewApp api={reviewApi} {...polling} />
      ) : nodeSetupApis === undefined ? (
        <CameraSetupApp api={stationApi} {...polling} />
      ) : (
        <NodeSetupApp apis={nodeSetupApis} />
      )}
    </div>
  );
}

export function viewFromHash(hash: string): ApplicationView {
  return hash === "#setup" ? "setup" : "review";
}
