"""Read-only preflight checks for one autonomous Android capture pair."""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import ipaddress
import json
import math
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Mapping
from pathlib import Path
from typing import TYPE_CHECKING, cast, final

from tools.android_apk_installer import parse_package_path, parse_sha256, sha256_file

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from email.message import Message
    from http.client import HTTPResponse


PACKAGE_NAME = "com.agoessling.swingcapture"
TOKEN_PATTERN = re.compile(r"^[A-Za-z0-9_-]{32}$")
TOKEN_XML_PATTERN = re.compile(r'<string name="control_token">([^<]+)</string>')
CREDENTIAL_TOKEN_IN_TEXT_PATTERN = re.compile(
    r"(?<![A-Za-z0-9_-])[A-Za-z0-9_-]{32}(?![A-Za-z0-9_-])"
)
BEARER_CREDENTIAL_PATTERN = re.compile(r"(?i)(\bbearer\s+)[A-Za-z0-9._~+/=-]+")
BATTERY_FIELD_PATTERN = re.compile(r"^\s*(level|scale|temperature|voltage):\s*(-?\d+)\s*$")
THERMAL_STATUS_PATTERN = re.compile(r"Thermal [Ss]tatus:\s*(\d+)")
BSSID_PREFIX_PATTERN = r"(?im)^\s*(?:m)?WifiInfo:.*?\bBSSID:\s*"
BSSID_VALUE_PATTERN = r"(?P<bssid>(?:[0-9a-f]{2}:){5}[0-9a-f]{2})\b"
BSSID_PATTERN = re.compile(BSSID_PREFIX_PATTERN + BSSID_VALUE_PATTERN)
MINIMUM_STORAGE_BYTES = 2 * 1024 * 1024 * 1024
MINIMUM_BATTERY_PERCENT = 30.0
MAXIMUM_THERMAL_STATUS = 2
MAXIMUM_BATTERY_PERCENT = 100.0
MINIMUM_BATTERY_TEMPERATURE_CELSIUS = -20.0
MAXIMUM_BATTERY_TEMPERATURE_CELSIUS = 100.0
MAXIMUM_ANDROID_THERMAL_STATUS = 6
MAXIMUM_POSE_INFERENCE_P95_NS = 200_000_000
MAXIMUM_POSE_INFERENCE_OUTLIER_NS = 400_000_000
MINIMUM_RECENT_POSE_INFERENCE_SAMPLES = 100
MAXIMUM_RECENT_POSE_INFERENCE_SAMPLES = 150
PRODUCTION_POSE_STANDBY_WIDTH = 640
PRODUCTION_POSE_STANDBY_HEIGHT = 360
MINIMUM_JITTER_SAMPLE_COUNT = 2
MAXIMUM_WEB_ASSET_BYTES = 2 * 1024 * 1024
HTTP_TIMEOUT_SECONDS = 5
PAIR_NODE_COUNT = 2
HTTP_OK = 200
HTTP_NO_CONTENT = 204
HTTP_UNAUTHORIZED = 401
MINIMUM_ANDROID_API_LEVEL = 34
MAXIMUM_PEER_CLOCK_UNCERTAINTY_NS = 25_000_000
MAXIMUM_PEER_CLOCK_AGE_NS = 10_000_000_000
IPV6_VERSION = 6
_INVALID_ORIGIN_PORT = "direct-LAN origin has an invalid port"
_INVALID_ORIGIN = "direct-LAN origin must be an uncredentialed HTTP origin"
_LOOPBACK_HOST = "loopback origin could be an ADB USB forward"
_LOOPBACK_ADDRESS = "loopback or unspecified origin is not direct LAN"
_MAXIMUM_SIGNED_64_BIT_INTEGER = 2**63 - 1
_STOPPED_CAPTURE_STATES = frozenset(("ready", "setup", "stopped"))
_FIELD_RECORDING_STATES = frozenset(("idle", "starting", "recording", "stopping", "ready", "error"))
_TERMINAL_FIELD_RECORDING_STATES = frozenset(("idle", "ready", "error"))
_CLEAN_FIELD_RECORDING_STATES = frozenset(("idle", "ready"))
_FIELD_RECORDING_STATUS_FIELDS = frozenset(
    (
        "schema_version",
        "state",
        "active_recording_id",
        "shared_recording_id",
        "started_at_utc",
        "started_elapsed_realtime_ns",
        "elapsed_ms",
        "video_bytes",
        "audio_frames",
        "max_duration_seconds",
        "error",
        "hil_reject_next_start",
        "hil_fail_next_accepted_start",
        "hil_accepted_start_waiting",
    )
)
_SENSITIVE_STATUS_KEY_FRAGMENTS = (
    "authorization",
    "capability",
    "credential",
    "password",
    "secret",
    "token",
)
_STATUS_DIAGNOSTIC_FIELDS = (
    "schema_version",
    "state",
    "armed",
    "active_session_id",
    "error",
    "video_frames",
    "audio_frames",
    "ring_bytes",
    "ring_duration_us",
    "shared_session_id",
    "server_elapsed_realtime_ns",
    "last_trigger_elapsed_realtime_ns",
    "pose",
    "operational_health",
    "pair_network_health",
    "device_admission",
    "reboot_recovery",
    "live_status",
)


def _message(*parts: str) -> str:
    """Join diagnostic fragments without relying on implicit literal concatenation."""
    return "".join(parts)


def _credential_free_status_value(value: object) -> object:
    """Recursively retain JSON telemetry while redacting credential-shaped values."""
    if isinstance(value, str):
        redacted = BEARER_CREDENTIAL_PATTERN.sub(r"\1[REDACTED]", value)
        return CREDENTIAL_TOKEN_IN_TEXT_PATTERN.sub("[REDACTED]", redacted)
    if isinstance(value, list):
        return [_credential_free_status_value(item) for item in cast("list[object]", value)]
    if isinstance(value, Mapping):
        mapping = cast("Mapping[object, object]", value)
        retained: dict[str, object] = {}
        for raw_key, item in mapping.items():
            key = str(raw_key)
            if any(fragment in key.casefold() for fragment in _SENSITIVE_STATUS_KEY_FRAGMENTS):
                retained[key] = "[REDACTED]"
            else:
                retained[key] = _credential_free_status_value(item)
        return retained
    return value


def capture_status_diagnostics(status: Mapping[str, object]) -> dict[str, object]:
    """Return the rich, credential-free status subset retained in every doctor attempt."""
    return {
        field: _credential_free_status_value(status[field])
        for field in _STATUS_DIAGNOSTIC_FIELDS
        if field in status
    }


def field_recording_status_diagnostics(status: Mapping[str, object]) -> dict[str, object]:
    """Retain the full credential-free field-recorder status response."""
    return cast("dict[str, object]", _credential_free_status_value(status))


@final
class _RejectRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Keep bearer credentials on the explicitly configured phone origin."""

    # urllib fixes this callback signature; returning implicitly rejects the redirect.
    def redirect_request(  # pyright: ignore[reportIncompatibleMethodOverride, reportImplicitOverride]  # noqa: PLR0913
        self,
        request: urllib.request.Request,
        file_pointer: object,
        code: int,
        message: str,
        headers: Message,
        new_url: str,
    ) -> None:
        del request, file_pointer, code, message, headers, new_url


@dataclasses.dataclass(frozen=True)
class Check:
    """One named preflight assertion."""

    name: str
    passed: bool
    message: str

    def as_json(self) -> dict[str, object]:
        """Return a stable machine-readable representation."""
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class DeviceTelemetry:
    """Small read-only telemetry snapshot parsed from Android system services."""

    battery_percent: float
    battery_temperature_celsius: float
    battery_voltage_millivolts: int
    thermal_status: int
    storage_usable_bytes: int


@dataclasses.dataclass(frozen=True)
class HostedWebAsset:
    """Bounded evidence that one browser asset is served directly by the phone."""

    path: str
    status: int
    direct_response: bool
    content_type: str
    cache_control: str
    nosniff: bool
    body_bytes: int
    required_markers_present: bool


@dataclasses.dataclass(frozen=True)
class BrowserCorsEvidence:
    """Response policy needed by a browser controlling the other phone's origin."""

    status: int
    direct_response: bool
    allow_origin: str
    allow_methods: str
    allow_headers: str


@dataclasses.dataclass(frozen=True)
class ReachabilityEvidence:
    """One directed direct-LAN probe, independent of the ADB transport."""

    source: str
    target: str
    transport: str
    reachable: bool
    http_status: int | None = None

    def as_json(self) -> dict[str, object]:
        """Return stable matrix evidence without retaining command output."""
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class LanNodeDiagnostics:
    """Wi-Fi association and required directed paths for one phone."""

    serial: str
    origin: str
    bssid: str | None
    host_to_phone: ReachabilityEvidence
    phone_to_peer: ReachabilityEvidence


@dataclasses.dataclass(frozen=True)
class NodeEvidence:
    """Evidence collected from one phone without retaining its credential."""

    serial: str
    origin: str
    model: str
    adb_transport: str
    installed_apk_sha256: str
    descriptor: Mapping[str, object]
    setup: Mapping[str, object]
    status: Mapping[str, object]
    field_recording_status: Mapping[str, object]
    clock: Mapping[str, object]
    unauthenticated_setup_status: int
    web_assets: tuple[HostedWebAsset, ...]
    browser_cors: BrowserCorsEvidence
    telemetry: DeviceTelemetry


