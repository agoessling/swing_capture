"""Bounded host-side telemetry collection for one paired hitting session."""

from __future__ import annotations

import dataclasses
import datetime
import hashlib
import ipaddress
import json
import os
import re
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
from collections.abc import Mapping
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Protocol, cast, final

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from email.message import Message
    from http.client import HTTPResponse


PACKAGE_NAME = "com.agoessling.swingcapture"
PAIR_NODE_COUNT = 2
DEFAULT_INTERVAL_SECONDS = 1.0
DEFAULT_MAXIMUM_LOGCAT_BYTES = 8 * 1024 * 1024
DEFAULT_MAXIMUM_ARTIFACT_BYTES = 256 * 1024 * 1024
MAXIMUM_DURATION_SECONDS = 12 * 60 * 60
MAXIMUM_INTERVAL_SECONDS = 60.0
MINIMUM_INTERVAL_SECONDS = 0.25
MAXIMUM_LOGCAT_BYTES = 64 * 1024 * 1024
MINIMUM_ARTIFACT_BYTES = 16 * 1024 * 1024
MAXIMUM_ARTIFACT_BYTES = 4 * 1024 * 1024 * 1024
MAXIMUM_HTTP_BODY_BYTES = 1024 * 1024
MAXIMUM_COMMAND_BYTES = 256 * 1024
CONTROL_RESERVE_BYTES = 8 * 1024 * 1024
HTTP_TIMEOUT_SECONDS = 4.0
COMMAND_TIMEOUT_SECONDS = 5.0
MAXIMUM_NETWORK_PORT = 65_535
HTTP_OK = 200
TOKEN_PATTERN = re.compile(r"^[A-Za-z0-9_-]{32}$")
TOKEN_XML_PATTERN = re.compile(r'<string name="control_token">([^<]+)</string>')
SERIAL_PATTERN = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
BEARER_PATTERN = re.compile(r"(?i)(\bbearer\s+)[A-Za-z0-9._~+/=-]+")
MEDIA_ACCESS_PATTERN = re.compile(r"(?i)(media_access=)[A-Za-z0-9_-]+")
STATUS_ENDPOINTS = (
    ("capture_status", "/api/v1/capture/status"),
    ("field_recording_status", "/api/v1/field-recording/status"),
)
DIAGNOSTIC_COMMANDS = (
    ("power", ("shell", "dumpsys", "power")),
    ("battery", ("shell", "dumpsys", "battery")),
    ("thermal", ("shell", "dumpsys", "thermalservice")),
    ("wifi", ("shell", "cmd", "wifi", "status")),
)


class SidecarError(RuntimeError):
    """Raised when a session cannot be collected without violating its contract."""


class ArtifactLimitError(SidecarError):
    """Raised when another data artifact would exceed the configured byte bound."""


@dataclasses.dataclass(frozen=True)
class Node:
    """One ADB serial bound to one credential-free direct application origin."""

    serial: str
    origin: str


@dataclasses.dataclass(frozen=True)
class Configuration:
    """Validated finite collection policy."""

    adb: Path
    nodes: tuple[Node, Node]
    output_directory: Path
    duration_seconds: float
    interval_seconds: float = DEFAULT_INTERVAL_SECONDS
    maximum_logcat_bytes: int = DEFAULT_MAXIMUM_LOGCAT_BYTES
    maximum_artifact_bytes: int = DEFAULT_MAXIMUM_ARTIFACT_BYTES

    def __post_init__(self) -> None:
        """Reject non-pair or unbounded collection policies."""
        if len(self.nodes) != PAIR_NODE_COUNT:
            message = "session sidecar requires exactly two nodes"
            raise ValueError(message)
        if len({node.serial for node in self.nodes}) != PAIR_NODE_COUNT:
            message = "session sidecar node serials must be distinct"
            raise ValueError(message)
        if len({node.origin for node in self.nodes}) != PAIR_NODE_COUNT:
            message = "session sidecar node origins must be distinct"
            raise ValueError(message)
        if not 0 < self.duration_seconds <= MAXIMUM_DURATION_SECONDS:
            message = "duration must be within (0, 43200] seconds"
            raise ValueError(message)
        if not MINIMUM_INTERVAL_SECONDS <= self.interval_seconds <= MAXIMUM_INTERVAL_SECONDS:
            message = "interval must be within [0.25, 60] seconds"
            raise ValueError(message)
        if not 1 <= self.maximum_logcat_bytes <= MAXIMUM_LOGCAT_BYTES:
            message = "maximum logcat bytes must be within [1, 67108864]"
            raise ValueError(message)
        if not MINIMUM_ARTIFACT_BYTES <= self.maximum_artifact_bytes <= MAXIMUM_ARTIFACT_BYTES:
            message = "maximum artifact bytes must be within [16777216, 4294967296]"
            raise ValueError(message)


