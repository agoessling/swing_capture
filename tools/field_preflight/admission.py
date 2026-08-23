"""Connect, launch, and admit an Android pair without changing pair configuration."""

from __future__ import annotations

import concurrent.futures
import dataclasses
import hashlib
import json
import os
import re
import subprocess
import tempfile
import time
import urllib.parse
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import TypedDict, cast

PACKAGE_NAME = "com.agoessling.swingcapture"
MAIN_ACTIVITY = f"{PACKAGE_NAME}/.MainActivity"
WIRELESS_SERIAL_PATTERN = re.compile(r"^(?P<host>[A-Za-z0-9._-]+):(?P<port>[0-9]{1,5})$")
WIRELESS_HOST_PATTERN = re.compile(r"^[A-Za-z0-9._-]+$")
MDNS_CONNECT_SERVICE = "_adb-tls-connect._tcp"
MAXIMUM_LOG_BYTES = 64 * 1024
MAXIMUM_NETWORK_PORT = 65_535
PAIR_NODE_COUNT = 2
MDNS_COMPACT_FIELD_COUNT = 2
MDNS_VERBOSE_FIELD_COUNT = 3
HTTP_OK = 200
MAXIMUM_DOCTOR_FAILURES = 32
MAXIMUM_ATTEMPTS = 10
MAXIMUM_RETRY_SECONDS = 30
CAMERA_ROLES: frozenset[str] = frozenset(("face_on", "down_the_line"))
CREDENTIAL_TOKEN_PATTERN = re.compile(r"(?<![A-Za-z0-9_-])[A-Za-z0-9_-]{32}(?![A-Za-z0-9_-])")
BEARER_CREDENTIAL_PATTERN = re.compile(r"(?i)(\bbearer\s+)[A-Za-z0-9._~+/=-]+")
BSSID_PATTERN = re.compile(r"^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$")


class AdmissionError(RuntimeError):
    """Raised when invocation cannot safely produce admission evidence."""


@dataclasses.dataclass(frozen=True)
class Node:
    """One wireless-ADB host or endpoint and its direct application origin."""

    serial: str
    origin: str
    adb_endpoint_source: str = "explicit"


@dataclasses.dataclass(frozen=True)
class ExpectedRole:
    """Operator-confirmed camera role for one stable DHCP host."""

    host: str
    role: str


@dataclasses.dataclass(frozen=True)
class CommandResult:
    """Bounded subprocess result used by the production and fake runners."""

    returncode: int
    stdout: str = ""
    stderr: str = ""


CommandRunner = Callable[[Sequence[str]], CommandResult]
Sleeper = Callable[[float], None]


class NodeReport(TypedDict):
    """Credential-free preparation result for one phone."""

    serial: str
    origin: str
    adb_endpoint_source: str
    connected: bool
    authorized: bool
    user_unlocked: bool
    launch_requested: bool
    launch_succeeded: bool
    screen_sleep_requested: bool
    screen_sleep_succeeded: bool
    screen_noninteractive: bool
    issues: list[str]
    passed: bool


class DoctorFailure(TypedDict):
    """One credential-redacted strict-doctor failure safe for operator output."""

    name: str
    message: str


class DoctorAttempt(TypedDict):
    """One bounded strict-doctor invocation."""

    attempt: int
    exit_code: int
    report: str | None
    role_association_passed: bool
    exact_apk_evidence_passed: bool | None
    lan_diagnostics_passed: bool
    failures: list[DoctorFailure]
    passed: bool


class RoleAssociation(TypedDict):
    """Expected and observed credential-free role binding for one host."""

    host: str
    expected_role: str
    observed_role: str | None
    passed: bool


class AdmissionReport(TypedDict):
    """Stable aggregate report retained by the field wrapper."""

    schema_version: int
    report_type: str
    passed: bool
    credentials_redacted: bool
    reboot_performed: bool
    device_unlocked_by_tool: bool
    launch_mode: str
    screen_policy: str
    require_monitoring: bool
    expected_apk_sha256: str | None
    preparation_failure: str | None
    nodes: list[NodeReport]
    role_associations: list[RoleAssociation]
    lan_diagnostics: dict[str, object] | None
    doctor_attempts: list[DoctorAttempt]
    limitations: list[str]