def parse_control_token(preferences: str) -> str:
    """Extract a valid token from app-private preferences without logging it."""
    match = TOKEN_XML_PATTERN.search(preferences)
    if match is None or TOKEN_PATTERN.fullmatch(match.group(1)) is None:
        msg = "Android node preferences do not contain a valid control credential"
        raise ValueError(msg)
    return match.group(1)


def parse_telemetry(battery: str, thermal: str, storage: str) -> DeviceTelemetry:
    """Parse bounded battery, thermal, and storage evidence from adb output."""
    fields: dict[str, int] = {}
    for line in battery.splitlines():
        match = BATTERY_FIELD_PATTERN.fullmatch(line)
        if match is not None:
            fields[match.group(1)] = int(match.group(2))
    thermal_match = THERMAL_STATUS_PATTERN.search(thermal)
    storage_lines = [line.strip() for line in storage.splitlines() if line.strip()]
    if (
        not {"level", "scale", "temperature", "voltage"}.issubset(fields)
        or fields["scale"] <= 0
        or thermal_match is None
        or not storage_lines
        or re.fullmatch(r"\d+", storage_lines[-1]) is None
    ):
        msg = "required Android battery, thermal, or storage fields are absent"
        raise ValueError(msg)
    result = DeviceTelemetry(
        battery_percent=100.0 * fields["level"] / fields["scale"],
        battery_temperature_celsius=fields["temperature"] / 10.0,
        battery_voltage_millivolts=fields["voltage"],
        thermal_status=int(thermal_match.group(1)),
        storage_usable_bytes=int(storage_lines[-1]),
    )
    if (
        not 0 <= result.battery_percent <= MAXIMUM_BATTERY_PERCENT
        or not MINIMUM_BATTERY_TEMPERATURE_CELSIUS
        <= result.battery_temperature_celsius
        <= MAXIMUM_BATTERY_TEMPERATURE_CELSIUS
        or result.battery_voltage_millivolts <= 0
        or not 0 <= result.thermal_status <= MAXIMUM_ANDROID_THERMAL_STATUS
        or result.storage_usable_bytes < 0
    ):
        msg = "Android battery, thermal, or storage fields are outside valid bounds"
        raise ValueError(msg)
    return result


def parse_wifi_bssid(status: str) -> str:
    """Extract one current infrastructure BSSID from Android's Wi-Fi status."""
    matches = {match.group("bssid").lower() for match in BSSID_PATTERN.finditer(status)}
    unavailable = {"00:00:00:00:00:00", "02:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"}
    matches.difference_update(unavailable)
    if len(matches) != 1:
        message = "Android Wi-Fi status does not identify exactly one current BSSID"
        raise ValueError(message)
    return next(iter(matches))


def _direct_lan_origin(origin: str) -> tuple[str, int]:
    """Return a non-loopback HTTP host/port or reject a possible host-side forward."""
    parsed = urllib.parse.urlsplit(origin)
    try:
        port = parsed.port
    except ValueError as failure:
        raise ValueError(_INVALID_ORIGIN_PORT) from failure
    host = parsed.hostname
    if (
        parsed.scheme != "http"
        or host is None
        or port is None
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError(_INVALID_ORIGIN)
    if host.casefold() == "localhost":
        raise ValueError(_LOOPBACK_HOST)
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return host, port
    if address.is_loopback or address.is_unspecified:
        raise ValueError(_LOOPBACK_ADDRESS)
    return host, port


def _uses_expected_wifi_route(
    route: str,
    expected_peer: ipaddress.IPv4Address | ipaddress.IPv6Address,
    expected_source: ipaddress.IPv4Address | ipaddress.IPv6Address,
) -> bool:
    """Require one unambiguous wlan0 route from the phone's advertised LAN address."""
    lines = [line.split() for line in route.splitlines() if line.strip()]
    if not lines or not lines[0]:
        return False
    tokens = [token for line in lines for token in line]
    try:
        routed_peer = ipaddress.ip_address(tokens[0])
    except ValueError:
        return False
    if routed_peer != expected_peer or tokens.count("dev") != 1 or tokens.count("src") != 1:
        return False
    device_index = tokens.index("dev")
    source_index = tokens.index("src")
    if (
        device_index + 1 >= len(tokens)
        or source_index + 1 >= len(tokens)
        or tokens[device_index + 1] != "wlan0"
    ):
        return False
    try:
        routed_source = ipaddress.ip_address(tokens[source_index + 1])
    except ValueError:
        return False
    return routed_source == expected_source


def _mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, dict):
        message = f"{label} must be an object"
        raise TypeError(message)
    return cast("Mapping[str, object]", value)


def _array(value: object, label: str) -> Sequence[object]:
    if not isinstance(value, list):
        message = f"{label} must be an array"
        raise TypeError(message)
    return cast("Sequence[object]", value)


def _string(value: object) -> str:
    return value if isinstance(value, str) else ""


def _integer(value: object) -> int | None:
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, str) and re.fullmatch(r"0|[1-9]\d*", value) is not None:
        return int(value)
    return None


def _optional_mapping(value: object) -> Mapping[str, object]:
    return cast("Mapping[str, object]", value) if isinstance(value, Mapping) else {}


@dataclasses.dataclass(frozen=True)
class _PairNetworkHealthAssessment:
    valid: bool
    diagnostic: str
    state: str = ""
    measured: bool = False
    stale: bool = False
    peer_origin: str = ""
    peer_node_id: str = ""
    issues: tuple[str, ...] = ()


def _invalid_pair_network_health(diagnostic: str) -> _PairNetworkHealthAssessment:
    return _PairNetworkHealthAssessment(valid=False, diagnostic=diagnostic)


def _canonical_nonnegative_integer(value: object) -> int | None:
    if not isinstance(value, str) or re.fullmatch(r"0|[1-9]\d*", value) is None:
        return None
    parsed = int(value)
    return parsed if parsed <= _MAXIMUM_SIGNED_64_BIT_INTEGER else None


