"""Hermetic policy tests for the Android pair preflight."""

from __future__ import annotations

import copy
import dataclasses
import json
import os
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import TYPE_CHECKING, cast, final
from urllib.parse import urlsplit

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

from tools.android_pair_doctor import (
    BrowserCorsEvidence,
    Check,
    DeviceTelemetry,
    EvidenceCollector,
    HostedWebAsset,
    LanNodeDiagnostics,
    NodeEvidence,
    ReachabilityEvidence,
    capture_status_diagnostics,
    collect_pair_evidence,
    evaluate_lan_diagnostics,
    evaluate_pair,
    field_recording_status_diagnostics,
    lan_diagnostics_json,
    parse_control_token,
    parse_telemetry,
    parse_wifi_bssid,
    resolve_input_path,
    resolve_output_path,
)


@dataclasses.dataclass(frozen=True)
class _NodeSpec:
    serial: str
    origin: str
    node_id: str
    role: str
    mode: str


def _mutable_mapping(value: object) -> dict[str, object]:
    assert isinstance(value, dict)
    return cast("dict[str, object]", value)


class _FakeCollectionBackends:
    def __init__(
        self,
        fixture: NodeEvidence,
        token: str,
        *,
        peer_ping_reachable: bool = True,
        route_probe_succeeds: bool = True,
        route_output: str | None = None,
    ) -> None:
        self.fixture: NodeEvidence = fixture
        self.token: str = token
        self.peer_ping_reachable: bool = peer_ping_reachable
        self.route_probe_succeeds: bool = route_probe_succeeds
        self.route_output: str | None = route_output
        self.observed_http_tokens: list[str | None] = []
        self.observed_asset_urls: list[str] = []
        self.observed_cors_urls: list[str] = []
        self.observed_operations: list[str] = []
        self.observed_adb_commands: list[tuple[str, ...]] = []

    def run(self, arguments: Sequence[str]) -> str:
        self.observed_adb_commands.append(tuple(arguments))
        joined = " ".join(arguments)
        if " ip route get " in f" {joined} ":
            self.observed_operations.append("adb:peer_route")
            if not self.route_probe_succeeds:
                raise subprocess.CalledProcessError(1, arguments)
            if self.route_output is not None:
                return self.route_output
            source = self.fixture.origin.split("://", maxsplit=1)[1].split(":", maxsplit=1)[0]
            peer = arguments[-1]
            return f"{peer} via 10.0.0.1 dev wlan0 table 1020 src {source} uid 2000\n    cache"
        if " ping " in f" {joined} ":
            self.observed_operations.append("adb:peer_ping")
            if not self.peer_ping_reachable:
                raise subprocess.CalledProcessError(1, arguments)
            return "1 packets transmitted, 1 received, 0% packet loss"
        responses = (
            ("get-state", "device"),
            ("ro.product.model", "Pixel 6"),
            (
                "cmd wifi status",
                'WifiInfo: SSID: "field", BSSID: 44:d9:e7:aa:bb:cc, RSSI: -48',
            ),
            ("pm path", "package:/data/app/~~token/app-token/base.apk"),
            ("sha256sum", f"{'a' * 64}  /data/app/~~token/app-token/base.apk"),
            (
                "node_configuration.xml",
                f'<map><string name="control_token">{self.token}</string></map>',
            ),
            ("dumpsys battery", "level: 80\nscale: 100\ntemperature: 300\nvoltage: 4200"),
            ("dumpsys thermalservice", "Thermal Status: 0"),
            ("%a", "2097152"),
            ("%S", "4096"),
        )
        response = next((value for needle, value in responses if needle in joined), None)
        if response is None:
            message = f"unexpected adb command: {joined}"
            raise AssertionError(message)
        operation = next(needle for needle, _ in responses if needle in joined)
        self.observed_operations.append(f"adb:{operation}")
        return response

    def read(self, url: str, credential: str | None) -> tuple[int, Mapping[str, object]]:
        self.observed_http_tokens.append(credential)
        path = urlsplit(url).path
        endpoint = (
            "field-recording-status"
            if path.endswith("/field-recording/status")
            else path.rsplit("/", maxsplit=1)[-1]
        )
        self.observed_operations.append(
            f"http:{endpoint}:{'authenticated' if credential is not None else 'anonymous'}"
        )
        responses: Mapping[str, Mapping[str, object]] = {
            "node": self.fixture.descriptor,
            "status": self.fixture.status,
            "field-recording-status": self.fixture.field_recording_status,
            "clock": self.fixture.clock,
        }
        if endpoint == "node" and credential != self.token:
            return 401, {"error": "authentication required"}
        if endpoint in responses:
            return 200, responses[endpoint]
        if endpoint == "setup" and credential == self.token:
            return 200, self.fixture.setup
        if endpoint == "setup" and credential is None:
            return 401, {"error": "authentication required"}
        message = f"unexpected HTTP request: {url}"
        raise AssertionError(message)

    def read_asset(self, url: str, required_markers: Sequence[bytes]) -> HostedWebAsset:
        self.observed_asset_urls.append(url)
        path = urlsplit(url).path
        content_type, cache_control, body_bytes = {
            "/": ("text/html; charset=utf-8", "no-store", 400),
            "/app.js": ("text/javascript; charset=utf-8", "no-cache", 350_000),
            "/app.css": ("text/css; charset=utf-8", "no-cache", 31_000),
        }[path]
        return HostedWebAsset(
            path=path,
            status=200,
            direct_response=True,
            content_type=content_type,
            cache_control=cache_control,
            nosniff=True,
            body_bytes=body_bytes,
            required_markers_present=bool(required_markers),
        )

    def read_cors(self, url: str) -> BrowserCorsEvidence:
        self.observed_cors_urls.append(url)
        return _nominal_browser_cors()


@final
class _SelectiveCollector(EvidenceCollector):
    def __init__(self, responses: Mapping[str, NodeEvidence | Exception]) -> None:
        super().__init__(Path())
        self._responses: Mapping[str, NodeEvidence | Exception] = responses

    def collect(  # pyright: ignore[reportImplicitOverride]
        self, serial: str, origin: str
    ) -> NodeEvidence:
        del origin
        response = self._responses[serial]
        if isinstance(response, Exception):
            raise response
        return response

    def collect_lan_diagnostics(  # pyright: ignore[reportImplicitOverride]
        self, serial: str, origin: str, peer_origin: str
    ) -> LanNodeDiagnostics:
        return LanNodeDiagnostics(
            serial=serial,
            origin=origin,
            bssid="44:d9:e7:aa:bb:cc",
            host_to_phone=ReachabilityEvidence(
                source="field_host",
                target=origin,
                transport="direct_http_clock",
                reachable=True,
                http_status=200,
            ),
            phone_to_peer=ReachabilityEvidence(
                source=origin,
                target=peer_origin,
                transport="adb_shell_wifi_icmp",
                reachable=True,
            ),
        )