def parse_node(value: str) -> Node:
    """Parse and cross-check HOST[:ADB_PORT]=http://HOST:APP_PORT."""
    serial, separator, origin = value.partition("=")
    serial_match = WIRELESS_SERIAL_PATTERN.fullmatch(serial)
    host_only = WIRELESS_HOST_PATTERN.fullmatch(serial)
    if not separator or (serial_match is None and host_only is None):
        message = "--node must use HOST[:ADB_PORT]=http://HOST:APP_PORT"
        raise ValueError(message)
    if serial_match is not None:
        adb_port = int(serial_match.group("port"))
        if not 1 <= adb_port <= MAXIMUM_NETWORK_PORT:
            message = "wireless ADB port is outside 1..65535"
            raise ValueError(message)
    parsed = urllib.parse.urlsplit(origin)
    try:
        application_port = parsed.port
    except ValueError as failure:
        message = "application origin contains an invalid port"
        raise ValueError(message) from failure
    if (
        parsed.scheme != "http"
        or parsed.hostname is None
        or application_port is None
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
    ):
        message = "application origin must be an uncredentialed http://HOST:PORT origin"
        raise ValueError(message)
    adb_host = serial_match.group("host") if serial_match is not None else serial
    if parsed.hostname.casefold() != adb_host.casefold():
        message = "wireless ADB endpoint and application origin must name the same phone"
        raise ValueError(message)
    return Node(
        serial=serial,
        origin=f"http://{parsed.hostname}:{application_port}",
        adb_endpoint_source="explicit" if serial_match is not None else "mdns_pending",
    )


def parse_expected_role(value: str) -> ExpectedRole:
    """Parse HOST=face_on|down_the_line without accepting a transient ADB port."""
    host, separator, role = value.partition("=")
    if not separator or WIRELESS_HOST_PATTERN.fullmatch(host) is None or role not in CAMERA_ROLES:
        message = "--expected-role must use HOST=face_on|down_the_line"
        raise ValueError(message)
    return ExpectedRole(host, role)


def _adb_host(serial: str) -> str:
    match = WIRELESS_SERIAL_PATTERN.fullmatch(serial)
    return match.group("host") if match is not None else serial


def validate_pair(nodes: Sequence[Node]) -> None:
    """Reject ambiguous or duplicated pair invocations before contacting hardware."""
    if len(nodes) != PAIR_NODE_COUNT:
        message = "exactly two --node values are required"
        raise ValueError(message)
    if len({_adb_host(node.serial).casefold() for node in nodes}) != PAIR_NODE_COUNT:
        message = "wireless ADB hosts must be distinct"
        raise ValueError(message)
    if len({node.origin for node in nodes}) != PAIR_NODE_COUNT:
        message = "application origins must be distinct"
        raise ValueError(message)


def validate_expected_roles(nodes: Sequence[Node], expected_roles: Sequence[ExpectedRole]) -> None:
    """Require one complementary role bound to each requested stable phone host."""
    if len(expected_roles) != PAIR_NODE_COUNT:
        message = "exactly two --expected-role values are required"
        raise ValueError(message)
    by_host = {association.host.casefold(): association.role for association in expected_roles}
    if len(by_host) != PAIR_NODE_COUNT:
        message = "expected-role hosts must be distinct"
        raise ValueError(message)
    node_hosts = {_adb_host(node.serial).casefold() for node in nodes}
    if set(by_host) != node_hosts:
        message = "expected-role hosts must exactly match the two --node hosts"
        raise ValueError(message)
    if frozenset(by_host.values()) != CAMERA_ROLES:
        message = "expected roles must be complementary face_on and down_the_line"
        raise ValueError(message)