def _strict_json_integer(value: object) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def _pair_network_direction_valid(value: object) -> bool:  # noqa: PLR0911
    if not isinstance(value, Mapping):
        return False
    value = cast("Mapping[str, object]", value)
    expected_fields = {
        "schema_version",
        "attempts",
        "successes",
        "timeouts",
        "round_trip_ns",
        "transfer_bytes",
        "transfer_duration_ns",
        "transfer_complete",
        "minimum_round_trip_ns",
        "median_round_trip_ns",
        "p95_round_trip_ns",
        "maximum_round_trip_ns",
        "jitter_ns",
        "transfer_bits_per_second",
    }
    if set(value) != expected_fields or _strict_json_integer(value.get("schema_version")) != 1:
        return False
    attempts = _strict_json_integer(value.get("attempts"))
    successes = _strict_json_integer(value.get("successes"))
    timeouts = _strict_json_integer(value.get("timeouts"))
    if (
        attempts is None
        or attempts <= 0
        or successes is None
        or not 0 <= successes <= attempts
        or timeouts is None
        or not 0 <= timeouts <= attempts - successes
    ):
        return False
    round_trips_value = value.get("round_trip_ns")
    if not isinstance(round_trips_value, list):
        return False
    round_trips = [
        _canonical_nonnegative_integer(item) for item in cast("list[object]", round_trips_value)
    ]
    if any(item is None for item in round_trips) or len(round_trips) != successes:
        return False
    parsed_round_trips = cast("list[int]", round_trips)
    if parsed_round_trips != sorted(parsed_round_trips):
        return False
    transfer_bytes = _canonical_nonnegative_integer(value.get("transfer_bytes"))
    transfer_duration = _canonical_nonnegative_integer(value.get("transfer_duration_ns"))
    transfer_complete = value.get("transfer_complete")
    if (
        transfer_bytes is None
        or transfer_duration is None
        or not isinstance(transfer_complete, bool)
        or (transfer_complete and (transfer_bytes == 0 or transfer_duration == 0))
    ):
        return False
    derived = {
        "minimum_round_trip_ns": parsed_round_trips[0] if parsed_round_trips else 0,
        "median_round_trip_ns": (
            parsed_round_trips[(50 * len(parsed_round_trips) + 99) // 100 - 1]
            if parsed_round_trips
            else 0
        ),
        "p95_round_trip_ns": (
            parsed_round_trips[(95 * len(parsed_round_trips) + 99) // 100 - 1]
            if parsed_round_trips
            else 0
        ),
        "maximum_round_trip_ns": parsed_round_trips[-1] if parsed_round_trips else 0,
        "jitter_ns": (
            parsed_round_trips[-1] - parsed_round_trips[0]
            if len(parsed_round_trips) >= MINIMUM_JITTER_SAMPLE_COUNT
            else 0
        ),
    }
    if any(
        _canonical_nonnegative_integer(value.get(field)) != expected
        for field, expected in derived.items()
    ):
        return False
    bits_per_second = value.get("transfer_bits_per_second")
    if (
        not isinstance(bits_per_second, (int, float))
        or isinstance(bits_per_second, bool)
        or not math.isfinite(bits_per_second)
        or bits_per_second < 0
    ):
        return False
    expected_bits_per_second = (
        0.0
        if transfer_duration == 0
        else transfer_bytes * 8.0 * 1_000_000_000.0 / transfer_duration
    )
    return math.isclose(bits_per_second, expected_bits_per_second, rel_tol=1e-12, abs_tol=0.0)


def _pair_network_health_assessment(  # noqa: C901, PLR0911, PLR0912
    value: object, pose_mode: str
) -> _PairNetworkHealthAssessment:
    if pose_mode not in {"leader", "shadow", "disabled"}:
        return _invalid_pair_network_health("pose mode is unsupported for network health")
    if not isinstance(value, Mapping):
        return _invalid_pair_network_health("pair network health is missing or not an object")
    value = cast("Mapping[str, object]", value)
    expected_fields = {
        "schema_version",
        "configured",
        "state",
        "raw_state",
        "measured",
        "stale",
        "transition_pending",
        "age_ns",
        "issues",
        "peer",
        "measured_at_elapsed_realtime_ns",
        "local_to_peer",
        "peer_to_local",
    }
    if set(value) != expected_fields or _strict_json_integer(value.get("schema_version")) != 1:
        return _invalid_pair_network_health("pair network-health fields do not match schema 1")
    configured = value.get("configured")
    measured = value.get("measured")
    stale = value.get("stale")
    transition_pending = value.get("transition_pending")
    if not all(
        isinstance(field, bool) for field in (configured, measured, stale, transition_pending)
    ):
        return _invalid_pair_network_health("pair network-health flags must be booleans")
    state = value.get("state")
    raw_state = value.get("raw_state")
    if state not in {"good", "degraded", "unusable"} or raw_state not in {
        "good",
        "degraded",
        "unusable",
    }:
        return _invalid_pair_network_health("pair network-health state is unsupported")
    age = _canonical_nonnegative_integer(value.get("age_ns"))
    issues_value = value.get("issues")
    issues_list = cast("list[object]", issues_value) if isinstance(issues_value, list) else []
    if (
        age is None
        or not isinstance(issues_value, list)
        or not all(isinstance(issue, str) and bool(issue) for issue in issues_list)
    ):
        return _invalid_pair_network_health("pair network-health age or issues are malformed")
    issues = tuple(cast("list[str]", issues_list))

    peer_value = value.get("peer")
    peer_origin = ""
    peer_node_id = ""
    if peer_value is not None:
        if not isinstance(peer_value, Mapping):
            return _invalid_pair_network_health("pair network-health peer is malformed")
        peer = cast("Mapping[str, object]", peer_value)
        if set(peer) != {"origin", "node_id"}:
            return _invalid_pair_network_health("pair network-health peer is malformed")
        peer_origin = _string(peer.get("origin"))
        peer_node_id = _string(peer.get("node_id"))
        if not peer_origin or not peer_node_id:
            return _invalid_pair_network_health("pair network-health peer identity is empty")
    if cast("bool", configured) != (peer_value is not None):
        return _invalid_pair_network_health("pair network-health configured flag contradicts peer")
    if pose_mode == "leader" and configured is not True:
        return _invalid_pair_network_health("pose leader lacks configured network-health peer")
    if pose_mode in {"shadow", "disabled"} and configured is not False:
        return _invalid_pair_network_health(f"pose {pose_mode} unexpectedly measures a peer")

    measured_at = value.get("measured_at_elapsed_realtime_ns")
    local_to_peer = value.get("local_to_peer")
    peer_to_local = value.get("peer_to_local")
    measurement_present = (
        _canonical_nonnegative_integer(measured_at) is not None
        and local_to_peer is not None
        and peer_to_local is not None
    )
    if cast("bool", measured) != measurement_present:
        return _invalid_pair_network_health(
            "pair network-health measured flag contradicts evidence"
        )
    if measured is True and not (
        _pair_network_direction_valid(local_to_peer)
        and _pair_network_direction_valid(peer_to_local)
    ):
        return _invalid_pair_network_health("pair network-health direction evidence is malformed")
    if measured is False and (
        age != 0
        or stale is not False
        or state != "unusable"
        or raw_state != "unusable"
        or transition_pending is not False
        or measured_at is not None
        or local_to_peer is not None
        or peer_to_local is not None
    ):
        return _invalid_pair_network_health("unmeasured pair network-health fields disagree")
    if stale is True and (
        measured is not True
        or state != "unusable"
        or raw_state != "unusable"
        or transition_pending is not False
    ):
        return _invalid_pair_network_health("stale pair network-health fields disagree")
    if cast("bool", transition_pending) != (stale is False and state != raw_state):
        return _invalid_pair_network_health("pair network-health transition flag disagrees")
    if state == "good" and raw_state == "good" and issues:
        return _invalid_pair_network_health("good pair network health unexpectedly reports issues")
    if (state != "good" or raw_state != "good") and not issues:
        return _invalid_pair_network_health("non-good pair network health lacks an issue")
    return _PairNetworkHealthAssessment(
        valid=True,
        diagnostic=f"pair network-health contract is valid for pose {pose_mode}",
        state=cast("str", state),
        measured=cast("bool", measured),
        stale=cast("bool", stale),
        peer_origin=peer_origin,
        peer_node_id=peer_node_id,
        issues=issues,
    )


def _web_hosting_status(assets: Sequence[HostedWebAsset]) -> tuple[bool, str]:  # noqa: C901
    expected = {
        "/": ("text/html; charset=utf-8", "no-store"),
        "/app.js": ("text/javascript; charset=utf-8", "no-cache"),
        "/app.css": ("text/css; charset=utf-8", "no-cache"),
    }
    by_path = {asset.path: asset for asset in assets}
    problems: list[str] = []
    if len(by_path) != len(assets) or set(by_path) != set(expected):
        problems.append("asset inventory is incomplete or duplicated")
    for path, (content_type, cache_control) in expected.items():
        asset = by_path.get(path)
        if asset is None:
            continue
        if asset.status != HTTP_OK:
            problems.append(f"{path} returned HTTP {asset.status}")
        if not asset.direct_response:
            problems.append(f"{path} redirected away from the phone")
        if asset.content_type.lower() != content_type:
            problems.append(f"{path} has content type {asset.content_type!r}")
        if cache_control not in asset.cache_control.lower():
            problems.append(f"{path} has cache policy {asset.cache_control!r}")
        if not asset.nosniff:
            problems.append(f"{path} lacks nosniff")
        if asset.body_bytes <= 0 or asset.body_bytes > MAXIMUM_WEB_ASSET_BYTES:
            problems.append(f"{path} has invalid bounded size {asset.body_bytes}")
        if not asset.required_markers_present:
            problems.append(f"{path} is missing its required bundle marker")
    if problems:
        return False, "; ".join(problems)
    sizes = ", ".join(f"{path}={by_path[path].body_bytes}B" for path in sorted(expected))
    return True, f"phone-hosted review document, script, and stylesheet are ready ({sizes})"


def _browser_cors_status(evidence: BrowserCorsEvidence) -> tuple[bool, str]:
    methods = {value.strip().upper() for value in evidence.allow_methods.split(",")}
    headers = {value.strip().lower() for value in evidence.allow_headers.split(",")}
    expected_methods = {"GET", "HEAD", "POST", "PUT", "OPTIONS"}
    expected_headers = {"authorization", "range", "content-type"}
    ready = (
        evidence.status == HTTP_NO_CONTENT
        and evidence.direct_response
        and evidence.allow_origin == "*"
        and expected_methods.issubset(methods)
        and expected_headers.issubset(headers)
    )
    if ready:
        return True, "cross-origin browser control preflight accepts auth, range, and mutations"
    return (
        False,
        _message(
            "browser CORS preflight ",
            f"status={evidence.status} direct={evidence.direct_response} ",
            f"origin={evidence.allow_origin!r} methods={sorted(methods)!r} ",
            f"headers={sorted(headers)!r}",
        ),
    )


def _nullable_nonempty_string(value: object) -> bool:
    return value is None or (isinstance(value, str) and bool(value))


def _field_recording_status_assessment(  # noqa: PLR0911 -- fail-closed schema diagnostics.
    value: object,
) -> tuple[bool, bool, str]:
    """Validate the recorder schema and determine whether no recording work remains."""
    if not isinstance(value, Mapping):
        return False, False, "field-recording status is missing or not an object"
    status = cast("Mapping[str, object]", value)
    if len(status) != len(_FIELD_RECORDING_STATUS_FIELDS) or any(
        field not in status for field in _FIELD_RECORDING_STATUS_FIELDS
    ):
        return False, False, "field-recording status fields do not match schema 1"
    state = status.get("state")
    active_recording_id = status.get("active_recording_id")
    shared_recording_id = status.get("shared_recording_id")
    started_at = status.get("started_at_utc")
    started_elapsed = status.get("started_elapsed_realtime_ns")
    elapsed_ms = _strict_json_integer(status.get("elapsed_ms"))
    max_duration_seconds = _strict_json_integer(status.get("max_duration_seconds"))
    error = status.get("error")
    fault_flags = {
        name: status.get(name)
        for name in (
            "hil_reject_next_start",
            "hil_fail_next_accepted_start",
            "hil_accepted_start_waiting",
        )
    }
    schema_valid = (
        _strict_json_integer(status.get("schema_version")) == 1
        and isinstance(state, str)
        and state in _FIELD_RECORDING_STATES
        and _nullable_nonempty_string(active_recording_id)
        and _nullable_nonempty_string(shared_recording_id)
        and _nullable_nonempty_string(started_at)
        and (started_elapsed is None or _canonical_nonnegative_integer(started_elapsed) is not None)
        and elapsed_ms is not None
        and elapsed_ms >= 0
        and _canonical_nonnegative_integer(status.get("video_bytes")) is not None
        and _canonical_nonnegative_integer(status.get("audio_frames")) is not None
        and max_duration_seconds is not None
        and max_duration_seconds > 0
        and isinstance(error, str)
        and all(isinstance(flag, bool) for flag in fault_flags.values())
    )
    if not schema_valid:
        return False, False, "field-recording status values are malformed"
    if state == "idle" and (
        active_recording_id is not None
        or shared_recording_id is not None
        or started_at is not None
        or started_elapsed is not None
        or elapsed_ms != 0
        or status.get("video_bytes") != "0"
        or status.get("audio_frames") != "0"
        or error != ""
    ):
        return False, False, "idle field-recording status retains active recording evidence"
    if state in _TERMINAL_FIELD_RECORDING_STATES and active_recording_id is not None:
        return False, False, "terminal field-recording status retains an active recording ID"
    if state in {"starting", "recording", "stopping"} and (
        active_recording_id is None or shared_recording_id is None
    ):
        return False, False, "active field-recording status lacks recording identifiers"
    if status.get("hil_accepted_start_waiting") is True and (
        status.get("hil_fail_next_accepted_start") is not True or state != "starting"
    ):
        return False, False, "field-recording injected-fault waiting state is contradictory"
    identifiers_clean = active_recording_id is None and (
        state == "ready" or shared_recording_id is None
    )
    clean = (
        state in _CLEAN_FIELD_RECORDING_STATES
        and identifiers_clean
        and all(flag is False for flag in fault_flags.values())
    )
    diagnostic = _message(
        f"field-recording state={state!r} active_recording_id={active_recording_id!r} ",
        f"shared_recording_id={shared_recording_id!r} fault_flags={fault_flags!r}",
    )
    return True, clean, diagnostic


def _stopped_clean_status(status: Mapping[str, object]) -> tuple[bool, str]:
    """Require one node to be fully stopped with no pending autonomous publication work."""
    state = _string(status.get("state"))
    armed = status.get("armed")
    active_session_id = status.get("active_session_id")
    pose = _optional_mapping(status.get("pose"))
    autonomous = _optional_mapping(pose.get("autonomous_pair"))
    autonomous_state = _string(autonomous.get("state"))
    autonomous_session_id = autonomous.get("active_shared_session_id")
    replication_backlog_size = _integer(autonomous.get("replication_backlog_size"))
    publication_flags = {
        name: autonomous.get(name)
        for name in ("local_triggered", "peer_triggered", "local_published", "peer_published")
    }
    stopped = (
        state in _STOPPED_CAPTURE_STATES
        and armed is False
        and "active_session_id" in status
        and active_session_id is None
    )
    autonomous_clean = (
        autonomous_state == "stopped"
        and "active_shared_session_id" in autonomous
        and autonomous_session_id is None
        and replication_backlog_size == 0
        and all(value is False for value in publication_flags.values())
    )
    passed = stopped and autonomous_clean
    diagnostic = _message(
        f"capture state={state!r} armed={armed!s} active_session_id={active_session_id!r}; ",
        f"autonomous_state={autonomous_state!r} ",
        f"active_shared_session_id={autonomous_session_id!r} ",
        f"replication_backlog_size={replication_backlog_size!r} ",
        f"publication_flags={publication_flags!r}",
    )
    return passed, diagnostic


def evaluate_pair(  # noqa: C901, PLR0912, PLR0915
    nodes: Sequence[NodeEvidence],
    *,
    require_monitoring: bool = False,
    require_stopped_clean: bool = False,
    require_wireless_adb: bool = False,
    expected_apk_sha256: str | None = None,
) -> list[Check]:
    """Evaluate the field-session contract using already-collected evidence."""
    if require_monitoring and require_stopped_clean:
        message = "monitoring and stopped-clean admission are mutually exclusive"
        raise ValueError(message)
    checks: list[Check] = []
    if len(nodes) != PAIR_NODE_COUNT:
        return [
            Check(
                name="pair.count",
                passed=False,
                message="exactly two phones are required",
            )
        ]

    node_ids: list[str] = []
    roles: list[str] = []
    modes: list[str] = []
    leader: NodeEvidence | None = None
    shadow: NodeEvidence | None = None
    leader_network_health: _PairNetworkHealthAssessment | None = None
    for index, evidence in enumerate(nodes):
        prefix = f"node.{index + 1}"
        descriptor = evidence.descriptor
        setup = evidence.setup
        status = evidence.status
        configuration = _mapping(setup.get("configuration"), f"{prefix} configuration")
        pose = _mapping(configuration.get("pose"), f"{prefix} pose")
        readiness = _mapping(setup.get("readiness"), f"{prefix} readiness")
        device_admission = _optional_mapping(setup.get("device_admission"))
        reboot_recovery = _optional_mapping(setup.get("reboot_recovery"))
        node = _mapping(setup.get("node"), f"{prefix} node")
        node_id = _string(descriptor.get("node_id"))
        role = _string(descriptor.get("role"))
        mode = _string(_mapping(descriptor.get("pose"), f"{prefix} descriptor pose").get("mode"))
        urls = _array(descriptor.get("service_urls"), f"{prefix} service URLs")
        issues = _array(readiness.get("issues"), f"{prefix} readiness issues")
        configuration_issues = [
            issue for issue in issues if issue != "Stop capture before editing phone setup."
        ]
        node_ids.append(node_id)
        roles.append(role)
        modes.append(mode)
        if mode == "leader":
            leader = evidence
        elif mode == "shadow":
            shadow = evidence

        network_health = _pair_network_health_assessment(status.get("pair_network_health"), mode)
        if mode == "leader":
            leader_network_health = network_health
        field_recording_valid, field_recording_clean, field_recording_message = (
            _field_recording_status_assessment(evidence.field_recording_status)
        )

        identity_ok = (
            descriptor.get("schema_version") == 1
            and setup.get("schema_version") == 1
            and status.get("schema_version") == PAIR_NODE_COUNT
            and evidence.clock.get("schema_version") == 1
            and bool(node_id)
            and node.get("node_id") == node_id
            and evidence.clock.get("node_id") == node_id
            and descriptor.get("control_authentication") == "bearer"
            and configuration.get("role") == role
        )
        checks.append(Check(f"{prefix}.identity", identity_ok, "stable identity and schema agree"))
        checks.append(
            Check(
                f"{prefix}.pair_network_health_contract",
                network_health.valid,
                network_health.diagnostic,
            )
        )
        checks.append(
            Check(
                f"{prefix}.field_recording_contract",
                field_recording_valid,
                field_recording_message,
            )
        )
        checks.append(
            Check(
                f"{prefix}.lan_origin",
                evidence.origin in urls
                and evidence.unauthenticated_setup_status == HTTP_UNAUTHORIZED,
                "advertised origin is direct and setup rejects an unauthenticated request",
            )
        )
        web_hosting_ready, web_hosting_message = _web_hosting_status(evidence.web_assets)
        checks.append(
            Check(
                f"{prefix}.web_hosting",
                web_hosting_ready,
                web_hosting_message,
            )
        )
        browser_cors_ready, browser_cors_message = _browser_cors_status(evidence.browser_cors)
        checks.append(
            Check(
                f"{prefix}.browser_cors",
                browser_cors_ready,
                browser_cors_message,
            )
        )
        checks.append(
            Check(
                f"{prefix}.profile",
                descriptor.get("capture_profile") == "720p240"
                and configuration.get("capture_profile") == "720p240",
                "production 720p240 profile is selected",
            )
        )
        checks.append(
            Check(
                f"{prefix}.readiness",
                len(configuration_issues) == 0,
                "setup reports no configuration readiness issues"
                if not configuration_issues
                else "; ".join(map(str, configuration_issues)),
            )
        )
        admission_issues_value = device_admission.get("issues")
        admission_issues = (
            _array(cast("object", admission_issues_value), f"{prefix} device admission issues")
            if isinstance(admission_issues_value, list)
            else ()
        )
        device_admitted = (
            device_admission.get("schema_version") == 1
            and device_admission.get("profile") == "720p240"
            and device_admission.get("ready") is True
            and device_admission.get("probe_succeeded") is True
            and (_integer(device_admission.get("api_level")) or 0) >= MINIMUM_ANDROID_API_LEVEL
            and not admission_issues
        )
        checks.append(
            Check(
                f"{prefix}.device_admission",
                device_admitted,
                (
                    "current APK admits this phone for the complete 720p240 product floor"
                    if device_admitted
                    else _message(
                        "device admission ",
                        f"ready={device_admission.get('ready')!s} ",
                        f"probe={device_admission.get('probe_succeeded')!s} ",
                        f"api={device_admission.get('api_level')!s} ",
                        f"issues={list(admission_issues)!r}",
                    )
                ),
            )
        )
        reboot_issues_value = reboot_recovery.get("issues")
        reboot_issues = (
            _array(cast("object", reboot_issues_value), f"{prefix} reboot recovery issues")
            if isinstance(reboot_issues_value, list)
            else ()
        )
        ready_this_boot = (
            reboot_recovery.get("schema_version") == 1
            and reboot_recovery.get("mode") == "operator_launch_after_first_unlock"
            and reboot_recovery.get("direct_boot_aware") is False
            and reboot_recovery.get("boot_receiver_registered") is True
            and reboot_recovery.get("automatic_capture_before_first_unlock") is False
            and reboot_recovery.get("operator_foreground_launch_required_after_os_reboot") is True
            and reboot_recovery.get("process_restart_policy") == "android_start_sticky_best_effort"
            and reboot_recovery.get("user_unlocked") is True
            and reboot_recovery.get("service_running") is True
            and reboot_recovery.get("ready_this_boot") is True
            and not reboot_issues
        )
        checks.append(
            Check(
                f"{prefix}.current_boot",
                ready_this_boot,
                (
                    "phone is unlocked and the operator-started service is ready this boot"
                    if ready_this_boot
                    else _message(
                        "current-boot readiness ",
                        f"unlocked={reboot_recovery.get('user_unlocked')!s} ",
                        f"service={reboot_recovery.get('service_running')!s} ",
                        f"ready={reboot_recovery.get('ready_this_boot')!s} ",
                        f"issues={list(reboot_issues)!r}",
                    )
                ),
            )
        )
        monitoring = status.get("armed") is True and _string(status.get("state")) == "armed"
        checks.append(
            Check(
                f"{prefix}.monitoring",
                monitoring or not require_monitoring,
                "station is armed" if monitoring else "station is not armed (allowed by this run)",
            )
        )
        stopped_clean, stopped_clean_message = _stopped_clean_status(status)
        checks.append(
            Check(
                f"{prefix}.stopped_clean",
                stopped_clean or not require_stopped_clean,
                stopped_clean_message,
            )
        )
        checks.append(
            Check(
                f"{prefix}.field_recording_stopped_clean",
                field_recording_clean or not require_stopped_clean,
                field_recording_message,
            )
        )
        if require_monitoring:
            pose_status = _optional_mapping(status.get("pose"))
            metrics = _optional_mapping(pose_status.get("metrics"))
            configured_delegate = _string(pose_status.get("configured_delegate"))
            actual_delegate = _string(metrics.get("delegate"))
            successful_inferences = _integer(metrics.get("successful_inferences"))
            failed_inferences = _integer(metrics.get("failed_inferences"))
            inference_p95 = _integer(metrics.get("inference_duration_p95_ns"))
            maximum_inference = _integer(metrics.get("maximum_inference_duration_ns"))
            recent_samples = _integer(metrics.get("recent_inference_sample_count"))
            recent_inference_p95 = _integer(metrics.get("recent_inference_duration_p95_ns"))
            recent_maximum_inference = _integer(metrics.get("recent_maximum_inference_duration_ns"))
            recent_deadline_misses = _integer(metrics.get("recent_inference_deadline_misses"))
            rejected_decision_timestamps = _integer(metrics.get("rejected_decision_timestamps"))
            production_runtime = (
                pose_status.get("experiment_enabled") is False
                and _string(pose_status.get("model_variant")) == "lite"
                and _string(pose_status.get("model_asset_path")) == "pose_landmarker_lite.task"
                and pose_status.get("actual_standby_width") == PRODUCTION_POSE_STANDBY_WIDTH
                and pose_status.get("actual_standby_height") == PRODUCTION_POSE_STANDBY_HEIGHT
                and configured_delegate == _string(pose.get("inference_delegate"))
                and (
                    (configured_delegate == "gpu_preferred" and actual_delegate == "gpu")
                    or (configured_delegate == "gpu_required" and actual_delegate == "gpu")
                    or (configured_delegate == "cpu_only" and actual_delegate == "cpu")
                    or (configured_delegate == "npu_required" and actual_delegate == "npu")
                    or (
                        configured_delegate == "npu_preferred"
                        and actual_delegate in {"cpu", "gpu", "npu"}
                    )
                )
            )
            pose_monitoring = (
                _string(pose_status.get("mode")) == mode
                and _string(pose_status.get("phase")) == "monitoring"
                and production_runtime
                and successful_inferences is not None
                and successful_inferences > 0
                and failed_inferences == 0
                and inference_p95 is not None
                and inference_p95 > 0
                and maximum_inference is not None
                and maximum_inference > 0
                and recent_samples is not None
                and MINIMUM_RECENT_POSE_INFERENCE_SAMPLES
                <= recent_samples
                <= MAXIMUM_RECENT_POSE_INFERENCE_SAMPLES
                and recent_samples <= successful_inferences + failed_inferences
                and recent_inference_p95 is not None
                and 0 < recent_inference_p95 <= MAXIMUM_POSE_INFERENCE_P95_NS
                and recent_maximum_inference is not None
                and 0 < recent_maximum_inference <= MAXIMUM_POSE_INFERENCE_OUTLIER_NS
                and recent_deadline_misses is not None
                and 0 <= recent_deadline_misses <= recent_samples
                and rejected_decision_timestamps == 0
            )
            checks.append(
                Check(
                    f"{prefix}.pose_monitoring",
                    pose_monitoring,
                    _message(
                        "pose monitoring metrics ",
                        f"configured_delegate={configured_delegate!r} ",
                        f"actual_delegate={actual_delegate!r} ",
                        f"model={_string(pose_status.get('model_variant'))!r} ",
                        _message(
                            "standby_size=",
                            f"{pose_status.get('actual_standby_width')}x",
                            f"{pose_status.get('actual_standby_height')} ",
                        ),
                        f"successful={successful_inferences} failed={failed_inferences} ",
                        f"recent_samples={recent_samples} ",
                        f"recent_p95_ns={recent_inference_p95} ",
                        f"recent_max_ns={recent_maximum_inference} ",
                        f"recent_deadline_misses={recent_deadline_misses} ",
                        f"lifetime_p95_ns={inference_p95} lifetime_max_ns={maximum_inference} ",
                        f"rejected_timestamps={rejected_decision_timestamps}",
                    ),
                )
            )
            standby_audio = _optional_mapping(pose_status.get("standby_audio"))
            retained_audio_frames = _integer(standby_audio.get("end_frame_position"))
            standby_audio_ready = (
                standby_audio.get("ready") is True
                and retained_audio_frames is not None
                and retained_audio_frames > 0
                and _string(standby_audio.get("last_error")) == ""
            )
            checks.append(
                Check(
                    f"{prefix}.standby_audio",
                    standby_audio_ready,
                    _message(
                        "standby audio ",
                        f"ready={standby_audio.get('ready')!s} ",
                        f"retained_frames={retained_audio_frames} ",
                        f"last_error={_string(standby_audio.get('last_error'))!r}",
                    ),
                )
            )
        wireless = ":" in evidence.serial
        checks.append(
            Check(
                f"{prefix}.adb_transport",
                wireless or not require_wireless_adb,
                f"authorized {evidence.adb_transport} adb transport",
            )
        )
        if expected_apk_sha256 is not None:
            checks.append(
                Check(
                    f"{prefix}.exact_apk",
                    evidence.installed_apk_sha256 == expected_apk_sha256,
                    "installed APK exactly matches the Bazel artifact"
                    if evidence.installed_apk_sha256 == expected_apk_sha256
                    else "installed APK differs from the requested Bazel artifact",
                )
            )
        telemetry = evidence.telemetry
        telemetry_ok = (
            telemetry.battery_percent >= MINIMUM_BATTERY_PERCENT
            and telemetry.thermal_status <= MAXIMUM_THERMAL_STATUS
            and telemetry.storage_usable_bytes >= MINIMUM_STORAGE_BYTES
        )
        resource_message = "".join(
            (
                f"battery={telemetry.battery_percent:.0f}% ",
                f"temperature={telemetry.battery_temperature_celsius:.1f}C ",
                f"thermal={telemetry.thermal_status} ",
                f"storage={telemetry.storage_usable_bytes / (1024**3):.1f}GiB",
            )
        )
        checks.append(
            Check(
                f"{prefix}.resources",
                telemetry_ok,
                resource_message,
            )
        )
        peer = pose.get("peer")
        if mode == "leader":
            peer_mapping: Mapping[str, object] = (
                cast("Mapping[str, object]", peer)
                if isinstance(peer, Mapping)
                else cast("Mapping[str, object]", {})
            )
            checks.append(
                Check(
                    f"{prefix}.leader_peer_configured",
                    bool(_string(peer_mapping.get("origin"))),
                    "leader has an outbound peer origin",
                )
            )

    checks.append(
        Check(
            "pair.unique_identity",
            len(set(node_ids)) == PAIR_NODE_COUNT,
            "node IDs are distinct",
        )
    )
    checks.append(
        Check(
            "pair.roles",
            set(roles) == {"down_the_line", "face_on"},
            "roles are complementary down-the-line and face-on",
        )
    )
    checks.append(
        Check(
            "pair.pose_topology",
            set(modes) == {"leader", "shadow"},
            "pose topology contains one leader and one shadow",
        )
    )
    if leader is not None and shadow is not None:
        leader_configuration = _mapping(leader.setup.get("configuration"), "leader configuration")
        leader_pose = _mapping(leader_configuration.get("pose"), "leader pose")
        leader_peer = _mapping(leader_pose.get("peer"), "leader peer")
        pairing = _mapping(leader.setup.get("pairing"), "leader pairing")
        binding_ok = (
            leader_peer.get("origin") == shadow.origin
            and pairing.get("state") == "active"
            and pairing.get("peer_node_id") == shadow.descriptor.get("node_id")
            and pairing.get("expected_role") == shadow.descriptor.get("role")
            and pairing.get("origin") == shadow.origin
            and pairing.get("credential_status") == "verified"
            and isinstance(pairing.get("credential_generation"), int)
            and cast("int", pairing.get("credential_generation")) >= 1
        )
        binding_message = (
            "leader binding matches the reachable shadow identity, role, address, and credential"
        )
        checks.append(
            Check(
                "pair.authenticated_binding",
                binding_ok,
                binding_message,
            )
        )
        network_peer_matches = (
            leader_network_health is not None
            and leader_network_health.peer_origin == shadow.origin
            and leader_network_health.peer_node_id == shadow.descriptor.get("node_id")
        )
        network_viable = (
            leader_network_health is not None
            and leader_network_health.valid
            and network_peer_matches
            and leader_network_health.measured
            and not leader_network_health.stale
            and leader_network_health.state in {"good", "degraded"}
        )
        if leader_network_health is None:
            network_message = "leader pair network-health evidence is absent"
        elif not leader_network_health.valid:
            network_message = leader_network_health.diagnostic
        elif not network_peer_matches:
            network_message = (
                "leader pair network-health peer does not match the authenticated shadow"
            )
        elif leader_network_health.state == "degraded" and network_viable:
            network_message = _message(
                "WARNING: degraded pair network health; arming requires explicit operator ",
                "override; ",
                "; ".join(leader_network_health.issues),
            )
        elif network_viable:
            network_message = "bidirectional application-level pair network health is good"
        else:
            network_message = _message(
                "pair network health is not viable: ",
                leader_network_health.diagnostic,
                f" state={leader_network_health.state!r}",
                f" measured={leader_network_health.measured}",
                f" stale={leader_network_health.stale}",
                f" issues={list(leader_network_health.issues)!r}",
            )
        checks.append(Check("pair.network_health", network_viable, network_message))
        if require_monitoring:
            leader_status_pose = _optional_mapping(leader.status.get("pose"))
            autonomous_pair = _optional_mapping(leader_status_pose.get("autonomous_pair"))
            peer_clock = _optional_mapping(leader_status_pose.get("peer_clock"))
            standby_audio = _optional_mapping(leader_status_pose.get("standby_audio"))
            peer_uncertainty = _integer(peer_clock.get("uncertainty_ns"))
            peer_age = _integer(peer_clock.get("age_ns"))
            peer_visible = (
                _string(autonomous_pair.get("state")) == "monitoring"
                and autonomous_pair.get("peer_available") is True
            )
            clock_usable = (
                peer_clock.get("peer_node_id") == shadow.descriptor.get("node_id")
                and peer_uncertainty is not None
                and 0 <= peer_uncertainty <= MAXIMUM_PEER_CLOCK_UNCERTAINTY_NS
                and peer_age is not None
                and 0 <= peer_age <= MAXIMUM_PEER_CLOCK_AGE_NS
            )
            audio_ready = (
                standby_audio.get("ready") is True
                and _string(standby_audio.get("last_error")) == ""
            )
            checks.extend(
                (
                    Check(
                        "pair.peer_reachable",
                        peer_visible,
                        "leader autonomous monitoring can reach the configured peer",
                    ),
                    Check(
                        "pair.peer_clock",
                        clock_usable,
                        f"leader clock mapping uncertainty_ns={peer_uncertainty} age_ns={peer_age}",
                    ),
                    Check(
                        "pair.standby_audio",
                        audio_ready,
                        _message(
                            "leader standby audio trigger input is ready=",
                            f"{standby_audio.get('ready')!s} last_error=",
                            f"{_string(standby_audio.get('last_error'))!r}",
                        ),
                    ),
                )
            )
    else:
        checks.append(
            Check(
                name="pair.authenticated_binding",
                passed=False,
                message="leader/shadow topology is absent",
            )
        )
    return checks


class EvidenceCollector:
    """Collect bounded read-only evidence from adb and direct LAN APIs."""

    def __init__(
        self,
        adb: Path,
        *,
        command_runner: Callable[[Sequence[str]], str] | None = None,
        http_reader: Callable[[str, str | None], tuple[int, Mapping[str, object]]] | None = None,
        asset_reader: Callable[[str, Sequence[bytes]], HostedWebAsset] | None = None,
        cors_reader: Callable[[str], BrowserCorsEvidence] | None = None,
    ) -> None:
        """Initialize with replaceable subprocess and HTTP boundaries for tests."""
        self._adb: Path = adb
        self._command_runner: Callable[[Sequence[str]], str] = command_runner or self._run_command
        self._http_reader: Callable[[str, str | None], tuple[int, Mapping[str, object]]] = (
            http_reader or self._read_json
        )
        self._asset_reader: Callable[[str, Sequence[bytes]], HostedWebAsset] = (
            asset_reader or self._read_asset
        )
        self._cors_reader: Callable[[str], BrowserCorsEvidence] = cors_reader or self._read_cors

    @staticmethod
    def _run_command(arguments: Sequence[str]) -> str:
        completed = subprocess.run(
            arguments,
            check=True,
            capture_output=True,
            text=True,
            timeout=4,
        )
        return completed.stdout.strip()

    @staticmethod
    def _read_json(url: str, token: str | None) -> tuple[int, Mapping[str, object]]:
        headers = {"Accept": "application/json"}
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        request = urllib.request.Request(url, headers=headers)  # noqa: S310
        try:
            # Callers accept only HTTP origins; no file or custom URL schemes reach here.
            opener = urllib.request.build_opener(_RejectRedirectHandler())
            raw_response = cast(
                "HTTPResponse",
                opener.open(request, timeout=HTTP_TIMEOUT_SECONDS),
            )
            with raw_response as response:
                raw_body = cast("object", json.load(response))
                return response.status, _mapping(raw_body, "HTTP response")
        except urllib.error.HTTPError as failure:
            try:
                raw_body = cast("object", json.load(failure))
                body: Mapping[str, object] = _mapping(raw_body, "HTTP error response")
            except (ValueError, json.JSONDecodeError):
                body = {}
            return failure.code, body

    @staticmethod
    def _read_asset(url: str, required_markers: Sequence[bytes]) -> HostedWebAsset:
        request = urllib.request.Request(url, headers={"Accept": "text/html,*/*;q=0.8"})  # noqa: S310
        path = urllib.parse.urlsplit(url).path
        try:
            raw_response = cast(
                "HTTPResponse",
                urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS),  # noqa: S310
            )
            with raw_response as response:
                body = response.read(MAXIMUM_WEB_ASSET_BYTES + 1)
                return HostedWebAsset(
                    path=path,
                    status=response.status,
                    direct_response=response.url == url,
                    content_type=response.headers.get("Content-Type", ""),
                    cache_control=response.headers.get("Cache-Control", ""),
                    nosniff=response.headers.get("X-Content-Type-Options", "").lower() == "nosniff",
                    body_bytes=len(body),
                    required_markers_present=all(marker in body for marker in required_markers),
                )
        except urllib.error.HTTPError as failure:
            body = failure.read(MAXIMUM_WEB_ASSET_BYTES + 1)
            return HostedWebAsset(
                path=path,
                status=failure.code,
                direct_response=failure.url == url,
                content_type=failure.headers.get("Content-Type", ""),
                cache_control=failure.headers.get("Cache-Control", ""),
                nosniff=failure.headers.get("X-Content-Type-Options", "").lower() == "nosniff",
                body_bytes=len(body),
                required_markers_present=all(marker in body for marker in required_markers),
            )

    @staticmethod
    def _read_cors(url: str) -> BrowserCorsEvidence:
        request = urllib.request.Request(  # noqa: S310 - caller supplies a validated HTTP origin.
            url,
            method="OPTIONS",
            headers={
                "Origin": "http://field-review-client.invalid",
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "Authorization, Range, Content-Type",
            },
        )
        try:
            raw_response = cast(
                "HTTPResponse",
                urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS),  # noqa: S310
            )
            with raw_response as response:
                return BrowserCorsEvidence(
                    status=response.status,
                    direct_response=response.url == url,
                    allow_origin=response.headers.get("Access-Control-Allow-Origin", ""),
                    allow_methods=response.headers.get("Access-Control-Allow-Methods", ""),
                    allow_headers=response.headers.get("Access-Control-Allow-Headers", ""),
                )
        except urllib.error.HTTPError as failure:
            return BrowserCorsEvidence(
                status=failure.code,
                direct_response=failure.url == url,
                allow_origin=failure.headers.get("Access-Control-Allow-Origin", ""),
                allow_methods=failure.headers.get("Access-Control-Allow-Methods", ""),
                allow_headers=failure.headers.get("Access-Control-Allow-Headers", ""),
            )

    def _adb_command(self, serial: str, *arguments: str) -> str:
        return self._command_runner([str(self._adb), "-s", serial, *arguments])

    def _json_endpoint(
        self,
        origin: str,
        path: str,
        token: str | None,
        label: str,
    ) -> tuple[int, Mapping[str, object]]:
        try:
            return self._http_reader(f"{origin}{path}", token)
        except (OSError, RuntimeError, TypeError, ValueError) as failure:
            message = f"{label} request to {origin}{path} failed: {failure}"
            raise RuntimeError(message) from failure

    def collect_lan_diagnostics(
        self, serial: str, origin: str, peer_origin: str
    ) -> LanNodeDiagnostics:
        """Probe every required path independently so failures remain matrix evidence."""
        normalized_origin = origin.rstrip("/")
        normalized_peer_origin = peer_origin.rstrip("/")
        try:
            bssid = parse_wifi_bssid(self._adb_command(serial, "shell", "cmd", "wifi", "status"))
        except (OSError, RuntimeError, TypeError, ValueError, subprocess.SubprocessError):
            bssid = None

        host_status: int | None = None
        host_reachable = False
        try:
            _direct_lan_origin(normalized_origin)
            host_status, clock = self._json_endpoint(
                normalized_origin, "/api/v1/clock", None, "host-to-phone LAN clock"
            )
            host_reachable = (
                host_status == HTTP_OK
                and clock.get("schema_version") == 1
                and bool(_string(clock.get("node_id")))
            )
        except (OSError, RuntimeError, TypeError, ValueError, subprocess.SubprocessError):
            pass

        phone_reachable = False
        try:
            phone_host, _ = _direct_lan_origin(normalized_origin)
            peer_host, _ = _direct_lan_origin(normalized_peer_origin)
            expected_source = ipaddress.ip_address(phone_host)
            expected_peer = ipaddress.ip_address(peer_host)
            if expected_source.version == expected_peer.version:
                family_arguments = ("-6",) if expected_peer.version == IPV6_VERSION else ()
                route = self._adb_command(
                    serial,
                    "shell",
                    "ip",
                    *family_arguments,
                    "route",
                    "get",
                    str(expected_peer),
                )
                # ADB is only the command channel. Route provenance ensures that a healthy USB
                # transport or host-side port forward cannot satisfy this phone-originated edge.
                if _uses_expected_wifi_route(route, expected_peer, expected_source):
                    self._adb_command(
                        serial,
                        "shell",
                        "ping",
                        "-c",
                        "1",
                        "-W",
                        "2",
                        str(expected_peer),
                    )
                    phone_reachable = True
        except (OSError, RuntimeError, TypeError, ValueError, subprocess.SubprocessError):
            pass

        return LanNodeDiagnostics(
            serial=serial,
            origin=normalized_origin,
            bssid=bssid,
            host_to_phone=ReachabilityEvidence(
                source="field_host",
                target=normalized_origin,
                transport="direct_http_clock",
                reachable=host_reachable,
                http_status=host_status,
            ),
            phone_to_peer=ReachabilityEvidence(
                source=normalized_origin,
                target=normalized_peer_origin,
                transport="adb_shell_wifi_icmp",
                reachable=phone_reachable,
            ),
        )

    def collect(self, serial: str, origin: str) -> NodeEvidence:
        """Collect one complete node snapshot or fail before pair evaluation."""
        state = self._adb_command(serial, "get-state")
        if state != "device":
            message = f"adb transport for {serial} is not authorized"
            raise RuntimeError(message)
        model = self._adb_command(serial, "shell", "getprop", "ro.product.model")
        preferences = self._adb_command(
            serial,
            "exec-out",
            "run-as",
            PACKAGE_NAME,
            "cat",
            "shared_prefs/node_configuration.xml",
        )
        token = parse_control_token(preferences)
        # Snapshot the live monitoring state before the doctor hashes the APK or asks the setup
        # endpoint to assemble its larger capability report. Those admission checks are still
        # required below, but the measurement tool must not load the phone immediately before it
        # samples latency and peer-clock readiness.
        status_status, status = self._json_endpoint(
            origin, "/api/v1/capture/status", token, "authenticated capture status"
        )
        field_recording_status_code, field_recording_status = self._json_endpoint(
            origin,
            "/api/v1/field-recording/status",
            token,
            "authenticated field-recording status",
        )
        package_path = self._adb_command(serial, "shell", "pm", "path", PACKAGE_NAME)
        try:
            installed_apk_path = parse_package_path(package_path)
            installed_apk_sha256 = parse_sha256(
                self._adb_command(serial, "shell", "sha256sum", installed_apk_path),
                installed_apk_path,
            )
        except ValueError as failure:
            message = f"capture app on {serial} is absent, split, or has no verifiable base APK"
            raise RuntimeError(message) from failure
        descriptor_status, descriptor = self._json_endpoint(
            origin, "/api/v1/node", token, "authenticated node descriptor"
        )
        setup_status, setup = self._json_endpoint(
            origin, "/api/v1/setup", token, "authenticated setup"
        )
        clock_status, clock = self._json_endpoint(origin, "/api/v1/clock", None, "node clock")
        unauthenticated_setup_status, _ = self._json_endpoint(
            origin, "/api/v1/setup", None, "unauthenticated setup rejection"
        )
        expected_statuses = (HTTP_OK,) * 5
        if (
            descriptor_status,
            setup_status,
            status_status,
            field_recording_status_code,
            clock_status,
        ) != expected_statuses:
            message = f"one or more direct LAN APIs on {origin} are unavailable"
            raise RuntimeError(message)
        try:
            web_assets = (
                self._asset_reader(
                    f"{origin}/",
                    (b'href="/app.css"', b'src="/app.js"'),
                ),
                self._asset_reader(f"{origin}/app.js", (b"Swing review",)),
                self._asset_reader(f"{origin}/app.css", (b".review-shell",)),
            )
            browser_cors = self._cors_reader(f"{origin}/api/v1/capture/status")
        except (OSError, RuntimeError, TypeError, ValueError) as failure:
            message = f"phone-hosted review UI request to {origin} failed: {failure}"
            raise RuntimeError(message) from failure
        battery = self._adb_command(serial, "shell", "dumpsys", "battery")
        thermal = self._adb_command(serial, "shell", "dumpsys", "thermalservice")
        available_blocks = self._adb_command(
            serial,
            "shell",
            "run-as",
            PACKAGE_NAME,
            "stat",
            "-f",
            "-c",
            "%a",
            "files",
        )
        block_size = self._adb_command(
            serial,
            "shell",
            "run-as",
            PACKAGE_NAME,
            "stat",
            "-f",
            "-c",
            "%S",
            "files",
        )
        if (
            re.fullmatch(r"\d+", available_blocks) is None
            or re.fullmatch(r"\d+", block_size) is None
        ):
            message = f"Android storage telemetry on {serial} is malformed"
            raise RuntimeError(message)
        storage = str(int(available_blocks) * int(block_size))
        return NodeEvidence(
            serial=serial,
            origin=origin.rstrip("/"),
            model=model,
            adb_transport="wireless" if ":" in serial else "usb",
            installed_apk_sha256=installed_apk_sha256,
            descriptor=descriptor,
            setup=setup,
            status=status,
            field_recording_status=field_recording_status,
            clock=clock,
            unauthenticated_setup_status=unauthenticated_setup_status,
            web_assets=web_assets,
            browser_cors=browser_cors,
            telemetry=parse_telemetry(battery, thermal, storage),
        )