def _nominal_web_assets() -> tuple[HostedWebAsset, ...]:
    return (
        HostedWebAsset(
            path="/",
            status=200,
            direct_response=True,
            content_type="text/html; charset=utf-8",
            cache_control="no-store",
            nosniff=True,
            body_bytes=400,
            required_markers_present=True,
        ),
        HostedWebAsset(
            path="/app.js",
            status=200,
            direct_response=True,
            content_type="text/javascript; charset=utf-8",
            cache_control="no-cache",
            nosniff=True,
            body_bytes=350_000,
            required_markers_present=True,
        ),
        HostedWebAsset(
            path="/app.css",
            status=200,
            direct_response=True,
            content_type="text/css; charset=utf-8",
            cache_control="no-cache",
            nosniff=True,
            body_bytes=31_000,
            required_markers_present=True,
        ),
    )


def _nominal_browser_cors() -> BrowserCorsEvidence:
    return BrowserCorsEvidence(
        status=204,
        direct_response=True,
        allow_origin="*",
        allow_methods="GET, HEAD, POST, PUT, OPTIONS",
        allow_headers="Authorization, Range, Content-Type",
    )


def _network_direction() -> dict[str, object]:
    return {
        "schema_version": 1,
        "attempts": 3,
        "successes": 3,
        "timeouts": 0,
        "round_trip_ns": ["10000000", "12000000", "14000000"],
        "transfer_bytes": "1000000",
        "transfer_duration_ns": "100000000",
        "transfer_complete": True,
        "minimum_round_trip_ns": "10000000",
        "median_round_trip_ns": "12000000",
        "p95_round_trip_ns": "14000000",
        "maximum_round_trip_ns": "14000000",
        "jitter_ns": "4000000",
        "transfer_bits_per_second": 80000000.0,
    }


def _network_health(mode: str, peer: NodeEvidence | None) -> dict[str, object]:
    if mode != "leader":
        return {
            "schema_version": 1,
            "configured": False,
            "state": "unusable",
            "raw_state": "unusable",
            "measured": False,
            "stale": False,
            "transition_pending": False,
            "age_ns": "0",
            "issues": ["A peer is not configured for network-health measurement."],
            "peer": None,
            "measured_at_elapsed_realtime_ns": None,
            "local_to_peer": None,
            "peer_to_local": None,
        }
    assert peer is not None
    return {
        "schema_version": 1,
        "configured": True,
        "state": "good",
        "raw_state": "good",
        "measured": True,
        "stale": False,
        "transition_pending": False,
        "age_ns": "1000000000",
        "issues": [],
        "peer": {"origin": peer.origin, "node_id": peer.descriptor["node_id"]},
        "measured_at_elapsed_realtime_ns": "9000000000",
        "local_to_peer": _network_direction(),
        "peer_to_local": _network_direction(),
    }


def _node(
    specification: _NodeSpec,
    peer: NodeEvidence | None = None,
) -> NodeEvidence:
    serial = specification.serial
    origin = specification.origin
    node_id = specification.node_id
    role = specification.role
    mode = specification.mode
    peer_origin = None if peer is None else {"origin": peer.origin}
    pairing = (
        None
        if peer is None
        else {
            "state": "active",
            "peer_node_id": peer.descriptor["node_id"],
            "expected_role": peer.descriptor["role"],
            "origin": peer.origin,
            "credential_generation": 1,
            "credential_status": "verified",
        }
    )
    descriptor = {
        "schema_version": 1,
        "node_id": node_id,
        "role": role,
        "capture_profile": "720p240",
        "control_authentication": "bearer",
        "service_urls": [origin],
        "pose": {"mode": mode},
    }
    return NodeEvidence(
        serial=serial,
        origin=origin,
        model="Pixel",
        adb_transport="usb",
        installed_apk_sha256="a" * 64,
        descriptor=descriptor,
        setup={
            "schema_version": 1,
            "node": {"node_id": node_id},
            "configuration": {
                "role": role,
                "capture_profile": "720p240",
                "pose": {
                    "mode": mode,
                    "inference_delegate": "gpu_preferred",
                    "peer": peer_origin,
                },
            },
            "pairing": pairing,
            "readiness": {"issues": [], "capture_state": "stopped"},
            "device_admission": {
                "schema_version": 1,
                "profile": "720p240",
                "ready": True,
                "probe_succeeded": True,
                "probe_diagnostic": None,
                "api_level": 36,
                "issues": [],
            },
            "reboot_recovery": {
                "schema_version": 1,
                "mode": "operator_launch_after_first_unlock",
                "direct_boot_aware": False,
                "boot_receiver_registered": True,
                "automatic_capture_before_first_unlock": False,
                "operator_foreground_launch_required_after_os_reboot": True,
                "process_restart_policy": "android_start_sticky_best_effort",
                "user_unlocked": True,
                "service_running": True,
                "ready_this_boot": True,
                "issues": [],
            },
        },
        status={
            "schema_version": 2,
            "state": "stopped",
            "armed": False,
            "active_session_id": None,
            "pose": {
                "autonomous_pair": {
                    "state": "stopped",
                    "active_shared_session_id": None,
                    "replication_backlog_size": 0,
                    "local_triggered": False,
                    "peer_triggered": False,
                    "local_published": False,
                    "peer_published": False,
                }
            },
            "operational_health": {
                "thermal": {"status": 0, "headroom": 0.5},
                "storage": {"usable_bytes": 8 * 1024**3, "ready": True},
            },
            "pair_network_health": _network_health(mode, peer),
        },
        field_recording_status={
            "schema_version": 1,
            "state": "idle",
            "active_recording_id": None,
            "shared_recording_id": None,
            "started_at_utc": None,
            "started_elapsed_realtime_ns": None,
            "elapsed_ms": 0,
            "video_bytes": "0",
            "audio_frames": "0",
            "max_duration_seconds": 600,
            "error": "",
            "hil_reject_next_start": False,
            "hil_fail_next_accepted_start": False,
            "hil_accepted_start_waiting": False,
        },
        clock={"schema_version": 1, "node_id": node_id},
        unauthenticated_setup_status=401,
        web_assets=_nominal_web_assets(),
        browser_cors=_nominal_browser_cors(),
        telemetry=DeviceTelemetry(80, 29.5, 4200, 0, 8 * 1024**3),
    )


def _nominal() -> list[NodeEvidence]:
    shadow = _node(
        _NodeSpec(
            serial="1A011JEG501717",
            origin="http://10.0.0.2:8088",
            node_id="shadow-node",
            role="down_the_line",
            mode="shadow",
        )
    )
    leader = _node(
        _NodeSpec(
            serial="22181FDF6005QH",
            origin="http://10.0.0.3:8088",
            node_id="leader-node",
            role="face_on",
            mode="leader",
        ),
        peer=shadow,
    )
    return [leader, shadow]