def subprocess_runner(arguments: Sequence[str]) -> CommandResult:
    """Run one bounded host command without a shell."""
    result = subprocess.run(
        list(arguments),
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return CommandResult(result.returncode, result.stdout, result.stderr)


def _bounded_log(value: str) -> str:
    encoded = value.encode("utf-8", errors="replace")
    if len(encoded) <= MAXIMUM_LOG_BYTES:
        return encoded.decode("utf-8", errors="replace")
    suffix = b"\n[log truncated]\n"
    return (encoded[: MAXIMUM_LOG_BYTES - len(suffix)] + suffix).decode("utf-8", errors="replace")


def _redact_text(value: str) -> str:
    """Remove current bearer-token shapes even from a misbehaving child tool."""
    redacted = BEARER_CREDENTIAL_PATTERN.sub(r"\1[REDACTED]", value)
    return CREDENTIAL_TOKEN_PATTERN.sub("[REDACTED]", redacted)


def _redact_json(value: object) -> object:
    """Recursively redact credential-shaped string values in retained child JSON."""
    if isinstance(value, str):
        return _redact_text(value)
    if isinstance(value, list):
        return [_redact_json(item) for item in cast("list[object]", value)]
    if isinstance(value, Mapping):
        mapping = cast("Mapping[object, object]", value)
        return {str(key): _redact_json(item) for key, item in mapping.items()}
    return value


def _json_mapping(value: object) -> Mapping[str, object] | None:
    if not isinstance(value, Mapping):
        return None
    mapping = cast("Mapping[object, object]", value)
    if not all(isinstance(key, str) for key in mapping):
        return None
    return cast("Mapping[str, object]", mapping)


def _json_list(value: object) -> list[object] | None:
    return cast("list[object]", value) if isinstance(value, list) else None


def _run(runner: CommandRunner, arguments: Sequence[str]) -> CommandResult:
    try:
        result = runner(arguments)
    except (OSError, subprocess.SubprocessError) as failure:
        return CommandResult(127, "", str(failure))
    return CommandResult(
        result.returncode,
        _redact_text(_bounded_log(result.stdout)),
        _redact_text(_bounded_log(result.stderr)),
    )


def parse_mdns_connect_services(output: str) -> Mapping[str, tuple[str, ...]]:
    """Parse bounded `adb mdns services` output into connect endpoints grouped by host."""
    endpoints_by_host: dict[str, set[str]] = {}
    for line in output.splitlines():
        if MDNS_CONNECT_SERVICE not in line:
            continue
        fields = line.split()
        if len(fields) == MDNS_COMPACT_FIELD_COUNT and fields[0].rstrip(".").endswith(
            f".{MDNS_CONNECT_SERVICE}"
        ):
            endpoint = fields[1]
        elif (
            len(fields) == MDNS_VERBOSE_FIELD_COUNT
            and fields[1].rstrip(".") == MDNS_CONNECT_SERVICE
        ):
            endpoint = fields[2]
        else:
            message = "adb mDNS connect-service output is malformed"
            raise ValueError(message)
        endpoint_match = WIRELESS_SERIAL_PATTERN.fullmatch(endpoint)
        if (
            endpoint_match is None
            or not 1 <= int(endpoint_match.group("port")) <= MAXIMUM_NETWORK_PORT
        ):
            message = "adb mDNS connect endpoint is malformed"
            raise ValueError(message)
        host = endpoint_match.group("host").casefold()
        endpoints_by_host.setdefault(host, set()).add(endpoint)
    return {host: tuple(sorted(endpoints)) for host, endpoints in sorted(endpoints_by_host.items())}


def resolve_wireless_nodes(
    adb: Path,
    nodes: Sequence[Node],
    *,
    runner: CommandRunner,
) -> tuple[Node, ...]:
    """Resolve every host-only node to exactly one current TLS connect endpoint."""
    validate_pair(nodes)
    if all(WIRELESS_SERIAL_PATTERN.fullmatch(node.serial) is not None for node in nodes):
        return tuple(nodes)
    discovery = _run(runner, [str(adb), "mdns", "services"])
    if discovery.returncode != 0:
        message = "adb mDNS discovery failed; verify the workstation is still paired"
        raise AdmissionError(message)
    try:
        endpoints_by_host = parse_mdns_connect_services(discovery.stdout)
    except ValueError as failure:
        raise AdmissionError(str(failure)) from failure
    resolved: list[Node] = []
    for node in nodes:
        if WIRELESS_SERIAL_PATTERN.fullmatch(node.serial) is not None:
            resolved.append(node)
            continue
        endpoints = endpoints_by_host.get(_adb_host(node.serial).casefold(), ())
        if len(endpoints) != 1:
            detail = "none" if not endpoints else ", ".join(endpoints)
            message = (
                f"expected exactly one _adb-tls-connect._tcp endpoint for {node.serial}; "
                f"found {detail}"
            )
            raise AdmissionError(message)
        resolved.append(Node(endpoints[0], node.origin, "mdns"))
    return tuple(resolved)


def _prepare_node(  # noqa: C901 - preparation intentionally records each independent gate.
    adb: Path,
    node: Node,
    *,
    launch_after_unlock: bool,
    sleep_screen_after_launch: bool,
    runner: CommandRunner,
) -> NodeReport:
    """Connect and inspect one node, optionally performing the documented foreground launch."""
    evidence: NodeReport = {
        "serial": node.serial,
        "origin": node.origin,
        "adb_endpoint_source": node.adb_endpoint_source,
        "connected": False,
        "authorized": False,
        "user_unlocked": False,
        "launch_requested": launch_after_unlock,
        "launch_succeeded": False,
        "screen_sleep_requested": sleep_screen_after_launch,
        "screen_sleep_succeeded": False,
        "screen_noninteractive": False,
        "issues": [],
        "passed": False,
    }
    issues = evidence["issues"]
    connection = _run(runner, [str(adb), "connect", node.serial])
    connection_output = f"{connection.stdout}\n{connection.stderr}".casefold()
    evidence["connected"] = connection.returncode == 0 and not any(
        marker in connection_output for marker in ("failed", "unable", "cannot", "refused")
    )
    if not evidence["connected"]:
        issues.append("Wireless ADB connection failed; enable Wireless debugging and reconnect it.")

    state = _run(runner, [str(adb), "-s", node.serial, "get-state"])
    evidence["authorized"] = state.returncode == 0 and state.stdout.strip() == "device"
    if not evidence["authorized"]:
        issues.append("Wireless ADB is offline or unauthorized on this workstation.")

    if evidence["authorized"]:
        unlocked = _run(
            runner,
            [
                str(adb),
                "-s",
                node.serial,
                "shell",
                "getprop",
                "sys.user.0.ce_available",
            ],
        )
        evidence["user_unlocked"] = (
            unlocked.returncode == 0 and unlocked.stdout.strip().casefold() == "true"
        )
        if not evidence["user_unlocked"]:
            issues.append("Unlock this phone once after boot; the tool never unlocks a device.")

    if launch_after_unlock and evidence["user_unlocked"]:
        launched = _run(
            runner,
            [
                str(adb),
                "-s",
                node.serial,
                "shell",
                "am",
                "start",
                "-W",
                "-n",
                MAIN_ACTIVITY,
            ],
        )
        evidence["launch_succeeded"] = (
            launched.returncode == 0
            and re.search(r"(?im)^Status:\s*ok\s*$", launched.stdout) is not None
        )
        if not evidence["launch_succeeded"]:
            issues.append("Foreground application launch did not complete successfully.")
    elif not launch_after_unlock:
        evidence["launch_succeeded"] = False

    if sleep_screen_after_launch and evidence["launch_succeeded"]:
        sleep_result = _run(
            runner,
            [
                str(adb),
                "-s",
                node.serial,
                "shell",
                "input",
                "keyevent",
                "KEYCODE_SLEEP",
            ],
        )
        evidence["screen_sleep_succeeded"] = sleep_result.returncode == 0
        power = _run(
            runner,
            [str(adb), "-s", node.serial, "shell", "dumpsys", "power"],
        )
        evidence["screen_noninteractive"] = power.returncode == 0 and (
            re.search(r"(?im)^\s*mWakefulness=(?:Asleep|Dozing)\s*$", power.stdout) is not None
            or re.search(r"(?im)^\s*mInteractive=false\s*$", power.stdout) is not None
        )
        if not evidence["screen_sleep_succeeded"]:
            issues.append("Android rejected the screen-sleep command.")
        if not evidence["screen_noninteractive"]:
            issues.append("Phone did not report a non-interactive or dozing power state.")

    evidence["passed"] = (
        evidence["connected"] is True
        and evidence["authorized"] is True
        and evidence["user_unlocked"] is True
        and (not launch_after_unlock or evidence["launch_succeeded"] is True)
        and (
            not sleep_screen_after_launch
            or (
                evidence["screen_sleep_succeeded"] is True
                and evidence["screen_noninteractive"] is True
            )
        )
    )
    return evidence


def _read_doctor_report(path: Path) -> Mapping[str, object] | None:
    try:
        value = cast("object", json.loads(path.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError):
        return None
    return _json_mapping(value)


def _exact_apk_evidence_passed(
    doctor_report: Mapping[str, object] | None,
    expected_apk_sha256: str | None,
) -> bool | None:
    """Require reproducible installed-APK identity when the CLI supplied an APK."""
    if expected_apk_sha256 is None:
        return None
    if doctor_report is None or doctor_report.get("expected_apk_sha256") != expected_apk_sha256:
        return False
    nodes = _json_list(doctor_report.get("nodes"))
    return bool(
        nodes is not None
        and len(nodes) == PAIR_NODE_COUNT
        and all(
            (mapping := _json_mapping(node)) is not None
            and mapping.get("installed_apk_sha256") == expected_apk_sha256
            for node in nodes
        )
    )


def _lan_diagnostics_evidence(  # noqa: C901 - validates a nested evidence schema.
    doctor_report: Mapping[str, object] | None,
    nodes: Sequence[Node],
) -> tuple[dict[str, object] | None, bool]:
    """Require the strict doctor's complete BSSID inventory and directed LAN matrix."""
    if doctor_report is None:
        return None, False
    raw_diagnostics = _json_mapping(doctor_report.get("lan_diagnostics"))
    if raw_diagnostics is None:
        return None, False
    diagnostics = dict(raw_diagnostics)
    raw_nodes = _json_list(diagnostics.get("nodes"))
    raw_matrix = _json_list(diagnostics.get("reachability_matrix"))
    expected = {(node.serial, node.origin) for node in nodes}
    observed: set[tuple[str, str]] = set()
    inventory_valid = (
        diagnostics.get("schema_version") == 1
        and raw_nodes is not None
        and len(raw_nodes) == len(nodes)
    )
    if raw_nodes is not None:
        for raw_node_value in raw_nodes:
            raw_node = _json_mapping(raw_node_value)
            if raw_node is None:
                inventory_valid = False
                continue
            serial = raw_node.get("serial")
            origin = raw_node.get("origin")
            bssid = raw_node.get("bssid")
            if (
                not isinstance(serial, str)
                or not isinstance(origin, str)
                or not isinstance(bssid, str)
                or BSSID_PATTERN.fullmatch(bssid) is None
                or (serial, origin) not in expected
                or (serial, origin) in observed
            ):
                inventory_valid = False
                continue
            observed.add((serial, origin))
    inventory_valid = inventory_valid and observed == expected

    expected_edges = {("field_host", node.origin, "direct_http_clock") for node in nodes}
    expected_edges.update(
        {
            (node.origin, nodes[1 - index].origin, "adb_shell_wifi_icmp")
            for index, node in enumerate(nodes)
        }
    )
    observed_edges: set[tuple[str, str, str]] = set()
    matrix_valid = raw_matrix is not None and len(raw_matrix) == len(expected_edges)
    if raw_matrix is not None:
        for raw_edge_value in raw_matrix:
            raw_edge = _json_mapping(raw_edge_value)
            if raw_edge is None:
                matrix_valid = False
                continue
            source = raw_edge.get("source")
            target = raw_edge.get("target")
            transport = raw_edge.get("transport")
            if not all(isinstance(value, str) for value in (source, target, transport)):
                matrix_valid = False
                continue
            edge = cast("tuple[str, str, str]", (source, target, transport))
            if (
                edge not in expected_edges
                or edge in observed_edges
                or raw_edge.get("reachable") is not True
                or (transport == "direct_http_clock" and raw_edge.get("http_status") != HTTP_OK)
            ):
                matrix_valid = False
                continue
            observed_edges.add(edge)
    matrix_valid = matrix_valid and observed_edges == expected_edges
    return diagnostics, inventory_valid and matrix_valid


def _role_association_evidence(
    doctor_report: Mapping[str, object] | None,
    expected_roles: Sequence[ExpectedRole],
) -> tuple[list[RoleAssociation], bool]:
    """Compare the strict doctor's observed role to each stable host assignment."""
    expected_by_host = {association.host.casefold(): association for association in expected_roles}
    observed_by_host: dict[str, str] = {}
    shape_valid = doctor_report is not None
    raw_nodes = None if doctor_report is None else _json_list(doctor_report.get("nodes"))
    if raw_nodes is None or len(raw_nodes) != len(expected_roles):
        shape_valid = False
    else:
        for raw_node_value in raw_nodes:
            raw_node = _json_mapping(raw_node_value)
            if raw_node is None:
                shape_valid = False
                continue
            serial = raw_node.get("serial")
            role = raw_node.get("role")
            if not isinstance(serial, str) or not isinstance(role, str):
                shape_valid = False
                continue
            match = WIRELESS_SERIAL_PATTERN.fullmatch(serial)
            if match is None:
                shape_valid = False
                continue
            host = match.group("host").casefold()
            if host not in expected_by_host or host in observed_by_host:
                shape_valid = False
                continue
            observed_by_host[host] = role
    complete = shape_valid and set(observed_by_host) == set(expected_by_host)
    associations: list[RoleAssociation] = []
    for expected in expected_roles:
        observed = observed_by_host.get(expected.host.casefold())
        associations.append(
            {
                "host": expected.host,
                "expected_role": expected.role,
                "observed_role": observed,
                "passed": complete and observed == expected.role,
            }
        )
    passed = complete and all(association["passed"] for association in associations)
    return associations, passed


def _doctor_failures(doctor_report: Mapping[str, object] | None) -> list[DoctorFailure]:
    """Extract bounded actionable failures only from a credential-redacted report."""
    if doctor_report is None or doctor_report.get("credentials_redacted") is not True:
        return []
    raw_checks = _json_list(doctor_report.get("checks"))
    if raw_checks is None:
        return []
    failures: list[DoctorFailure] = []
    for raw_check_value in raw_checks:
        raw_check = _json_mapping(raw_check_value)
        if raw_check is None or raw_check.get("passed") is not False:
            continue
        name = raw_check.get("name")
        message = raw_check.get("message")
        if not isinstance(name, str) or not name or not isinstance(message, str) or not message:
            continue
        failures.append(
            {
                "name": name[:256],
                "message": " ".join(message.split())[:1024],
            }
        )
        if len(failures) == MAXIMUM_DOCTOR_FAILURES:
            break
    return failures


def _write_json_atomic(path: Path, value: Mapping[str, object]) -> None:
    _write_text_atomic(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def _write_text_atomic(path: Path, value: str) -> None:
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary.write(value)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_path = Path(temporary.name)
        temporary_path.replace(path)
        temporary_path = None
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def execute(  # noqa: C901, PLR0912, PLR0913, PLR0915 - linear evidence ceremony.
    adb: Path,
    doctor: Path,
    nodes: Sequence[Node],
    evidence_directory: Path,
    *,
    expected_roles: Sequence[ExpectedRole],
    launch_after_unlock: bool = False,
    sleep_screen_after_launch: bool = False,
    require_monitoring: bool = False,
    maximum_attempts: int = 3,
    retry_seconds: float = 2.0,
    runner: CommandRunner = subprocess_runner,
    sleeper: Sleeper = time.sleep,
    expected_apk: Path | None = None,
) -> AdmissionReport:
    """Execute a bounded ceremony and return credential-free admission evidence."""
    validate_pair(nodes)
    validate_expected_roles(nodes, expected_roles)
    if sleep_screen_after_launch and not launch_after_unlock:
        message = "sleep_screen_after_launch requires launch_after_unlock"
        raise ValueError(message)
    if not 1 <= maximum_attempts <= MAXIMUM_ATTEMPTS:
        message = "maximum_attempts must be in 1..10"
        raise ValueError(message)
    if not 0 <= retry_seconds <= MAXIMUM_RETRY_SECONDS:
        message = "retry_seconds must be in 0..30"
        raise ValueError(message)
    if evidence_directory.exists():
        message = "evidence directory already exists; refusing to overwrite it"
        raise AdmissionError(message)
    expected_apk_sha256 = None
    if expected_apk is not None:
        if not expected_apk.is_file():
            message = "expected_apk must name a regular file"
            raise ValueError(message)
        expected_apk_sha256 = _sha256(expected_apk)
    evidence_directory.mkdir(parents=True)

    role_associations, _ = _role_association_evidence(None, expected_roles)
    report: AdmissionReport = {
        "schema_version": 1,
        "report_type": "android_field_preflight",
        "passed": False,
        "credentials_redacted": True,
        "reboot_performed": False,
        "device_unlocked_by_tool": False,
        "launch_mode": "adb_foreground_activity" if launch_after_unlock else "none",
        "screen_policy": "sleep_after_launch" if sleep_screen_after_launch else "unchanged",
        "require_monitoring": require_monitoring,
        "expected_apk_sha256": expected_apk_sha256,
        "preparation_failure": None,
        "nodes": [],
        "role_associations": role_associations,
        "lan_diagnostics": None,
        "doctor_attempts": [],
        "limitations": [
            "This run does not prove that an OS reboot occurred.",
            "ADB launch is a development aid, not evidence of an on-device human launch ceremony.",
            "Physical camera, microphone, thermal, and swing behavior require separate evidence.",
        ],
    }
    # Leave an explicit failing checkpoint even if preparation or the child doctor is interrupted.
    _write_json_atomic(evidence_directory / "report.json", report)
    try:
        resolved_nodes = resolve_wireless_nodes(adb, nodes, runner=runner)
    except Exception as failure:
        report["preparation_failure"] = _redact_text(str(failure))[:1024]
        _write_json_atomic(evidence_directory / "report.json", report)
        raise
    node_reports = report["nodes"]
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(nodes)) as pool:
            preparations = [
                pool.submit(
                    _prepare_node,
                    adb,
                    node,
                    launch_after_unlock=launch_after_unlock,
                    sleep_screen_after_launch=sleep_screen_after_launch,
                    runner=runner,
                )
                for node in resolved_nodes
            ]
            # Resolve in input order so stable node evidence never depends on thread timing.
            node_reports.extend(preparation.result() for preparation in preparations)
    except Exception as failure:
        report["preparation_failure"] = _redact_text(str(failure))[:1024]
        _write_json_atomic(evidence_directory / "report.json", report)
        raise
    _write_json_atomic(evidence_directory / "report.json", report)

    attempts = report["doctor_attempts"]
    if all(node.get("passed") is True for node in node_reports):
        for attempt_number in range(1, maximum_attempts + 1):
            attempt_path = evidence_directory / f"doctor_attempt_{attempt_number}.json"
            with tempfile.TemporaryDirectory(
                prefix="swing_capture_field_preflight_doctor_"
            ) as raw_directory:
                raw_attempt_path = Path(raw_directory) / "report.json"
                arguments = [str(doctor)]
                if expected_apk is not None:
                    arguments.extend((str(adb), "--expected-apk", str(expected_apk)))
                for node in resolved_nodes:
                    arguments.extend(("--node", f"{node.serial}={node.origin}"))
                arguments.extend(("--json", str(raw_attempt_path), "--require-wireless-adb"))
                if require_monitoring:
                    arguments.append("--require-monitoring")
                result = _run(runner, arguments)
                doctor_report = _read_doctor_report(raw_attempt_path)
                if doctor_report is not None:
                    doctor_report = cast("Mapping[str, object]", _redact_json(doctor_report))
            _write_text_atomic(
                evidence_directory / f"doctor_attempt_{attempt_number}.stdout.txt",
                result.stdout,
            )
            _write_text_atomic(
                evidence_directory / f"doctor_attempt_{attempt_number}.stderr.txt",
                result.stderr,
            )
            if doctor_report is not None:
                _write_json_atomic(attempt_path, doctor_report)
            role_associations, role_association_passed = _role_association_evidence(
                doctor_report, expected_roles
            )
            exact_apk_evidence_passed = _exact_apk_evidence_passed(
                doctor_report, expected_apk_sha256
            )
            lan_diagnostics, lan_diagnostics_passed = _lan_diagnostics_evidence(
                doctor_report, resolved_nodes
            )
            report["role_associations"] = role_associations
            report["lan_diagnostics"] = lan_diagnostics
            attempt_passed = (
                result.returncode == 0
                and doctor_report is not None
                and doctor_report.get("schema_version") == 1
                and doctor_report.get("report_type") == "android_pair_preflight"
                and doctor_report.get("credentials_redacted") is True
                and doctor_report.get("passed") is True
                and role_association_passed
                and exact_apk_evidence_passed is not False
                and lan_diagnostics_passed
            )
            attempts.append(
                {
                    "attempt": attempt_number,
                    "exit_code": result.returncode,
                    "report": attempt_path.name if doctor_report is not None else None,
                    "role_association_passed": role_association_passed,
                    "exact_apk_evidence_passed": exact_apk_evidence_passed,
                    "lan_diagnostics_passed": lan_diagnostics_passed,
                    "failures": _doctor_failures(doctor_report),
                    "passed": attempt_passed,
                }
            )
            report["passed"] = attempt_passed
            _write_json_atomic(evidence_directory / "report.json", report)
            if attempt_passed:
                break
            if attempt_number < maximum_attempts:
                sleeper(retry_seconds)
    return report
