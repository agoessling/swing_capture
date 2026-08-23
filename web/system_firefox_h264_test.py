"""Opt-in Firefox qualification for exact-frame AVC review playback.

This test deliberately uses the installed Firefox and geckodriver.  It is a
local/manual browser qualification, not part of the hermetic default suite.
"""

from __future__ import annotations

import contextlib
import http.client
import http.server
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time
from typing import Any
import urllib.error
import urllib.request


_FIREFOX = Path(
    os.environ.get(
        "SWING_CAPTURE_FIREFOX_EXECUTABLE",
        "/snap/firefox/current/usr/lib/firefox/firefox",
    )
)
_GECKODRIVER = Path(os.environ.get("SWING_CAPTURE_GECKODRIVER_EXECUTABLE", "/snap/bin/geckodriver"))
_RANGE_PATTERN = re.compile(r"bytes=(\d+)-(\d*)")


class _FixtureHandler(http.server.BaseHTTPRequestHandler):
    fixture_root: Path
    range_requests: dict[str, int] = {}
    lock = threading.Lock()

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/":
            self._send_bytes(
                b"<!doctype html><meta charset=utf-8><title>AVC probe</title>", "text/html"
            )
            return
        name = self.path.removeprefix("/")
        if name not in {"down-the-line-gop30.mp4", "face-on-gop30.mp4"}:
            self.send_error(http.HTTPStatus.NOT_FOUND)
            return
        payload = (self.fixture_root / name).read_bytes()
        range_header = self.headers.get("Range")
        if range_header is None:
            self._send_bytes(payload, "video/mp4", {"Accept-Ranges": "bytes"})
            return
        match = _RANGE_PATTERN.fullmatch(range_header.strip())
        if match is None:
            self.send_error(http.HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
            return
        begin = int(match.group(1))
        end = int(match.group(2)) if match.group(2) else len(payload) - 1
        if begin >= len(payload) or end < begin:
            self.send_response(http.HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
            self.send_header("Content-Range", f"bytes */{len(payload)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        end = min(end, len(payload) - 1)
        with self.lock:
            self.range_requests[name] = self.range_requests.get(name, 0) + 1
        self._send_bytes(
            payload[begin : end + 1],
            "video/mp4",
            {
                "Accept-Ranges": "bytes",
                "Content-Range": f"bytes {begin}-{end}/{len(payload)}",
            },
            http.HTTPStatus.PARTIAL_CONTENT,
        )

    def log_message(self, _format: str, *_arguments: object) -> None:
        return

    def _send_bytes(
        self,
        payload: bytes,
        content_type: str,
        headers: dict[str, str] | None = None,
        status: http.HTTPStatus = http.HTTPStatus.OK,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)


def _unused_local_port() -> int:
    with socket.socket() as candidate:
        candidate.bind(("127.0.0.1", 0))
        return int(candidate.getsockname()[1])


def _webdriver_request(
    port: int,
    method: str,
    path: str,
    body: dict[str, Any] | None = None,
    *,
    timeout: float = 10.0,
) -> Any:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    encoded = None if body is None else json.dumps(body).encode()
    headers = {} if encoded is None else {"Content-Type": "application/json"}
    try:
        connection.request(method, path, body=encoded, headers=headers)
        response = connection.getresponse()
        payload = response.read()
    finally:
        connection.close()
    document = json.loads(payload) if payload else {"value": None}
    if not 200 <= response.status < 300:
        raise RuntimeError(
            f"WebDriver {method} {path} failed with HTTP {response.status}: {document}"
        )
    return document.get("value")


def _wait_for_webdriver(port: int, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 10.0
    status_url = f"http://127.0.0.1:{port}/status"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"geckodriver exited before becoming ready ({process.returncode})")
        try:
            with urllib.request.urlopen(status_url, timeout=0.25) as response:
                if response.status == http.HTTPStatus.OK:
                    return
        except (OSError, urllib.error.URLError):
            time.sleep(0.05)
    raise RuntimeError("geckodriver did not become ready within 10 seconds")


_PROBE_SCRIPT = r"""
const done = arguments[arguments.length - 1];
const sources = arguments[0];
const timeout = (promise, label) => Promise.race([
  promise,
  new Promise((_, reject) => setTimeout(() => reject(new Error(`${label} timed out`)), 5000)),
]);
const load = async (source) => {
  const video = document.createElement("video");
  video.muted = true;
  video.preload = "auto";
  document.body.append(video);
  const loaded = new Promise((resolve, reject) => {
    video.addEventListener("loadeddata", resolve, { once: true });
    video.addEventListener("error", () => reject(new Error(`decode failed for ${source}`)), {
      once: true,
    });
  });
  video.src = source;
  video.load();
  await timeout(loaded, `load ${source}`);
  return video;
};
const snapshot = (video) => {
  const canvas = document.createElement("canvas");
  canvas.width = 80;
  canvas.height = 45;
  const context = canvas.getContext("2d", { willReadFrequently: true });
  if (context === null) throw new Error("2D canvas unavailable");
  context.drawImage(video, 0, 0, canvas.width, canvas.height);
  const pixels = context.getImageData(0, 0, canvas.width, canvas.height).data;
  let hash = 2166136261;
  let nonblack = 0;
  for (let index = 0; index < pixels.length; index += 4) {
    const red = pixels[index] ?? 0;
    const green = pixels[index + 1] ?? 0;
    const blue = pixels[index + 2] ?? 0;
    if (red + green + blue > 24) nonblack += 1;
    hash = Math.imul(hash ^ red, 16777619);
    hash = Math.imul(hash ^ green, 16777619);
    hash = Math.imul(hash ^ blue, 16777619);
  }
  return {
    pixel_hash: (hash >>> 0).toString(16).padStart(8, "0"),
    nonblack_fraction: nonblack / (pixels.length / 4),
  };
};
const inspect = async (video, frameIndex) => {
  const expected = Math.round(frameIndex * 1000000 / 30) / 1000000;
  const seekTime = (frameIndex / 30) + (1 / 60);
  let observations = 0;
  const observedMediaTimes = [];
  let presentationNudge = false;
  let completed = false;
  let nudgeTimer = 0;
  const originalPlaybackRate = video.playbackRate;
  const pausedSeekSnapshot = new Promise((resolve) => {
    video.addEventListener("seeked", () => requestAnimationFrame(() => requestAnimationFrame(
      () => resolve(snapshot(video))
    )), { once: true });
  });
  const presented = new Promise((resolve, reject) => {
    const callback = async (_now, metadata) => {
      observations += 1;
      observedMediaTimes.push(metadata.mediaTime);
      if (Math.abs(metadata.mediaTime - expected) > 0.0001) {
        if (presentationNudge && metadata.mediaTime > expected) {
          video.pause();
          video.playbackRate = originalPlaybackRate;
          reject(new Error(
            `presentation nudge skipped frame ${frameIndex}; observed ${observedMediaTimes.join(",")}`
          ));
          return;
        }
        if (observations >= 8) {
          reject(new Error(
            `frame ${frameIndex} expected mediaTime ${expected}, observed ${observedMediaTimes.join(",")}`
          ));
          return;
        }
        video.requestVideoFrameCallback(callback);
        return;
      }
      completed = true;
      clearTimeout(nudgeTimer);
      video.pause();
      video.playbackRate = originalPlaybackRate;
      const paintedPausedSeek = await pausedSeekSnapshot;
      const confirmedPresentation = snapshot(video);
      if (paintedPausedSeek.pixel_hash !== confirmedPresentation.pixel_hash) {
        reject(new Error(
          `paused seek painted ${paintedPausedSeek.pixel_hash}, exact frame ${frameIndex} is ${confirmedPresentation.pixel_hash}`
        ));
        return;
      }
      resolve({
        frame_index: frameIndex,
        media_time_seconds: metadata.mediaTime,
        current_time_seconds: video.currentTime,
        presentation_nudge: presentationNudge,
        paused_seek_pixel_hash: paintedPausedSeek.pixel_hash,
        ...confirmedPresentation,
      });
    };
    video.requestVideoFrameCallback(callback);
    video.currentTime = seekTime;
    // Firefox 153 decodes a paused seek and advances currentTime, but does not enqueue a
    // post-seek requestVideoFrameCallback.  Start the already-muted element only when that
    // callback stalls, then pause synchronously on the exact requested presentation.  Chrome's
    // ordinary paused-seek behavior never reaches this recovery path.
    nudgeTimer = setTimeout(() => {
      if (completed) return;
      presentationNudge = true;
      video.playbackRate = 0.25;
      video.currentTime = Math.max(0, ((frameIndex - 1) / 30) + (1 / 60));
      video.play().catch((error) => reject(new Error(
        `presentation nudge failed: ${error instanceof Error ? error.message : String(error)}`
      )));
    }, 200);
  });
  return Promise.race([
    presented,
    new Promise((_, reject) => setTimeout(
      () => reject(new Error(
        `paused exact-frame ${frameIndex} timed out; callbacks=${observations} currentTime=${video.currentTime}`
      )),
      5000,
    )),
  ]);
};
(async () => {
  const codec = document.createElement("video").canPlayType('video/mp4; codecs="avc1.42C01E"');
  if (codec === "") throw new Error("Firefox does not advertise AVC/H.264 Constrained Baseline");
  if (typeof HTMLVideoElement.prototype.requestVideoFrameCallback !== "function") {
    throw new Error("requestVideoFrameCallback is unavailable");
  }
  const results = [];
  for (const source of sources) {
    const video = await load(source);
    if (video.videoWidth !== 320 || video.videoHeight !== 180 || Math.abs(video.duration - 3) > 0.01) {
      throw new Error(`unexpected media geometry/duration: ${video.videoWidth}x${video.videoHeight} ${video.duration}`);
    }
    const frames = [];
    for (const frameIndex of [61, 28, 29, 30, 29]) frames.push(await inspect(video, frameIndex));
    if (frames.some((frame) => frame.nonblack_fraction <= 0.2)) {
      throw new Error("decoded frame is black or empty");
    }
    if (frames[2].pixel_hash !== frames[4].pixel_hash) {
      throw new Error("reverse seek did not reproduce frame 29 pixels");
    }
    if (new Set(frames.map((frame) => frame.pixel_hash)).size < 3) {
      throw new Error("requested frames are not visibly distinct");
    }
    video.currentTime = 0.95;
    await timeout(video.play(), "play");
    await timeout(new Promise((resolve) => {
      const poll = () => video.currentTime > 1.0 ? resolve() : setTimeout(poll, 10);
      poll();
    }), "playback advance");
    video.pause();
    results.push({
      source,
      width: video.videoWidth,
      height: video.videoHeight,
      duration: video.duration,
      frames,
    });
    video.remove();
  }
  done({ ok: true, codec, request_video_frame_callback: true, results });
})().catch((error) => done({ ok: false, error: error instanceof Error ? error.message : String(error) }));
"""


def _fixture_root() -> Path:
    test_srcdir = os.environ.get("TEST_SRCDIR")
    test_workspace = os.environ.get("TEST_WORKSPACE")
    if test_srcdir is None or test_workspace is None:
        return Path(__file__).resolve().parent / "fixtures" / "production_h264"
    return Path(test_srcdir) / test_workspace / "web" / "fixtures" / "production_h264"


def main() -> None:
    for executable in (_FIREFOX, _GECKODRIVER):
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise RuntimeError(f"required system browser executable is unavailable: {executable}")
    fixture_root = _fixture_root()
    _FixtureHandler.fixture_root = fixture_root
    _FixtureHandler.range_requests = {}
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FixtureHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    webdriver_port = _unused_local_port()
    session_id: str | None = None
    with tempfile.TemporaryDirectory(prefix="swing-firefox-") as temporary_directory:
        driver_log_path = Path(temporary_directory) / "geckodriver.log"
        with driver_log_path.open("w", encoding="utf-8") as driver_log:
            driver = subprocess.Popen(
                [str(_GECKODRIVER), "--host", "127.0.0.1", "--port", str(webdriver_port)],
                stdout=driver_log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            try:
                _wait_for_webdriver(webdriver_port, driver)
                session = _webdriver_request(
                    webdriver_port,
                    "POST",
                    "/session",
                    {
                        "capabilities": {
                            "alwaysMatch": {
                                "browserName": "firefox",
                                "acceptInsecureCerts": True,
                                "moz:firefoxOptions": {
                                    "binary": str(_FIREFOX),
                                    "args": ["-headless"],
                                },
                            }
                        }
                    },
                    timeout=30.0,
                )
                if not isinstance(session, dict) or not isinstance(session.get("sessionId"), str):
                    raise RuntimeError(f"WebDriver returned an invalid session: {session}")
                session_id = session["sessionId"]
                capabilities = session.get("capabilities", {})
                browser_name = capabilities.get("browserName")
                if browser_name != "firefox":
                    raise RuntimeError(f"expected Firefox, WebDriver reported {browser_name}")
                base_url = f"http://127.0.0.1:{server.server_port}"
                _webdriver_request(
                    webdriver_port,
                    "POST",
                    f"/session/{session_id}/timeouts",
                    {"script": 30_000, "pageLoad": 10_000},
                )
                _webdriver_request(
                    webdriver_port,
                    "POST",
                    f"/session/{session_id}/url",
                    {"url": base_url},
                )
                evidence = _webdriver_request(
                    webdriver_port,
                    "POST",
                    f"/session/{session_id}/execute/async",
                    {
                        "script": _PROBE_SCRIPT,
                        "args": [
                            [
                                f"{base_url}/down-the-line-gop30.mp4",
                                f"{base_url}/face-on-gop30.mp4",
                            ]
                        ],
                    },
                    timeout=35.0,
                )
                missing_ranges = {
                    name
                    for name in ("down-the-line-gop30.mp4", "face-on-gop30.mp4")
                    if _FixtureHandler.range_requests.get(name, 0) == 0
                }
                errors: list[str] = []
                if not isinstance(evidence, dict) or evidence.get("ok") is not True:
                    errors.append(f"Firefox AVC exact-frame probe failed: {evidence}")
                if not errors and missing_ranges:
                    errors.append(
                        f"Firefox issued no HTTP Range request for {sorted(missing_ranges)}"
                    )
                report = {
                    "schema_version": 1,
                    "passed": not errors,
                    "errors": errors,
                    "browser_name": browser_name,
                    "browser_version": capabilities.get("browserVersion"),
                    "platform_name": capabilities.get("platformName"),
                    "geckodriver_version": subprocess.run(
                        [str(_GECKODRIVER), "--version"],
                        check=True,
                        capture_output=True,
                        text=True,
                    ).stdout.splitlines()[0],
                    "range_requests": dict(sorted(_FixtureHandler.range_requests.items())),
                    "probe": evidence,
                }
                output_directory = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
                if output_directory is not None:
                    output_path = Path(output_directory) / "firefox-h264-exact-frame.json"
                    output_path.write_text(f"{json.dumps(report, indent=2)}\n", encoding="utf-8")
                print(json.dumps(report, indent=2))
                if errors:
                    raise RuntimeError("; ".join(errors))
            finally:
                if session_id is not None:
                    with contextlib.suppress(Exception):
                        _webdriver_request(
                            webdriver_port,
                            "DELETE",
                            f"/session/{session_id}",
                            timeout=5.0,
                        )
                driver.terminate()
                with contextlib.suppress(subprocess.TimeoutExpired):
                    driver.wait(timeout=5.0)
                if driver.poll() is None:
                    driver.kill()
                    driver.wait(timeout=5.0)
                if driver.returncode not in (0, -15):
                    driver_log.flush()
                    raise RuntimeError(driver_log_path.read_text(encoding="utf-8"))
    server.shutdown()
    server.server_close()
    server_thread.join(timeout=5.0)


if __name__ == "__main__":
    main()