def _passed(nodes: list[NodeEvidence]) -> bool:
    return all(check.passed for check in evaluate_pair(nodes))


def _monitoring(nodes: list[NodeEvidence]) -> list[NodeEvidence]:
    result: list[NodeEvidence] = []
    shadow_id = str(nodes[1].descriptor["node_id"])
    for node in nodes:
        mode = str(_mutable_mapping(node.descriptor["pose"])["mode"])
        pose = {
            "mode": mode,
            "configured_delegate": "gpu_preferred",
            "experiment_enabled": False,
            "model_variant": "lite",
            "model_asset_path": "pose_landmarker_lite.task",
            "actual_standby_width": 640,
            "actual_standby_height": 360,
            "phase": "monitoring",
            "metrics": {
                "delegate": "gpu",
                "successful_inferences": 120,
                "failed_inferences": 0,
                "inference_duration_p95_ns": "180000000",
                "maximum_inference_duration_ns": "220000000",
                "recent_inference_sample_count": 100,
                "recent_inference_duration_p95_ns": "180000000",
                "recent_maximum_inference_duration_ns": "220000000",
                "recent_inference_deadline_misses": 3,
                "rejected_decision_timestamps": 0,
            },
            "autonomous_pair": {
                "state": "monitoring" if mode == "leader" else "stopped",
                "peer_available": mode == "leader",
            },
            "peer_clock": (
                {
                    "peer_node_id": shadow_id,
                    "uncertainty_ns": "20000000",
                    "age_ns": "2000000000",
                }
                if mode == "leader"
                else None
            ),
            "standby_audio": {
                "ready": True,
                "end_frame_position": "4800",
                "last_error": "",
            },
        }
        result.append(
            dataclass_replace(
                node,
                status={
                    **node.status,
                    "schema_version": 2,
                    "state": "armed",
                    "armed": True,
                    "pose": pose,
                },
            )
        )
    return result


def test_nominal_pair_is_ready() -> None:
    """A complementary authenticated pair passes the default preflight."""
    assert _passed(_nominal())


def test_stopped_clean_gate_rejects_capture_and_autonomous_publication_work() -> None:
    """Pre-session admission fails closed on every active or dirty terminal dimension."""
    nodes = _nominal()
    assert all(check.passed for check in evaluate_pair(nodes, require_stopped_clean=True))
    assert all(check.passed for check in evaluate_pair(_monitoring(nodes)))
    mutual_exclusion_raised = False
    try:
        evaluate_pair(nodes, require_monitoring=True, require_stopped_clean=True)
    except ValueError:
        mutual_exclusion_raised = True
    assert mutual_exclusion_raised

    def stopped_clean_check(status: dict[str, object]) -> Check:
        checks = evaluate_pair(
            [dataclass_replace(nodes[0], status=status), nodes[1]],
            require_stopped_clean=True,
        )
        return next(check for check in checks if check.name == "node.1.stopped_clean")

    variants: list[tuple[str, dict[str, object]]] = []
    for state in ("arming", "armed", "waiting_post_roll", "encoding", "stopping"):
        status = _mutable_mapping(copy.deepcopy(nodes[0].status))
        status["state"] = state
        status["armed"] = state == "armed"
        variants.append((state, status))
    active_capture = _mutable_mapping(copy.deepcopy(nodes[0].status))
    active_capture["active_session_id"] = "active-session"
    variants.append(("active capture session", active_capture))
    missing_active_capture = _mutable_mapping(copy.deepcopy(nodes[0].status))
    del missing_active_capture["active_session_id"]
    variants.append(("missing active capture session evidence", missing_active_capture))
    for field, value in (
        ("replication_backlog_size", 1),
        ("active_shared_session_id", "shared-session"),
        ("state", "publishing"),
        ("local_triggered", True),
        ("peer_triggered", True),
        ("local_published", True),
        ("peer_published", True),
    ):
        status = _mutable_mapping(copy.deepcopy(nodes[0].status))
        autonomous = _mutable_mapping(_mutable_mapping(status["pose"])["autonomous_pair"])
        autonomous[field] = value
        variants.append((field, status))
    missing_shared_session = _mutable_mapping(copy.deepcopy(nodes[0].status))
    del _mutable_mapping(_mutable_mapping(missing_shared_session["pose"])["autonomous_pair"])[
        "active_shared_session_id"
    ]
    variants.append(("missing active shared session evidence", missing_shared_session))
    for label, status in variants:
        check = stopped_clean_check(status)
        assert not check.passed, label
        assert "replication_backlog_size" in check.message, label


def test_stopped_clean_gate_rejects_field_recording_and_malformed_evidence() -> None:
    """Separate recorder activity, publication, faults, and missing keys all fail closed."""
    nodes = _nominal()

    def checks_for(status: Mapping[str, object]) -> tuple[Check, Check]:
        checks = evaluate_pair(
            [dataclass_replace(nodes[0], field_recording_status=status), nodes[1]],
            require_stopped_clean=True,
        )
        by_name = {check.name: check for check in checks}
        return (
            by_name["node.1.field_recording_contract"],
            by_name["node.1.field_recording_stopped_clean"],
        )

    for state in ("starting", "recording", "stopping"):
        status = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
        status.update(
            {
                "state": state,
                "active_recording_id": "recording-local",
                "shared_recording_id": "recording-shared",
                "started_at_utc": "2026-08-23T18:00:00Z",
                "started_elapsed_realtime_ns": "1000000",
            }
        )
        contract, clean = checks_for(status)
        assert contract.passed, state
        assert not clean.passed, state

    ready = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
    ready.update(
        {
            "state": "ready",
            "shared_recording_id": "completed-shared",
            "started_at_utc": "2026-08-23T18:00:00Z",
            "started_elapsed_realtime_ns": "1000000",
            "elapsed_ms": 42_000,
            "video_bytes": "12345678",
            "audio_frames": "2016000",
        }
    )
    contract, clean = checks_for(ready)
    assert contract.passed
    assert clean.passed

    failed = _mutable_mapping(copy.deepcopy(ready))
    failed["state"] = "error"
    failed["error"] = "encoder failed"
    contract, clean = checks_for(failed)
    assert contract.passed
    assert not clean.passed

    for fault in (
        "hil_reject_next_start",
        "hil_fail_next_accepted_start",
    ):
        status = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
        status[fault] = True
        contract, clean = checks_for(status)
        assert contract.passed, fault
        assert not clean.passed, fault

    waiting = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
    waiting.update(
        {
            "state": "starting",
            "active_recording_id": "recording-local",
            "shared_recording_id": "recording-shared",
            "started_at_utc": "2026-08-23T18:00:00Z",
            "started_elapsed_realtime_ns": "1000000",
            "hil_fail_next_accepted_start": True,
            "hil_accepted_start_waiting": True,
        }
    )
    contract, clean = checks_for(waiting)
    assert contract.passed
    assert not clean.passed

    for missing_key in nodes[0].field_recording_status:
        malformed = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
        del malformed[missing_key]
        contract, clean = checks_for(malformed)
        assert not contract.passed, missing_key
        assert not clean.passed, missing_key

    malformed_schema = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
    malformed_schema["schema_version"] = True
    contract, clean = checks_for(malformed_schema)
    assert not contract.passed
    assert not clean.passed
    malformed_state = _mutable_mapping(copy.deepcopy(nodes[0].field_recording_status))
    malformed_state["state"] = []
    contract, clean = checks_for(malformed_state)
    assert not contract.passed
    assert not clean.passed


