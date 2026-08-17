import { useState } from "react";
import type { StationApi } from "./api.js";
import { App as CameraSetupApp } from "./app.js";
import type { ReviewApi } from "./review_api.js";
import { ReviewApp } from "./review_app.js";

export type ApplicationView = "review" | "setup";

export interface ApplicationProps {
  stationApi: StationApi;
  reviewApi: ReviewApi;
  initialView?: ApplicationView;
  pollIntervalMs?: number;
  setupAvailable?: boolean;
}

export function Application({
  stationApi,
  reviewApi,
  initialView = "review",
  pollIntervalMs,
  setupAvailable = true,
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
    <>
      <nav aria-label="Primary" className="application-nav">
        <strong>Swing Capture</strong>
        <div>
          <button
            aria-current={view === "review" ? "page" : undefined}
            className={view === "review" ? "active" : ""}
            onClick={() => selectView("review")}
            type="button"
          >
            Review
          </button>
          {setupAvailable ? (
            <button
              aria-current={view === "setup" ? "page" : undefined}
              className={view === "setup" ? "active" : ""}
              onClick={() => selectView("setup")}
              type="button"
            >
              Camera setup
            </button>
          ) : null}
        </div>
      </nav>
      {view === "review" ? (
        <ReviewApp api={reviewApi} {...polling} />
      ) : (
        <CameraSetupApp api={stationApi} {...polling} />
      )}
    </>
  );
}

export function viewFromHash(hash: string): ApplicationView {
  return hash === "#setup" ? "setup" : "review";
}