@dataclasses.dataclass(frozen=True)
class CommandResult:
    """Bounded result from one fixed read-only ADB command."""

    returncode: int
    stdout: bytes = b""
    stderr: bytes = b""
    truncated: bool = False


@dataclasses.dataclass(frozen=True)
class HttpResult:
    """Bounded result from one authenticated GET request."""

    status: int | None
    body: bytes = b""
    error: str = ""
    truncated: bool = False


@dataclasses.dataclass(frozen=True)
class LogcatResult:
    """Final bounded logcat bytes and termination metadata."""

    contents: bytes
    truncated_prefix: bool
    error: str = ""


class LogcatHandle(Protocol):
    """Replaceable streaming logcat boundary."""

    def stop(self) -> LogcatResult:
        """Stop collection and return its bounded retained tail."""
        ...


class Backend(Protocol):
    """Replaceable ADB/HTTP boundary used by hermetic tests."""

    def adb(self, serial: str, arguments: Sequence[str], maximum_bytes: int) -> CommandResult:
        """Run one fixed read-only ADB operation."""
        ...

    def http_get(self, origin: str, path: str, token: str) -> HttpResult:
        """GET one fixed status endpoint without redirects."""
        ...

    def start_logcat(self, serial: str, maximum_bytes: int) -> LogcatHandle:
        """Start one bounded logcat stream."""
        ...


def parse_node(value: str) -> Node:
    """Parse SERIAL=http://HOST:PORT without accepting embedded credentials."""
    serial, separator, origin = value.partition("=")
    if not separator or SERIAL_PATTERN.fullmatch(serial) is None:
        message = "--node must use SERIAL=http://HOST:PORT"
        raise ValueError(message)
    parsed = urllib.parse.urlsplit(origin)
    try:
        port = parsed.port
    except ValueError as failure:
        message = "node origin contains an invalid port"
        raise ValueError(message) from failure
    if (
        parsed.scheme != "http"
        or parsed.hostname is None
        or port is None
        or not 1 <= port <= MAXIMUM_NETWORK_PORT
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
    ):
        message = "node origin must be an uncredentialed HTTP origin with an explicit port"
        raise ValueError(message)
    host = parsed.hostname
    if host.lower() == "localhost":
        message = "node origin must not be loopback or unspecified"
        raise ValueError(message)
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        pass
    else:
        if address.is_loopback or address.is_unspecified:
            message = "node origin must not be loopback or unspecified"
            raise ValueError(message)
    normalized = f"http://{parsed.netloc}"
    return Node(serial=serial, origin=normalized)


def parse_control_token(preferences: bytes) -> str:
    """Extract the app-private control token without retaining the preferences."""
    try:
        decoded = preferences.decode("utf-8", errors="strict")
    except UnicodeDecodeError as failure:
        message = "node preferences are not valid UTF-8"
        raise SidecarError(message) from failure
    match = TOKEN_XML_PATTERN.search(decoded)
    if match is None or TOKEN_PATTERN.fullmatch(match.group(1)) is None:
        message = "node preferences do not contain a valid control credential"
        raise SidecarError(message)
    return match.group(1)


def _utc_now() -> datetime.datetime:
    return datetime.datetime.now(datetime.UTC)