def test_capture_status_diagnostics_retains_metrics_and_redacts_credentials() -> None:
    """Every attempt keeps numeric margins without allowing a credential into artifacts."""
    secret = "A" * 32
    redacted = "[REDACTED]"
    expected_inference_samples = 150
    expected_noise_floor = 0.001
    status = _mutable_mapping(copy.deepcopy(_nominal()[0].status))
    pose = _mutable_mapping(status["pose"])
    pose["metrics"] = {
        "recent_inference_sample_count": expected_inference_samples,
        "recent_inference_duration_p95_ns": "187000000",
    }
    pose["standby_audio"] = {
        "peak_amplitude": 0.25,
        "noise_floor": expected_noise_floor,
        "threshold": 0.015,
    }
    pose["control_token"] = secret
    status["error"] = f"request used Bearer {secret}"

    retained = capture_status_diagnostics(status)

    retained_pose = _mutable_mapping(retained["pose"])
    assert retained_pose["control_token"] == redacted
    assert redacted in str(retained["error"])
    assert secret not in json.dumps(retained)
    assert (
        _mutable_mapping(retained_pose["metrics"])["recent_inference_sample_count"]
        == expected_inference_samples
    )
    assert _mutable_mapping(retained_pose["standby_audio"])["noise_floor"] == expected_noise_floor
    health = _mutable_mapping(retained["pair_network_health"])
    assert _mutable_mapping(health["local_to_peer"])["p95_round_trip_ns"] == "14000000"

    field_status = _mutable_mapping(copy.deepcopy(_nominal()[0].field_recording_status))
    field_status["error"] = f"request used Bearer {secret}"
    retained_field_status = field_recording_status_diagnostics(field_status)
    assert retained_field_status["elapsed_ms"] == 0
    assert secret not in json.dumps(retained_field_status)


def test_pair_network_health_nominal_and_degraded_are_viable_in_both_modes() -> None:
    """Degraded service remains usable but cannot lose its explicit operator warning."""
    for require_monitoring in (False, True):
        nodes = _monitoring(_nominal()) if require_monitoring else _nominal()
        nominal_checks = evaluate_pair(nodes, require_monitoring=require_monitoring)
        nominal_health = next(
            check for check in nominal_checks if check.name == "pair.network_health"
        )
        assert nominal_health.passed
        assert "good" in nominal_health.message

        leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
        health = _mutable_mapping(leader_status["pair_network_health"])
        health["state"] = "degraded"
        health["raw_state"] = "degraded"
        health["issues"] = ["Some phone-to-phone application requests failed or timed out."]
        degraded_checks = evaluate_pair(
            [dataclass_replace(nodes[0], status=leader_status), nodes[1]],
            require_monitoring=require_monitoring,
        )
        degraded_health = next(
            check for check in degraded_checks if check.name == "pair.network_health"
        )
        assert degraded_health.passed
        assert "WARNING: degraded" in degraded_health.message
        assert "explicit operator override" in degraded_health.message


def test_pair_network_health_unusable_and_stale_fail_in_both_modes() -> None:
    """Neither an overrideable warning nor armed flags can hide nonviable coordination."""
    for require_monitoring in (False, True):
        for stale in (False, True):
            nodes = _monitoring(_nominal()) if require_monitoring else _nominal()
            leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
            health = _mutable_mapping(leader_status["pair_network_health"])
            health["state"] = "unusable"
            health["raw_state"] = "unusable"
            health["stale"] = stale
            health["age_ns"] = "31000000000" if stale else "1000000000"
            health["issues"] = [
                "Pair network-health evidence is stale."
                if stale
                else "At least one phone-to-phone application direction is unreachable."
            ]
            checks = evaluate_pair(
                [dataclass_replace(nodes[0], status=leader_status), nodes[1]],
                require_monitoring=require_monitoring,
            )
            failed = {check.name for check in checks if not check.passed}
            assert "pair.network_health" in failed
            assert "node.1.pair_network_health_contract" not in failed


def test_pair_network_health_unmeasured_leader_fails_viability() -> None:
    """A configured leader cannot arm from an internally valid but unmeasured snapshot."""
    nodes = _nominal()
    leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    health = _mutable_mapping(leader_status["pair_network_health"])
    health.update(
        {
            "state": "unusable",
            "raw_state": "unusable",
            "measured": False,
            "stale": False,
            "transition_pending": False,
            "age_ns": "0",
            "issues": ["Pair network health has not been measured."],
            "measured_at_elapsed_realtime_ns": None,
            "local_to_peer": None,
            "peer_to_local": None,
        }
    )
    checks = evaluate_pair([dataclass_replace(nodes[0], status=leader_status), nodes[1]])
    failed = {check.name for check in checks if not check.passed}
    assert "pair.network_health" in failed
    assert "node.1.pair_network_health_contract" not in failed


def test_pair_network_health_requires_exact_authenticated_shadow() -> None:
    """Fresh good metrics for another peer cannot qualify the configured pair."""
    nodes = _nominal()
    leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    health = _mutable_mapping(leader_status["pair_network_health"])
    peer = _mutable_mapping(health["peer"])
    peer["node_id"] = "replaced-shadow"
    checks = evaluate_pair([dataclass_replace(nodes[0], status=leader_status), nodes[1]])
    failed = {check.name for check in checks if not check.passed}
    assert "pair.network_health" in failed
    assert "node.1.pair_network_health_contract" not in failed


def test_pair_network_health_malformed_contract_fails_without_aborting_diagnosis() -> None:
    """Type drift is retained as named failures while unrelated pair checks still run."""
    nodes = _nominal()
    leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    health = _mutable_mapping(leader_status["pair_network_health"])
    health["age_ns"] = 1000
    local_direction = _mutable_mapping(health["local_to_peer"])
    local_direction["successes"] = 4
    checks = evaluate_pair([dataclass_replace(nodes[0], status=leader_status), nodes[1]])
    failed = {check.name for check in checks if not check.passed}
    assert "node.1.pair_network_health_contract" in failed
    assert "pair.network_health" in failed
    assert "pair.roles" not in failed


