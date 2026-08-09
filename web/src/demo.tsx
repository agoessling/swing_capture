import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app.js";
import { FakeStationApi } from "./fake_api.js";
import "./styles.css";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("Missing #root application mount");
}

createRoot(root).render(
  <StrictMode>
    <App api={new FakeStationApi()} />
  </StrictMode>,
);