def evaluate_lan_diagnostics(diagnostics: Sequence[LanNodeDiagnostics]) -> list[Check]:
    """Gate the BSSID inventory and every required directed direct-LAN edge."""
    if len(diagnostics) != PAIR_NODE_COUNT:
        return [
            Check(
                name="lan_diagnostics.count",
                passed=False,
                message="exactly two phone LAN diagnostic rows are required",
            )
        ]
    checks: list[Check] = []
    for index, diagnostic in enumerate(diagnostics):
        prefix = f"node.{index + 1}"
        checks.extend(
            (
                Check(
                    f"{prefix}.wifi_bssid",
                    diagnostic.bssid is not None,
                    f"current Wi-Fi BSSID={diagnostic.bssid}"
                    if diagnostic.bssid is not None
                    else "current Wi-Fi BSSID is unavailable",
                ),
                Check(
                    f"{prefix}.host_to_phone_lan",
                    diagnostic.host_to_phone.reachable,
                    (
                        "field host reached the phone's public clock over its direct HTTP origin"
                        if diagnostic.host_to_phone.reachable
                        else _message(
                            "field host could not reach the phone's direct HTTP origin; ",
                            "USB forwarding and neighbor-table evidence do not qualify this edge",
                        )
                    ),
                ),
                Check(
                    f"{prefix}.phone_to_peer_lan",
                    diagnostic.phone_to_peer.reachable,
                    (
                        "phone reached its peer over Wi-Fi ICMP"
                        if diagnostic.phone_to_peer.reachable
                        else _message(
                            "phone could not reach its peer over Wi-Fi; one-sided ARP or a ",
                            "healthy ADB transport does not qualify this edge",
                        )
                    ),
                ),
            )
        )
    return checks