def test_shadow_network_health_must_remain_unconfigured() -> None:
    """The intentionally unconfigured shadow cannot masquerade as another measuring leader."""
    nodes = _nominal()
    shadow_status = _mutable_mapping(copy.deepcopy(nodes[1].status))
    shadow_status["pair_network_health"] = copy.deepcopy(nodes[0].status["pair_network_health"])
    checks = evaluate_pair([nodes[0], dataclass_replace(nodes[1], status=shadow_status)])
    failed = {check.name for check in checks if not check.passed}
    assert "node.2.pair_network_health_contract" in failed
    assert "node.1.pair_network_health_contract" not in failed
    assert "pair.network_health" not in failed


def test_disabled_network_health_uses_the_unconfigured_contract() -> None:
    """A disabled standalone node does not become a network-health measurement owner."""
    nodes = _nominal()
    descriptor = _mutable_mapping(copy.deepcopy(nodes[0].descriptor))
    descriptor_pose = _mutable_mapping(descriptor["pose"])
    descriptor_pose["mode"] = "disabled"
    setup = _mutable_mapping(copy.deepcopy(nodes[0].setup))
    setup_pose = _mutable_mapping(_mutable_mapping(setup["configuration"])["pose"])
    setup_pose["mode"] = "disabled"
    status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    status["pair_network_health"] = _network_health("disabled", None)
    checks = evaluate_pair(
        [
            dataclass_replace(nodes[0], descriptor=descriptor, setup=setup, status=status),
            nodes[1],
        ]
    )
    failed = {check.name for check in checks if not check.passed}
    assert "node.1.pair_network_health_contract" not in failed
    assert "pair.pose_topology" in failed


def test_wrong_role_and_duplicate_identity_fail() -> None:
    """Two aliases for one installation or one role fail closed."""
    nodes = _nominal()
    duplicate = dataclass_replace(
        nodes[1],
        descriptor={**nodes[1].descriptor, "node_id": "leader-node", "role": "face_on"},
    )
    checks = evaluate_pair([nodes[0], duplicate])
    failed = {check.name for check in checks if not check.passed}
    assert "pair.unique_identity" in failed
    assert "pair.roles" in failed


def test_two_shadow_phones_fail_before_field_use() -> None:
    """A role-complementary pair is still unusable when neither phone owns leader duties."""
    nodes = _nominal()
    descriptor = _mutable_mapping(copy.deepcopy(nodes[0].descriptor))
    setup = _mutable_mapping(copy.deepcopy(nodes[0].setup))
    descriptor_pose = _mutable_mapping(descriptor["pose"])
    configuration = _mutable_mapping(setup["configuration"])
    setup_pose = _mutable_mapping(configuration["pose"])
    descriptor_pose["mode"] = "shadow"
    setup_pose["mode"] = "shadow"
    checks = evaluate_pair(
        [dataclass_replace(nodes[0], descriptor=descriptor, setup=setup), nodes[1]]
    )
    failed = {check.name for check in checks if not check.passed}
    assert "pair.pose_topology" in failed
    assert "pair.authenticated_binding" in failed


def test_stale_binding_and_readiness_issue_fail() -> None:
    """Credential drift is visible in both binding and readiness checks."""
    nodes = _nominal()
    setup = _mutable_mapping(copy.deepcopy(nodes[0].setup))
    pairing = _mutable_mapping(setup["pairing"])
    readiness = _mutable_mapping(setup["readiness"])
    pairing["credential_status"] = "authentication_failed"
    readiness["issues"] = ["Peer identity could not be authenticated."]
    checks = evaluate_pair([dataclass_replace(nodes[0], setup=setup), nodes[1]])
    failed = {check.name for check in checks if not check.passed}
    assert "node.1.readiness" in failed
    assert "pair.authenticated_binding" in failed


def test_product_floor_and_current_boot_must_be_explicitly_ready() -> None:
    """Generic setup readiness cannot hide a failed probe or forgotten post-reboot launch."""
    nodes = _nominal()
    setup = _mutable_mapping(copy.deepcopy(nodes[1].setup))
    admission = _mutable_mapping(setup["device_admission"])
    reboot = _mutable_mapping(setup["reboot_recovery"])
    admission["probe_succeeded"] = False
    admission["ready"] = False
    admission["issues"] = [{"code": "probe_failed"}]
    reboot["service_running"] = False
    reboot["ready_this_boot"] = False
    reboot["issues"] = ["Open Swing Capture once after unlock to start the node service."]
    checks = evaluate_pair([nodes[0], dataclass_replace(nodes[1], setup=setup)])
    failed = {check.name for check in checks if not check.passed}
    assert {"node.2.device_admission", "node.2.current_boot"}.issubset(failed)


def test_older_apk_contract_is_reported_per_phone_without_aborting_pair_diagnosis() -> None:
    """A stale installed APK produces actionable failures for both nodes, not one parser error."""
    stale_nodes: list[NodeEvidence] = []
    for node in _nominal():
        setup = _mutable_mapping(copy.deepcopy(node.setup))
        del setup["device_admission"]
        del setup["reboot_recovery"]
        stale_nodes.append(dataclass_replace(node, setup=setup))
    checks = evaluate_pair(stale_nodes)
    failed = {check.name for check in checks if not check.passed}
    assert {
        "node.1.device_admission",
        "node.1.current_boot",
        "node.2.device_admission",
        "node.2.current_boot",
    }.issubset(failed)


def test_resource_and_optional_monitoring_gates_fail() -> None:
    """Low resources and stopped stations fail the strict field gate."""
    nodes = _nominal()
    depleted = dataclass_replace(
        nodes[1],
        telemetry=DeviceTelemetry(10, 44, 3800, 3, 100),
    )
    checks = evaluate_pair([nodes[0], depleted], require_monitoring=True)
    failed = {check.name for check in checks if not check.passed}
    assert "node.1.monitoring" in failed
    assert "node.2.monitoring" in failed
    assert "node.2.resources" in failed


def test_monitoring_gate_allows_only_the_expected_edit_lock() -> None:
    """An armed station is ready even though setup editing is intentionally locked."""
    armed: list[NodeEvidence] = []
    for node in _monitoring(_nominal()):
        setup = _mutable_mapping(copy.deepcopy(node.setup))
        readiness = _mutable_mapping(setup["readiness"])
        readiness["issues"] = ["Stop capture before editing phone setup."]
        armed.append(
            dataclass_replace(
                node,
                setup=setup,
            )
        )
    assert all(check.passed for check in evaluate_pair(armed, require_monitoring=True))