def _format_utc(value: datetime.datetime) -> str:
    return value.astimezone(datetime.UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _canonical_json(value: object) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode("utf-8")


def _sha256(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def _redact_text(value: str, tokens: Sequence[str]) -> str:
    result = value
    for token in tokens:
        result = result.replace(token, "[REDACTED_CONTROL_TOKEN]")
    result = BEARER_PATTERN.sub(r"\1[REDACTED]", result)
    return MEDIA_ACCESS_PATTERN.sub(r"\1[REDACTED]", result)


def _safe_response_body(body: bytes, tokens: Sequence[str]) -> tuple[object | None, str]:
    """Return the exact JSON value, omitting malformed or credential-bearing bodies."""
    if any(token.encode("ascii") in body for token in tokens):
        return None, "response omitted because it contained the control credential"
    if re.search(rb"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]+", body) is not None:
        return None, "response omitted because it contained bearer credential material"
    if re.search(rb"(?i)media_access=[A-Za-z0-9_-]+", body) is not None:
        return None, "response omitted because it contained a media capability"
    try:
        decoded = body.decode("utf-8", errors="strict")
        value = cast("object", json.loads(decoded))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None, "response body was not valid JSON"
    if not isinstance(value, Mapping):
        return None, "response JSON was not an object"
    return cast("Mapping[str, object]", value), ""


@final
class _ArtifactWriter:
    """Finite artifact writer with a reserved budget for the report and inventory."""

    def __init__(self, root: Path, maximum_bytes: int) -> None:
        if root.exists():
            message = f"output directory already exists: {root}"
            raise SidecarError(message)
        root.mkdir(parents=True)
        self.root = root
        self.maximum_bytes = maximum_bytes
        self.data_limit = maximum_bytes - CONTROL_RESERVE_BYTES
        self.bytes_written = 0

    def _target(self, relative: str) -> Path:
        pure = PurePosixPath(relative)
        if pure.is_absolute() or ".." in pure.parts or not pure.parts:
            message = "artifact path is unsafe"
            raise SidecarError(message)
        target = self.root.joinpath(*pure.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        return target

    def write_data(self, relative: str, contents: bytes) -> None:
        if self.bytes_written + len(contents) > self.data_limit:
            message = "configured artifact data limit reached"
            raise ArtifactLimitError(message)
        self._write(relative, contents)

    def write_control(self, relative: str, contents: bytes) -> None:
        if self.bytes_written + len(contents) > self.maximum_bytes:
            message = "configured total artifact limit cannot hold control metadata"
            raise SidecarError(message)
        self._write(relative, contents)

    def _write(self, relative: str, contents: bytes) -> None:
        target = self._target(relative)
        temporary = target.with_name(f".{target.name}.tmp")
        with temporary.open("wb") as output:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(target)
        self.bytes_written += len(contents)

    def append_data(self, relative: str, contents: bytes) -> None:
        if self.bytes_written + len(contents) > self.data_limit:
            message = "configured artifact data limit reached"
            raise ArtifactLimitError(message)
        target = self._target(relative)
        with target.open("ab") as output:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        self.bytes_written += len(contents)


@final
class _BoundedTail:
    """Thread-safe byte tail retaining the newest logcat output."""

    def __init__(self, maximum_bytes: int) -> None:
        self.maximum_bytes = maximum_bytes
        self.chunks: deque[bytes] = deque()
        self.total = 0
        self.truncated = False
        self.lock = threading.Lock()

    def append(self, value: bytes) -> None:
        if not value:
            return
        with self.lock:
            self.chunks.append(value)
            self.total += len(value)
            while self.total > self.maximum_bytes and self.chunks:
                excess = self.total - self.maximum_bytes
                first = self.chunks.popleft()
                if len(first) > excess:
                    self.chunks.appendleft(first[excess:])
                    self.total -= excess
                else:
                    self.total -= len(first)
                self.truncated = True

    def result(self) -> tuple[bytes, bool]:
        with self.lock:
            return b"".join(self.chunks), self.truncated


@final
class _SubprocessLogcat:
    def __init__(self, process: subprocess.Popen[bytes], maximum_bytes: int) -> None:
        self.process = process
        self.tail = _BoundedTail(maximum_bytes)
        self.error = ""
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        try:
            if self.process.stdout is None:
                self.error = "logcat stdout pipe was unavailable"
                return
            while True:
                chunk = cast("bytes", self.process.stdout.read(64 * 1024))
                if not chunk:
                    return
                self.tail.append(chunk)
        except OSError as failure:
            self.error = f"logcat read failed: {failure}"

    def stop(self) -> LogcatResult:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.thread.join(timeout=2)
        contents, truncated = self.tail.result()
        return LogcatResult(contents=contents, truncated_prefix=truncated, error=self.error)


@final
class ProductionBackend:
    """Production implementation containing only fixed read-only operations."""

    def __init__(self, adb: Path) -> None:
        """Bind production operations to the Bazel-provided adb executable."""
        self.adb_path = adb

    def adb(self, serial: str, arguments: Sequence[str], maximum_bytes: int) -> CommandResult:
        """Run one bounded read-only ADB command for the configured serial."""
        try:
            completed = subprocess.run(
                (str(self.adb_path), "-s", serial, *arguments),
                check=False,
                capture_output=True,
                timeout=COMMAND_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.TimeoutExpired) as failure:
            return CommandResult(returncode=124, stderr=str(failure).encode("utf-8"))
        stdout, stdout_truncated = _bounded_prefix(completed.stdout, maximum_bytes)
        stderr, stderr_truncated = _bounded_prefix(completed.stderr, maximum_bytes)
        return CommandResult(
            returncode=completed.returncode,
            stdout=stdout,
            stderr=stderr,
            truncated=stdout_truncated or stderr_truncated,
        )

    def http_get(self, origin: str, path: str, token: str) -> HttpResult:
        """Read one bounded JSON endpoint without following redirects."""
        request = urllib.request.Request(  # noqa: S310 -- parse_node restricts origins to HTTP.
            f"{origin}{path}",
            method="GET",
            headers={"Accept": "application/json", "Authorization": f"Bearer {token}"},
        )
        opener = urllib.request.build_opener(_RejectRedirectHandler())
        try:
            response = cast("HTTPResponse", opener.open(request, timeout=HTTP_TIMEOUT_SECONDS))
            with response:
                body = response.read(MAXIMUM_HTTP_BODY_BYTES + 1)
                bounded, truncated = _bounded_prefix(body, MAXIMUM_HTTP_BODY_BYTES)
                return HttpResult(status=response.status, body=bounded, truncated=truncated)
        except urllib.error.HTTPError as failure:
            body = failure.read(MAXIMUM_HTTP_BODY_BYTES + 1)
            bounded, truncated = _bounded_prefix(body, MAXIMUM_HTTP_BODY_BYTES)
            return HttpResult(status=failure.code, body=bounded, truncated=truncated)
        except (OSError, ValueError) as failure:
            return HttpResult(status=None, error=str(failure))

    def start_logcat(self, serial: str, maximum_bytes: int) -> LogcatHandle:
        """Start a bounded-tail logcat stream for one configured serial."""
        process = subprocess.Popen(
            (
                str(self.adb_path),
                "-s",
                serial,
                "logcat",
                "-v",
                "threadtime",
                "-T",
                "1",
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        return _SubprocessLogcat(process, maximum_bytes)


@final
class _RejectRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Never forward a bearer credential away from the configured origin."""

    def redirect_request(  # pyright: ignore[reportImplicitOverride]  # noqa: PLR0913
        self,
        req: urllib.request.Request,
        fp: object,
        code: int,
        msg: str,
        headers: Message,
        newurl: str,
    ) -> None:
        del req, fp, code, msg, headers, newurl


def _bounded_prefix(value: bytes, maximum_bytes: int) -> tuple[bytes, bool]:
    return value[:maximum_bytes], len(value) > maximum_bytes


def _load_token(backend: Backend, node: Node) -> str:
    result = backend.adb(
        node.serial,
        (
            "exec-out",
            "run-as",
            PACKAGE_NAME,
            "cat",
            "shared_prefs/node_configuration.xml",
        ),
        64 * 1024,
    )
    if result.returncode != 0 or result.truncated:
        message = f"unable to read app-private control credential for {node.serial}"
        raise SidecarError(message)
    return parse_control_token(result.stdout)


def _diagnostics(
    backend: Backend, nodes: Sequence[Node], tokens: Sequence[str], now: datetime.datetime
) -> dict[str, object]:
    result: dict[str, object] = {"captured_at_utc": _format_utc(now), "nodes": []}
    encoded_nodes: list[dict[str, object]] = []
    for node in nodes:
        commands: dict[str, object] = {}
        for name, arguments in DIAGNOSTIC_COMMANDS:
            command = backend.adb(node.serial, arguments, MAXIMUM_COMMAND_BYTES)
            commands[name] = {
                "returncode": command.returncode,
                "stdout": _redact_text(command.stdout.decode("utf-8", errors="replace"), tokens),
                "stderr": _redact_text(command.stderr.decode("utf-8", errors="replace"), tokens),
                "truncated": command.truncated,
            }
        encoded_nodes.append({"serial": node.serial, "origin": node.origin, "commands": commands})
    result["nodes"] = encoded_nodes
    return result


def _endpoint_record(result: HttpResult, tokens: Sequence[str]) -> tuple[dict[str, object], int]:
    body, body_error = _safe_response_body(result.body, tokens)
    errors = [item for item in (result.error, body_error) if item]
    record: dict[str, object] = {
        "http_status": result.status,
        "body": body,
        "body_bytes": len(result.body),
        "body_sha256": _sha256(result.body),
        "truncated": result.truncated,
        "error": _redact_text("; ".join(errors), tokens),
    }
    failed = int(result.status != HTTP_OK or body is None or result.truncated)
    return record, failed


def _snapshot(  # noqa: PLR0913 -- explicit timestamp inputs make evidence deterministic.
    backend: Backend,
    nodes: Sequence[Node],
    tokens: Mapping[str, str],
    sequence: int,
    now: datetime.datetime,
    monotonic_ns: int,
) -> tuple[dict[str, object], int]:
    requests = [(node, name, path) for node in nodes for name, path in STATUS_ENDPOINTS]
    with ThreadPoolExecutor(max_workers=len(requests)) as executor:
        futures = {
            (node.serial, name): executor.submit(
                backend.http_get, node.origin, path, tokens[node.serial]
            )
            for node, name, path in requests
        }
        results: dict[tuple[str, str], HttpResult] = {}
        for key, future in futures.items():
            try:
                results[key] = future.result()
            except Exception as failure:  # noqa: BLE001, PERF203
                # Third-party/fake backend exceptions are evidence, not authority to stop phones.
                results[key] = HttpResult(status=None, error=str(failure))
    failures = 0
    encoded_nodes: list[dict[str, object]] = []
    token_values = tuple(tokens.values())
    for node in nodes:
        endpoints: dict[str, object] = {}
        for name, _ in STATUS_ENDPOINTS:
            record, failed = _endpoint_record(results[(node.serial, name)], token_values)
            endpoints[name] = record
            failures += failed
        encoded_nodes.append({"serial": node.serial, "origin": node.origin, **endpoints})
    return (
        {
            "schema_version": 1,
            "sequence": sequence,
            "captured_at_utc": _format_utc(now),
            "host_monotonic_ns": str(monotonic_ns),
            "nodes": encoded_nodes,
        },
        failures,
    )


def _token_free_inventory(root: Path, tokens: Sequence[str]) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        if path.name.startswith(".") and path.name.endswith(".tmp"):
            continue
        contents = path.read_bytes()
        if any(token.encode("ascii") in contents for token in tokens):
            message = f"credential detected in artifact {path.relative_to(root)}"
            raise SidecarError(message)
        if re.search(rb"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]+", contents) is not None:
            message = f"bearer credential material detected in {path.relative_to(root)}"
            raise SidecarError(message)
        if re.search(rb"(?i)media_access=[A-Za-z0-9_-]+", contents) is not None:
            message = f"media capability detected in {path.relative_to(root)}"
            raise SidecarError(message)
        entries.append(
            {
                "path": path.relative_to(root).as_posix(),
                "bytes": len(contents),
                "sha256": _sha256(contents),
            }
        )
    return entries


def collect(  # noqa: C901, PLR0912, PLR0915 -- one finally block owns evidence finalization.
    configuration: Configuration,
    *,
    backend: Backend | None = None,
    utc_now: Callable[[], datetime.datetime] = _utc_now,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, object]:
    """Collect finite read-only evidence and always finalize a token-free inventory."""
    selected_backend = backend or ProductionBackend(configuration.adb)
    writer = _ArtifactWriter(configuration.output_directory, configuration.maximum_artifact_bytes)
    started_at = utc_now()
    started_monotonic = monotonic()
    tokens: dict[str, str] = {}
    logcats: dict[str, LogcatHandle] = {}
    snapshot_count = 0
    endpoint_failure_count = 0
    artifact_limit_reached = False
    interrupted = False
    fatal_error = ""
    logcat_reports: list[dict[str, object]] = []
    try:
        for node in configuration.nodes:
            tokens[node.serial] = _load_token(selected_backend, node)
        for node in configuration.nodes:
            logcats[node.serial] = selected_backend.start_logcat(
                node.serial, configuration.maximum_logcat_bytes
            )
        writer.write_data(
            "diagnostics-initial.json",
            _canonical_json(
                _diagnostics(
                    selected_backend, configuration.nodes, tuple(tokens.values()), utc_now()
                )
            ),
        )
        deadline = started_monotonic + configuration.duration_seconds
        next_due = started_monotonic
        while monotonic() < deadline or snapshot_count == 0:
            snapshot, failures = _snapshot(
                selected_backend,
                configuration.nodes,
                tokens,
                snapshot_count,
                utc_now(),
                int(monotonic() * 1_000_000_000),
            )
            writer.append_data("status-snapshots.jsonl", _canonical_json(snapshot))
            snapshot_count += 1
            endpoint_failure_count += failures
            next_due += configuration.interval_seconds
            remaining = min(next_due, deadline) - monotonic()
            if remaining <= 0:
                if monotonic() >= deadline:
                    break
                continue
            sleep(remaining)
    except KeyboardInterrupt:
        interrupted = True
    except ArtifactLimitError as failure:
        artifact_limit_reached = True
        fatal_error = str(failure)
    except (OSError, SidecarError, ValueError) as failure:
        fatal_error = _redact_text(str(failure), tuple(tokens.values()))
    finally:
        if tokens:
            try:
                writer.write_data(
                    "diagnostics-final.json",
                    _canonical_json(
                        _diagnostics(
                            selected_backend, configuration.nodes, tuple(tokens.values()), utc_now()
                        )
                    ),
                )
            except (ArtifactLimitError, OSError) as failure:
                artifact_limit_reached = True
                fatal_error = fatal_error or str(failure)
        for index, node in enumerate(configuration.nodes, start=1):
            handle = logcats.get(node.serial)
            if handle is None:
                continue
            try:
                logcat = handle.stop()
                decoded = logcat.contents.decode("utf-8", errors="replace")
                retained = _redact_text(decoded, tuple(tokens.values())).encode("utf-8")
                writer.write_data(f"logcat-node-{index}.txt", retained)
                logcat_reports.append(
                    {
                        "serial": node.serial,
                        "path": f"logcat-node-{index}.txt",
                        "bytes": len(retained),
                        "truncated_prefix": logcat.truncated_prefix,
                        "error": _redact_text(logcat.error, tuple(tokens.values())),
                    }
                )
            except (ArtifactLimitError, OSError) as failure:
                artifact_limit_reached = True
                fatal_error = fatal_error or str(failure)
    completed_at = utc_now()
    report: dict[str, object] = {
        "schema_version": 1,
        "report_type": "paired_hitting_session_sidecar",
        "passed": not fatal_error and snapshot_count > 0,
        "read_only": True,
        "started_at_utc": _format_utc(started_at),
        "completed_at_utc": _format_utc(completed_at),
        "configured_duration_seconds": configuration.duration_seconds,
        "configured_interval_seconds": configuration.interval_seconds,
        "maximum_logcat_bytes_per_node": configuration.maximum_logcat_bytes,
        "maximum_artifact_bytes": configuration.maximum_artifact_bytes,
        "nodes": [dataclasses.asdict(node) for node in configuration.nodes],
        "snapshot_count": snapshot_count,
        "endpoint_failure_count": endpoint_failure_count,
        "artifact_limit_reached": artifact_limit_reached,
        "interrupted": interrupted,
        "fatal_error": _redact_text(fatal_error, tuple(tokens.values())),
        "credentials": {
            "source": "adb_run_as_app_private_preferences",
            "accepted_in_arguments": False,
            "retained_in_artifacts": False,
        },
        "logcat": logcat_reports,
        "prohibited_actions_performed": [],
        "limitations": [
            "The sidecar does not arm, stop, configure, delete, or export phone media.",
            "Status snapshots and Android diagnostics are observations, not operator ground truth.",
            "Logcat is a bounded tail and may omit early lines after its byte limit is reached.",
            "Media and diagnostic ZIPs remain separate operator-managed evidence.",
        ],
    }
    writer.write_control("report.json", _canonical_json(report))
    inventory_entries = _token_free_inventory(
        configuration.output_directory, tuple(tokens.values())
    )
    inventory = {
        "schema_version": 1,
        "report_type": "paired_hitting_session_sidecar_inventory",
        "token_free": True,
        "inventory_self_excluded": True,
        "files": inventory_entries,
    }
    writer.write_control("inventory.json", _canonical_json(inventory))
    return report