def lan_diagnostics_json(diagnostics: Sequence[LanNodeDiagnostics]) -> dict[str, object]:
    """Build the stable node inventory and directed matrix retained by both preflights."""
    return {
        "schema_version": 1,
        "nodes": [
            {
                "serial": diagnostic.serial,
                "origin": diagnostic.origin,
                "bssid": diagnostic.bssid,
            }
            for diagnostic in diagnostics
        ],
        "reachability_matrix": [
            edge.as_json()
            for diagnostic in diagnostics
            for edge in (diagnostic.host_to_phone, diagnostic.phone_to_peer)
        ],
    }


def _parse_node(value: str) -> tuple[str, str]:
    serial, separator, origin = value.partition("=")
    if not separator or not serial or not origin.startswith("http://"):
        msg = "--node must be SERIAL=http://PHONE_IP:PORT"
        raise argparse.ArgumentTypeError(msg)
    return serial, origin.rstrip("/")


def resolve_output_path(path: Path, workspace_directory: str | None = None) -> Path:
    """Resolve a relative report path against the invoking Bazel workspace."""
    if path.is_absolute():
        return path
    workspace = workspace_directory or os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace) / path if workspace else path


def resolve_input_path(path: Path) -> Path:
    """Resolve an absolute, Bazel-runfile, or invoking-workspace input path."""
    if path.is_absolute() or path.is_file():
        return path
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace) / path if workspace else path