def test_monitoring_gate_rejects_hidden_pair_audio_and_inference_failures() -> None:
    """Armed flags cannot hide a disconnected peer, stale clock, audio fault, or idle pose path."""
    nodes = _monitoring(_nominal())
    leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    leader_pose = _mutable_mapping(leader_status["pose"])
    autonomous_pair = _mutable_mapping(leader_pose["autonomous_pair"])
    peer_clock = _mutable_mapping(leader_pose["peer_clock"])
    standby_audio = _mutable_mapping(leader_pose["standby_audio"])
    autonomous_pair["peer_available"] = False
    peer_clock["age_ns"] = "10000000001"
    standby_audio["ready"] = False
    standby_audio["last_error"] = "audio recorder stopped"

    shadow_status = _mutable_mapping(copy.deepcopy(nodes[1].status))
    shadow_pose = _mutable_mapping(shadow_status["pose"])
    shadow_metrics = _mutable_mapping(shadow_pose["metrics"])
    shadow_audio = _mutable_mapping(shadow_pose["standby_audio"])
    shadow_metrics["successful_inferences"] = 0
    shadow_audio["ready"] = False
    checks = evaluate_pair(
        [
            dataclass_replace(nodes[0], status=leader_status),
            dataclass_replace(nodes[1], status=shadow_status),
        ],
        require_monitoring=True,
    )
    failed = {check.name for check in checks if not check.passed}
    assert {
        "node.2.pose_monitoring",
        "node.2.standby_audio",
        "pair.peer_reachable",
        "pair.peer_clock",
        "pair.standby_audio",
    }.issubset(failed)


def test_monitoring_gate_rejects_pose_latency_regression() -> None:
    """A station cannot look field-ready while current pose latency misses its policy."""
    nodes = _monitoring(_nominal())
    status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    pose = _mutable_mapping(status["pose"])
    metrics = _mutable_mapping(pose["metrics"])
    metrics["recent_inference_duration_p95_ns"] = "714000000"
    metrics["recent_maximum_inference_duration_ns"] = "714000000"

    checks = evaluate_pair(
        [dataclass_replace(nodes[0], status=status), nodes[1]],
        require_monitoring=True,
    )
    assert "node.1.pose_monitoring" in {check.name for check in checks if not check.passed}


def test_monitoring_gate_uses_recent_window_instead_of_diluted_lifetime() -> None:
    """Old lifetime tails remain visible but do not replace the representative recent window."""
    nodes = _monitoring(_nominal())
    status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    metrics = _mutable_mapping(_mutable_mapping(status["pose"])["metrics"])
    metrics["inference_duration_p95_ns"] = "714000000"
    metrics["maximum_inference_duration_ns"] = "714000000"

    checks = evaluate_pair(
        [dataclass_replace(nodes[0], status=status), nodes[1]],
        require_monitoring=True,
    )
    assert "node.1.pose_monitoring" not in {check.name for check in checks if not check.passed}


def test_monitoring_gate_rejects_runtime_drift_and_short_recent_window() -> None:
    """Configured preference cannot hide fallback, experiment leakage, or a tiny sample."""
    nodes = _monitoring(_nominal())
    mutations = (
        ("metrics", "delegate", "cpu"),
        ("pose", "model_variant", "full"),
        ("pose", "actual_standby_width", 1280),
        ("metrics", "recent_inference_sample_count", 99),
        ("metrics", "recent_inference_sample_count", 151),
    )
    for section, field, value in mutations:
        status = _mutable_mapping(copy.deepcopy(nodes[0].status))
        pose = _mutable_mapping(status["pose"])
        target = _mutable_mapping(pose["metrics"]) if section == "metrics" else pose
        target[field] = value
        checks = evaluate_pair(
            [dataclass_replace(nodes[0], status=status), nodes[1]],
            require_monitoring=True,
        )
        assert "node.1.pose_monitoring" in {check.name for check in checks if not check.passed}, (
            field
        )


def test_monitoring_gate_rejects_wrong_peer_clock_identity_and_uncertainty() -> None:
    """A clock snapshot for another peer or outside policy cannot qualify field monitoring."""
    nodes = _monitoring(_nominal())
    leader_status = _mutable_mapping(copy.deepcopy(nodes[0].status))
    pose = _mutable_mapping(leader_status["pose"])
    peer_clock = _mutable_mapping(pose["peer_clock"])
    peer_clock["peer_node_id"] = "replaced-shadow"
    peer_clock["uncertainty_ns"] = "25000001"
    checks = evaluate_pair(
        [dataclass_replace(nodes[0], status=leader_status), nodes[1]],
        require_monitoring=True,
    )
    assert "pair.peer_clock" in {check.name for check in checks if not check.passed}


def test_wireless_adb_gate_distinguishes_usb() -> None:
    """The optional field gate rejects USB-only transports."""
    checks = evaluate_pair(_nominal(), require_wireless_adb=True)
    assert {check.name for check in checks if not check.passed} == {
        "node.1.adb_transport",
        "node.2.adb_transport",
    }


def test_expected_apk_gate_rejects_only_the_stale_install() -> None:
    """A schema-compatible stale deployment cannot enter a field session unnoticed."""
    nodes = _nominal()
    stale = dataclass_replace(nodes[1], installed_apk_sha256="b" * 64)
    checks = evaluate_pair([nodes[0], stale], expected_apk_sha256="a" * 64)
    failed = {check.name for check in checks if not check.passed}
    assert failed == {"node.2.exact_apk"}


def test_phone_hosted_web_bundle_must_be_complete_and_direct() -> None:
    """A reachable JSON API cannot hide a missing, redirected, or stale browser bundle."""
    nodes = _nominal()
    broken_assets = list(nodes[0].web_assets)
    broken_assets[1] = dataclasses.replace(
        broken_assets[1],
        direct_response=False,
        content_type="text/html; charset=utf-8",
        required_markers_present=False,
    )
    checks = evaluate_pair([dataclass_replace(nodes[0], web_assets=tuple(broken_assets)), nodes[1]])
    failed = {check.name: check.message for check in checks if not check.passed}
    assert "node.1.web_hosting" in failed
    assert "redirected away" in failed["node.1.web_hosting"]
    assert "missing its required bundle marker" in failed["node.1.web_hosting"]


def test_phone_browser_cors_must_allow_cross_origin_control_and_media() -> None:
    """Direct APIs alone cannot qualify a browser flow whose preflight omits auth or ranges."""
    nodes = _nominal()
    broken_cors = dataclasses.replace(
        nodes[1].browser_cors,
        allow_headers="Content-Type",
    )
    checks = evaluate_pair([nodes[0], dataclass_replace(nodes[1], browser_cors=broken_cors)])
    failed = {check.name: check.message for check in checks if not check.passed}
    assert "node.2.browser_cors" in failed
    assert "authorization" not in broken_cors.allow_headers.lower()
    assert "headers=['content-type']" in failed["node.2.browser_cors"]


def test_token_and_telemetry_parsers_reject_malformed_input() -> None:
    """Private credential and system telemetry parsers are strict and bounded."""
    token = "a" * 32
    assert parse_control_token(f'<map><string name="control_token">{token}</string></map>') == token
    telemetry = parse_telemetry(
        "level: 75\nscale: 100\ntemperature: 301\nvoltage: 4200\n",
        "Thermal Status: 1\n",
        "8589934592\n",
    )
    expected_percent = 75
    assert telemetry.battery_percent == expected_percent
    assert telemetry.thermal_status == 1
    try:
        parse_control_token('<string name="control_token">secret</string>')
    except ValueError:
        pass
    else:
        message = "short token was accepted"
        raise AssertionError(message)


def test_wifi_bssid_parser_requires_one_current_non_placeholder_association() -> None:
    """Stale scans and Android's privacy placeholder cannot masquerade as association evidence."""
    assert (
        parse_wifi_bssid('WifiInfo: SSID: "field", BSSID: 44:D9:E7:AA:BB:CC, RSSI: -48\n')
        == "44:d9:e7:aa:bb:cc"
    )
    second_inventory = "mWifiInfo: SSID: field, BSSID: 44:d9:e7:aa:bb:dd"
    for malformed in (
        "WifiInfo: SSID: <unknown ssid>, BSSID: 02:00:00:00:00:00",
        "Wi-Fi is enabled but disconnected",
        f"WifiInfo: SSID: field, BSSID: 44:d9:e7:aa:bb:cc\n{second_inventory}",
    ):
        _expect_bssid_rejected(malformed)


def _expect_bssid_rejected(malformed: str) -> None:
    try:
        parse_wifi_bssid(malformed)
    except ValueError:
        return
    message = f"malformed BSSID inventory was accepted: {malformed!r}"
    raise AssertionError(message)


def test_collector_uses_direct_lan_auth_and_redacts_credential() -> None:
    """The concrete collector reads app-private auth but retains no token in evidence."""
    fixture = _nominal()[0]
    token = "C" * 32
    backends = _FakeCollectionBackends(fixture, token)

    collector = EvidenceCollector(
        Path("/runfiles/adb"),
        command_runner=backends.run,
        http_reader=backends.read,
        asset_reader=backends.read_asset,
        cors_reader=backends.read_cors,
    )
    evidence = collector.collect(fixture.serial, fixture.origin)
    peer_origin = "http://10.0.0.2:8088"
    diagnostics = collector.collect_lan_diagnostics(fixture.serial, fixture.origin, peer_origin)
    assert evidence.descriptor == fixture.descriptor
    assert evidence.field_recording_status == fixture.field_recording_status
    assert evidence.telemetry.storage_usable_bytes == 8 * 1024**3
    assert diagnostics.bssid == "44:d9:e7:aa:bb:cc"
    assert diagnostics.host_to_phone.reachable
    assert diagnostics.phone_to_peer.reachable
    assert diagnostics.phone_to_peer.target == peer_origin
    expected_authenticated_requests = 4
    assert backends.observed_http_tokens.count(token) == expected_authenticated_requests
    assert "http:node:authenticated" in backends.observed_operations
    assert "http:field-recording-status:authenticated" in backends.observed_operations
    assert "http:clock:anonymous" in backends.observed_operations
    assert backends.observed_asset_urls == [
        f"{fixture.origin}/",
        f"{fixture.origin}/app.js",
        f"{fixture.origin}/app.css",
    ]
    assert backends.observed_cors_urls == [f"{fixture.origin}/api/v1/capture/status"]
    assert "adb:peer_route" in backends.observed_operations
    assert "adb:peer_ping" in backends.observed_operations
    ping_command = next(command for command in backends.observed_adb_commands if "ping" in command)
    assert "-I" not in ping_command
    assert backends.observed_operations.index(
        "adb:peer_route"
    ) < backends.observed_operations.index("adb:peer_ping")
    status_operation = backends.observed_operations.index("http:status:authenticated")
    assert status_operation < backends.observed_operations.index("adb:sha256sum")
    assert status_operation < backends.observed_operations.index("http:setup:authenticated")
    assert token not in repr(evidence)


def test_lan_matrix_fails_closed_on_usb_forward_and_one_way_peer_path() -> None:
    """Healthy ADB/HTTP and one phone's successful path cannot substitute for every edge."""
    nodes = _nominal()
    token = "D" * 32
    blocked_backends = _FakeCollectionBackends(nodes[0], token, peer_ping_reachable=False)
    blocked = EvidenceCollector(
        Path("/runfiles/adb"),
        command_runner=blocked_backends.run,
        http_reader=blocked_backends.read,
    ).collect_lan_diagnostics(nodes[0].serial, nodes[0].origin, nodes[1].origin)
    forwarded_backends = _FakeCollectionBackends(nodes[1], token)
    forwarded = EvidenceCollector(
        Path("/runfiles/adb"),
        command_runner=forwarded_backends.run,
        http_reader=forwarded_backends.read,
    ).collect_lan_diagnostics(
        nodes[1].serial,
        "http://127.0.0.1:18088",
        nodes[0].origin,
    )

    checks = evaluate_lan_diagnostics([blocked, forwarded])
    failed = {check.name: check.message for check in checks if not check.passed}
    assert set(failed) == {
        "node.1.phone_to_peer_lan",
        "node.2.host_to_phone_lan",
        "node.2.phone_to_peer_lan",
    }
    assert "one-sided ARP" in failed["node.1.phone_to_peer_lan"]
    assert "USB forwarding" in failed["node.2.host_to_phone_lan"]
    assert "healthy ADB" in failed["node.2.phone_to_peer_lan"]
    matrix = cast(
        "list[Mapping[str, object]]",
        lan_diagnostics_json([blocked, forwarded])["reachability_matrix"],
    )
    expected_matrix_edges = 4
    assert len(matrix) == expected_matrix_edges
    assert sum(edge["reachable"] is True for edge in matrix) == 1


def test_phone_peer_lan_rejects_non_wifi_or_incomplete_route_evidence() -> None:
    """A successful ordinary ping cannot qualify without exact wlan0 route provenance."""
    nodes = _nominal()
    source = "10.0.0.3"
    rejected_routes = (
        f"10.0.0.2 dev rmnet_data0 src {source} uid 2000",
        "",
        f"garbage dev wlan0 src {source}",
        "10.0.0.2 dev wlan0 uid 2000",
        "10.0.0.2 dev wlan0 src 10.0.0.99 uid 2000",
        f"10.0.0.2 dev wlan0 src {source} uid 2000\ncache dev rmnet_data0",
    )
    for route in rejected_routes:
        backends = _FakeCollectionBackends(nodes[0], "D" * 32, route_output=route)
        diagnostic = EvidenceCollector(
            Path("/runfiles/adb"),
            command_runner=backends.run,
            http_reader=backends.read,
        ).collect_lan_diagnostics(nodes[0].serial, nodes[0].origin, nodes[1].origin)
        assert not diagnostic.phone_to_peer.reachable, route
        assert "adb:peer_route" in backends.observed_operations
        assert "adb:peer_ping" not in backends.observed_operations