class _Arguments(argparse.Namespace):
    """Typed command-line values produced by the preflight parser."""

    def __init__(self) -> None:
        """Initialize defaults that argparse replaces while parsing."""
        super().__init__()
        self.adb: Path = Path()
        self.node: list[tuple[str, str]] = []
        self.json: Path | None = None
        self.require_monitoring: bool = False
        self.require_stopped_clean: bool = False
        self.require_wireless_adb: bool = False
        self.expected_apk: Path | None = None


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("adb", type=Path, help="Bazel-provided adb executable")
    parser.add_argument(
        "--node",
        action="append",
        required=True,
        type=_parse_node,
        metavar="SERIAL=ORIGIN",
        help="phone adb serial and direct-LAN origin; specify exactly twice",
    )
    parser.add_argument("--json", type=Path, help="write redacted machine-readable evidence")
    state_group = parser.add_mutually_exclusive_group()
    state_group.add_argument("--require-monitoring", action="store_true")
    state_group.add_argument("--require-stopped-clean", action="store_true")
    parser.add_argument("--require-wireless-adb", action="store_true")
    parser.add_argument(
        "--expected-apk",
        type=Path,
        help="also require both installed monolithic APKs to match this file byte-for-byte",
    )
    return parser


def collect_pair_evidence(  # noqa: PLR0913 -- explicit collection policy boundary.
    collector: EvidenceCollector,
    nodes: Sequence[tuple[str, str]],
    *,
    require_monitoring: bool = False,
    require_stopped_clean: bool = False,
    require_wireless_adb: bool = False,
    expected_apk_sha256: str | None = None,
) -> tuple[list[NodeEvidence], list[LanNodeDiagnostics], list[Check]]:
    """Collect each phone independently and preserve all failures in one report."""
    evidence: list[NodeEvidence] = []
    checks: list[Check] = []
    futures: list[concurrent.futures.Future[NodeEvidence]] = []
    if nodes:
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=min(PAIR_NODE_COUNT, len(nodes))
        ) as pool:
            futures = [pool.submit(collector.collect, serial, origin) for serial, origin in nodes]
    for index, ((serial, origin), future) in enumerate(zip(nodes, futures, strict=True)):
        try:
            evidence.append(future.result())
            checks.append(
                Check(
                    name=f"node.{index + 1}.evidence_collection",
                    passed=True,
                    message=f"collected app, device, and direct LAN evidence from {origin}",
                )
            )
        except (  # noqa: PERF203 - retain an independent result for each concurrent phone.
            OSError,
            RuntimeError,
            TypeError,
            ValueError,
            subprocess.SubprocessError,
        ) as failure:
            checks.append(
                Check(
                    name=f"node.{index + 1}.evidence_collection",
                    passed=False,
                    message=f"{serial} at {origin}: {failure}",
                )
            )
    if len(evidence) == PAIR_NODE_COUNT:
        try:
            checks.extend(
                evaluate_pair(
                    evidence,
                    require_monitoring=require_monitoring,
                    require_stopped_clean=require_stopped_clean,
                    require_wireless_adb=require_wireless_adb,
                    expected_apk_sha256=expected_apk_sha256,
                )
            )
        except (OSError, RuntimeError, TypeError, ValueError) as failure:
            checks.append(
                Check(
                    name="evidence.evaluation",
                    passed=False,
                    message=str(failure),
                )
            )
    else:
        checks.extend(
            evaluate_pair(
                evidence,
                require_monitoring=require_monitoring,
                require_stopped_clean=require_stopped_clean,
                require_wireless_adb=require_wireless_adb,
                expected_apk_sha256=expected_apk_sha256,
            )
        )

    diagnostics: list[LanNodeDiagnostics] = []
    diagnostic_futures: list[concurrent.futures.Future[LanNodeDiagnostics]] = []
    if len(nodes) == PAIR_NODE_COUNT:
        with concurrent.futures.ThreadPoolExecutor(max_workers=PAIR_NODE_COUNT) as pool:
            diagnostic_futures = [
                pool.submit(
                    collector.collect_lan_diagnostics,
                    serial,
                    origin,
                    nodes[1 - index][1],
                )
                for index, (serial, origin) in enumerate(nodes)
            ]
    for index, (node, future) in enumerate(zip(nodes, diagnostic_futures, strict=True)):
        serial, origin = node
        peer_origin = nodes[1 - index][1]
        try:
            diagnostics.append(future.result())
        except (OSError, RuntimeError, TypeError, ValueError, subprocess.SubprocessError):
            diagnostics.append(
                LanNodeDiagnostics(
                    serial=serial,
                    origin=origin,
                    bssid=None,
                    host_to_phone=ReachabilityEvidence(
                        source="field_host",
                        target=origin,
                        transport="direct_http_clock",
                        reachable=False,
                    ),
                    phone_to_peer=ReachabilityEvidence(
                        source=origin,
                        target=peer_origin,
                        transport="adb_shell_wifi_icmp",
                        reachable=False,
                    ),
                )
            )
    checks.extend(evaluate_lan_diagnostics(diagnostics))
    return evidence, diagnostics, checks


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the read-only preflight and return nonzero for any failed check."""
    options = _parser().parse_args(arguments, namespace=_Arguments())
    if len(options.node) != PAIR_NODE_COUNT:
        _parser().error("--node must be specified exactly twice")
    expected_apk_path = (
        None if options.expected_apk is None else resolve_input_path(options.expected_apk)
    )
    if expected_apk_path is not None and not expected_apk_path.is_file():
        _parser().error("--expected-apk must name a regular file")
    expected_apk_sha256 = None if expected_apk_path is None else sha256_file(expected_apk_path)
    evidence, diagnostics, checks = collect_pair_evidence(
        EvidenceCollector(options.adb),
        options.node,
        require_monitoring=options.require_monitoring,
        require_stopped_clean=options.require_stopped_clean,
        require_wireless_adb=options.require_wireless_adb,
        expected_apk_sha256=expected_apk_sha256,
    )
    passed = all(check.passed for check in checks)
    report = {
        "schema_version": 1,
        "report_type": "android_pair_preflight",
        "passed": passed,
        "credentials_redacted": True,
        "require_monitoring": options.require_monitoring,
        "require_stopped_clean": options.require_stopped_clean,
        "lan_diagnostics": lan_diagnostics_json(diagnostics),
        "nodes": [
            {
                "serial": node.serial,
                "origin": node.origin,
                "model": node.model,
                "installed_apk_sha256": node.installed_apk_sha256,
                "node_id": node.descriptor.get("node_id"),
                "role": node.descriptor.get("role"),
                "pose_mode": _mapping(node.descriptor.get("pose"), "descriptor pose").get("mode"),
                "bssid": next(
                    (
                        diagnostic.bssid
                        for diagnostic in diagnostics
                        if diagnostic.serial == node.serial
                    ),
                    None,
                ),
                "capture_status": capture_status_diagnostics(node.status),
                "field_recording_status": field_recording_status_diagnostics(
                    node.field_recording_status
                ),
            }
            for node in evidence
        ],
        "checks": [check.as_json() for check in checks],
    }
    if expected_apk_sha256 is not None:
        report["expected_apk_sha256"] = expected_apk_sha256
    for check in checks:
        marker = "PASS" if check.passed else "FAIL"
        print(f"[{marker}] {check.name}: {check.message}")
    if options.json is not None:
        output_path = resolve_output_path(options.json)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