def test_phone_peer_lan_rejects_route_or_ping_command_failure() -> None:
    """Both read-only route evidence and the peer probe must complete successfully."""
    nodes = _nominal()
    cases = (
        _FakeCollectionBackends(nodes[0], "D" * 32, route_probe_succeeds=False),
        _FakeCollectionBackends(nodes[0], "D" * 32, peer_ping_reachable=False),
    )
    for backends in cases:
        diagnostic = EvidenceCollector(
            Path("/runfiles/adb"),
            command_runner=backends.run,
            http_reader=backends.read,
        ).collect_lan_diagnostics(nodes[0].serial, nodes[0].origin, nodes[1].origin)
        assert not diagnostic.phone_to_peer.reachable
        assert "adb:peer_route" in backends.observed_operations
    assert "adb:peer_ping" not in cases[0].observed_operations
    assert "adb:peer_ping" in cases[1].observed_operations


def test_authenticated_json_read_rejects_redirect_without_forwarding_bearer() -> None:
    """An attacker-controlled redirect cannot receive a phone control credential."""
    observed_authorizations: dict[str, str | None] = {}

    class RedirectHandler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            observed_authorizations[self.path] = self.headers.get("Authorization")
            if self.path == "/source":
                self.send_response(302)
                self.send_header("Location", "/sink")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b"{}")

        def log_message(  # pyright: ignore[reportIncompatibleMethodOverride, reportImplicitOverride]
            self, format_string: str, *arguments: object
        ) -> None:
            del format_string, arguments

    server = ThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        token = "R" * 32
        status, body = EvidenceCollector._read_json(  # pyright: ignore[reportPrivateUsage]  # noqa: SLF001
            f"http://127.0.0.1:{server.server_port}/source", token
        )
        expected_redirect_status = 302
        assert status == expected_redirect_status
        assert body == {}
        assert observed_authorizations == {"/source": f"Bearer {token}"}
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def test_pair_collection_reports_every_unreachable_phone() -> None:
    """One refused origin cannot suppress diagnosis of the other phone in the same pass."""
    nodes = _nominal()
    collector = _SelectiveCollector(
        {
            "leader-serial": ConnectionRefusedError("leader API refused connection"),
            "shadow-serial": ConnectionRefusedError("shadow API refused connection"),
        }
    )
    evidence, diagnostics, checks = collect_pair_evidence(
        collector,
        [
            ("leader-serial", nodes[0].origin),
            ("shadow-serial", nodes[1].origin),
        ],
    )
    assert evidence == []
    expected_phone_count = 2
    assert len(diagnostics) == expected_phone_count
    assert all(diagnostic.bssid is not None for diagnostic in diagnostics)
    failed = {check.name: check.message for check in checks if not check.passed}
    assert set(failed) == {
        "node.1.evidence_collection",
        "node.2.evidence_collection",
        "pair.count",
    }
    assert "leader-serial" in failed["node.1.evidence_collection"]
    assert "shadow-serial" in failed["node.2.evidence_collection"]


def test_report_path_resolves_against_workspace_without_changing_absolute_path() -> None:
    """Bazel-run reports land in the checkout rather than its runfiles tree."""
    relative = Path("artifacts/pair_preflight/report.json")
    workspace = "/workspace/swing_capture"
    assert resolve_output_path(relative, workspace) == Path(workspace) / relative
    absolute = Path("/var/lib/swing_capture/pair_preflight/report.json")
    assert resolve_output_path(absolute, workspace) == absolute


def test_expected_apk_prefers_a_bazel_runfile_before_the_workspace() -> None:
    """A runfiles APK wins when tests execute away from the workspace root."""
    with tempfile.TemporaryDirectory() as temporary_directory:
        tmp_path = Path(temporary_directory)
        runfile = tmp_path / "android" / "app" / "swing_capture.apk"
        runfile.parent.mkdir(parents=True)
        runfile.write_bytes(b"apk")
        previous = Path.cwd()
        try:
            os.chdir(tmp_path)
            assert resolve_input_path(Path("android/app/swing_capture.apk")) == Path(
                "android/app/swing_capture.apk"
            )
        finally:
            os.chdir(previous)


def dataclass_replace(value: NodeEvidence, **changes: object) -> NodeEvidence:
    """Typed wrapper keeps test fixture mutation concise under strict analysis."""
    return dataclasses.replace(value, **changes)


if __name__ == "__main__":
    test_nominal_pair_is_ready()
    test_stopped_clean_gate_rejects_capture_and_autonomous_publication_work()
    test_stopped_clean_gate_rejects_field_recording_and_malformed_evidence()
    test_capture_status_diagnostics_retains_metrics_and_redacts_credentials()
    test_wrong_role_and_duplicate_identity_fail()
    test_two_shadow_phones_fail_before_field_use()
    test_stale_binding_and_readiness_issue_fail()
    test_product_floor_and_current_boot_must_be_explicitly_ready()
    test_older_apk_contract_is_reported_per_phone_without_aborting_pair_diagnosis()
    test_resource_and_optional_monitoring_gates_fail()
    test_monitoring_gate_allows_only_the_expected_edit_lock()
    test_monitoring_gate_rejects_hidden_pair_audio_and_inference_failures()
    test_monitoring_gate_rejects_wrong_peer_clock_identity_and_uncertainty()
    test_wireless_adb_gate_distinguishes_usb()
    test_expected_apk_gate_rejects_only_the_stale_install()
    test_phone_hosted_web_bundle_must_be_complete_and_direct()
    test_phone_browser_cors_must_allow_cross_origin_control_and_media()
    test_token_and_telemetry_parsers_reject_malformed_input()
    test_wifi_bssid_parser_requires_one_current_non_placeholder_association()
    test_collector_uses_direct_lan_auth_and_redacts_credential()
    test_lan_matrix_fails_closed_on_usb_forward_and_one_way_peer_path()
    test_phone_peer_lan_rejects_non_wifi_or_incomplete_route_evidence()
    test_phone_peer_lan_rejects_route_or_ping_command_failure()
    test_authenticated_json_read_rejects_redirect_without_forwarding_bearer()
    test_pair_collection_reports_every_unreachable_phone()
    test_report_path_resolves_against_workspace_without_changing_absolute_path()
    test_expected_apk_prefers_a_bazel_runfile_before_the_workspace()
