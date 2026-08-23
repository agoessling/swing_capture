"""Collect and validate fresh Android product-floor inventories over Bazel-provided adb."""

# Evidence failures retain device and field context at the exact validation boundary. Pair
# collection also deliberately catches each future independently to preserve both-node evidence.
# ruff: noqa: EM101, EM102, PERF203, PLR0913, TRY003, TRY004

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import TypeAlias, cast, final

from tools.android_apk_installer import parse_package_path, sha256_file

PACKAGE_NAME = "com.agoessling.swingcapture"
REPORT_PATH = "files/reports/latest.json"
SERIAL_PATTERN = re.compile(r"^[A-Za-z0-9._:-]+$")
PAIR_NODE_COUNT = 2
MINIMUM_API_LEVEL = 34
MINIMUM_OPENGL_ES_VERSION = 0x00030001
DEFAULT_MAXIMUM_AGE_SECONDS = 600
MAXIMUM_AGE_SECONDS = 3_600
SHA256_BUFFER_BYTES = 1024 * 1024
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
POSE_INPUT_WIDTH = 640
POSE_INPUT_HEIGHT = 360
POSE_CADENCE_HZ = 5


@dataclasses.dataclass(frozen=True)
class Check:
    """One stable product-floor evidence assertion."""

    name: str
    passed: bool
    message: str

    def as_json(self) -> dict[str, object]:
        """Return this check in the stable report representation."""
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class CollectedInventory:
    """A validated report bound to the adb transport that supplied it."""

    serial: str
    report: Mapping[str, object]
    installed_apk_sha256: str
    checks: tuple[Check, ...]


CommandRunner: TypeAlias = Callable[[Sequence[str]], str]
ApkHasher: TypeAlias = Callable[[Sequence[str]], str]


def _mapping(value: object) -> Mapping[str, object]:
    return cast("Mapping[str, object]", value) if isinstance(value, Mapping) else {}


def _sequence(value: object) -> Sequence[object]:
    return cast("Sequence[object]", value) if isinstance(value, list) else ()


def _parse_utc(value: object) -> dt.datetime | None:
    if not isinstance(value, str):
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else None


def _parse_hex(value: object) -> int | None:
    if not isinstance(value, str) or re.fullmatch(r"0x[0-9a-fA-F]{8}", value) is None:
        return None
    return int(value, 16)


def validate_inventory(
    serial: str,
    report: Mapping[str, object],
    *,
    now: dt.datetime,
    maximum_age_seconds: int,
) -> tuple[Check, ...]:
    """Validate the exact evidence needed by the supported-device admission policy."""
    device = _mapping(report.get("device"))
    power = _mapping(report.get("power"))
    floor = _mapping(report.get("product_floor_assessment"))
    measured = _mapping(floor.get("measured"))
    pose = _mapping(floor.get("production_pose"))
    created = _parse_utc(report.get("created_at_utc"))
    age_seconds = None if created is None else (now - created).total_seconds()
    issues = _sequence(floor.get("issues"))
    gles = _parse_hex(measured.get("required_opengl_es_version_hex"))
    platform_message = f"api={measured.get('api_level')!s}"
    platform_message += f" OpenGL_ES={measured.get('required_opengl_es_version_hex')!s}"
    checks = [
        Check(
            "report.contract",
            report.get("schema_version") == 1
            and report.get("report_type") == "android_capability_inventory"
            and isinstance(report.get("node_id"), str)
            and bool(report.get("node_id"))
            and report.get("role") in {"face_on", "down_the_line"}
            and report.get("capture_profile") == "720p240",
            "inventory schema, node identity, role, and 720p240 profile are present",
        ),
        Check(
            "report.freshness",
            age_seconds is not None and 0 <= age_seconds <= maximum_age_seconds,
            "created_at_utc age is "
            + ("invalid" if age_seconds is None else f"{age_seconds:.1f} seconds"),
        ),
        Check(
            "device.identity",
            isinstance(device.get("model"), str)
            and bool(device.get("model"))
            and isinstance(device.get("fingerprint"), str)
            and bool(device.get("fingerprint")),
            f"model={device.get('model')!s} api={device.get('api_level')!s}",
        ),
        Check(
            "product_floor.assessment",
            floor.get("schema_version") == 1
            and floor.get("minimum_api_level") == MINIMUM_API_LEVEL
            and floor.get("selected_profile") == "720p240"
            and floor.get("ready") is True
            and floor.get("probe_succeeded") is True
            and not issues,
            "720p240 product-floor assessment is ready with no issues",
        ),
        Check(
            "product_floor.camera",
            measured.get("camera_permission") is True
            and measured.get("rear_realtime_camera_720p240") is True,
            "rear realtime fixed-240 camera, 640x360 standby, and permission are admitted",
        ),
        Check(
            "product_floor.encoder",
            measured.get("hardware_avc_720p240") is True,
            "hardware AVC encoder admits 720p240",
        ),
        Check(
            "product_floor.audio",
            measured.get("audio_permission") is True and measured.get("pcm16_mono_48khz") is True,
            "48 kHz mono PCM16 input and permission are admitted",
        ),
        Check(
            "product_floor.platform",
            isinstance(measured.get("api_level"), int)
            and cast("int", measured.get("api_level")) >= MINIMUM_API_LEVEL
            and gles is not None
            and gles >= MINIMUM_OPENGL_ES_VERSION,
            platform_message,
        ),
        Check(
            "product_floor.pose_isolation",
            pose.get("model") == "lite"
            and pose.get("input_width") == POSE_INPUT_WIDTH
            and pose.get("input_height") == POSE_INPUT_HEIGHT
            and pose.get("cadence_hz") == POSE_CADENCE_HZ
            and pose.get("delegate_selection_scope") == "per_node"
            and pose.get("device_fallback_can_affect_peer") is False,
            "production pose is Lite 640x360 at 5 Hz with per-node delegate isolation",
        ),
        Check(
            "device.screen_off",
            power.get("interactive") is False,
            f"screen interactive={power.get('interactive')!s}",
        ),
    ]
    return tuple(dataclasses.replace(check, name=f"{serial}.{check.name}") for check in checks)


def _subprocess_runner(arguments: Sequence[str]) -> str:
    completed = subprocess.run(
        arguments,
        check=True,
        capture_output=True,
        text=True,
        timeout=5,
    )
    return completed.stdout


def _subprocess_apk_hasher(arguments: Sequence[str]) -> str:
    """Pull an installed APK without retaining it and return its host-side digest."""
    digest = hashlib.sha256()
    with tempfile.TemporaryFile() as pulled:
        completed = subprocess.run(
            arguments,
            check=False,
            stdout=pulled,
            stderr=subprocess.PIPE,
            timeout=5,
        )
        completed.check_returncode()
        pulled.seek(0)
        while chunk := pulled.read(SHA256_BUFFER_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def collect_inventory(
    run: CommandRunner,
    *,
    hash_apk: ApkHasher,
    adb: Path,
    serial: str,
    expected_apk_sha256: str,
    now: dt.datetime,
    maximum_age_seconds: int,
) -> CollectedInventory:
    """Read one app-private report without starting, stopping, or arming the phone."""
    if SERIAL_PATTERN.fullmatch(serial) is None:
        raise ValueError("Android serial contains unsupported characters")
    if SHA256_PATTERN.fullmatch(expected_apk_sha256) is None:
        raise ValueError("expected APK SHA-256 is malformed")
    state = run((str(adb), "-s", serial, "get-state")).strip()
    if state != "device":
        raise RuntimeError(f"adb transport for {serial} is not authorized")
    try:
        installed_apk_path = parse_package_path(
            run((str(adb), "-s", serial, "shell", "pm", "path", PACKAGE_NAME))
        )
    except (OSError, ValueError, subprocess.SubprocessError) as failure:
        message = f"capture app on {serial} is absent, split, or has no verifiable base APK"
        raise RuntimeError(message) from failure
    try:
        installed_apk_sha256 = hash_apk(
            (
                str(adb),
                "-s",
                serial,
                "exec-out",
                "run-as",
                PACKAGE_NAME,
                "cat",
                installed_apk_path,
            )
        )
    except (OSError, ValueError, subprocess.SubprocessError) as failure:
        message = f"installed base APK on {serial} could not be pulled and hashed"
        raise RuntimeError(message) from failure
    if SHA256_PATTERN.fullmatch(installed_apk_sha256) is None:
        raise RuntimeError(f"installed base APK on {serial} produced an invalid SHA-256")
    raw = run(
        (
            str(adb),
            "-s",
            serial,
            "exec-out",
            "run-as",
            PACKAGE_NAME,
            "cat",
            REPORT_PATH,
        )
    )
    parsed = cast("object", json.loads(raw))
    if not isinstance(parsed, dict):
        raise ValueError("Android capability report must be a JSON object")
    report = cast("Mapping[str, object]", parsed)
    checks = validate_inventory(
        serial,
        report,
        now=now,
        maximum_age_seconds=maximum_age_seconds,
    )
    checks += (
        Check(
            f"{serial}.apk.exact_identity",
            installed_apk_sha256 == expected_apk_sha256,
            "installed base APK SHA-256 "
            + (
                "matches the expected Bazel APK"
                if installed_apk_sha256 == expected_apk_sha256
                else "does not match the expected Bazel APK"
            ),
        ),
    )
    return CollectedInventory(
        serial=serial,
        report=report,
        installed_apk_sha256=installed_apk_sha256,
        checks=checks,
    )


def resolve_output_path(path: Path) -> Path:
    """Resolve an evidence path relative to the invoking workspace when needed."""
    if path.is_absolute():
        return path
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace) / path if workspace else path


def prepare_output_directory(path: Path) -> None:
    """Create a new evidence directory without replacing a prior qualification."""
    if path.exists():
        if not path.is_dir() or any(path.iterdir()):
            raise ValueError("output directory already contains evidence")
        return
    path.mkdir(parents=True)


@final
class _Arguments(argparse.Namespace):
    """Typed command-line values populated by argparse."""

    def __init__(self) -> None:
        """Initialize defaults that argparse replaces for required options."""
        super().__init__()
        self.adb = Path()
        self.expected_apk = Path()
        self.serial: list[str] = []
        self.output_dir = Path()
        self.maximum_age_seconds = DEFAULT_MAXIMUM_AGE_SECONDS


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("adb", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--expected-apk", required=True, type=Path)
    parser.add_argument("--serial", action="append", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--maximum-age-seconds",
        type=int,
        default=DEFAULT_MAXIMUM_AGE_SECONDS,
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    """Collect the pair concurrently and preserve exact reports plus a summary."""
    options = _parser().parse_args(arguments, namespace=_Arguments())
    if len(options.serial) != PAIR_NODE_COUNT:
        _parser().error("--serial must be specified exactly twice")
    if not 1 <= options.maximum_age_seconds <= MAXIMUM_AGE_SECONDS:
        _parser().error("--maximum-age-seconds must be from 1 through 3600")
    if len(set(options.serial)) != PAIR_NODE_COUNT:
        _parser().error("--serial values must be distinct")

    try:
        expected_apk_sha256 = sha256_file(options.expected_apk)
    except OSError as failure:
        _parser().error(f"expected Bazel APK could not be hashed: {failure}")

    now = dt.datetime.now(dt.UTC)
    output = resolve_output_path(options.output_dir)
    try:
        prepare_output_directory(output)
    except (OSError, ValueError) as failure:
        _parser().error(str(failure))
    inventories: list[CollectedInventory] = []
    failures: list[Check] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=PAIR_NODE_COUNT) as pool:
        futures = {
            serial: pool.submit(
                collect_inventory,
                _subprocess_runner,
                hash_apk=_subprocess_apk_hasher,
                adb=options.adb,
                serial=serial,
                expected_apk_sha256=expected_apk_sha256,
                now=now,
                maximum_age_seconds=options.maximum_age_seconds,
            )
            for serial in options.serial
        }
    for serial in options.serial:
        try:
            inventories.append(futures[serial].result())
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as failure:
            failures.append(Check(name=f"{serial}.collection", passed=False, message=str(failure)))

    checks = failures + [check for inventory in inventories for check in inventory.checks]
    roles = {inventory.report.get("role") for inventory in inventories}
    nodes = {inventory.report.get("node_id") for inventory in inventories}
    checks.append(
        Check(
            "pair.identity_and_roles",
            len(inventories) == PAIR_NODE_COUNT
            and len(nodes) == PAIR_NODE_COUNT
            and roles == {"face_on", "down_the_line"},
            "reports belong to distinct complementary capture nodes",
        )
    )
    checks.append(
        Check(
            "pair.apk_exact_identity",
            len(inventories) == PAIR_NODE_COUNT
            and all(
                inventory.installed_apk_sha256 == expected_apk_sha256 for inventory in inventories
            ),
            "both installed base APK SHA-256 values match the expected Bazel APK",
        )
    )
    passed = all(check.passed for check in checks)
    for inventory in inventories:
        (output / f"{inventory.serial}.json").write_text(
            json.dumps(inventory.report, indent=2, sort_keys=True) + "\n"
        )
    summary = {
        "schema_version": 1,
        "report_type": "android_pair_product_floor",
        "created_at_utc": now.isoformat().replace("+00:00", "Z"),
        "passed": passed,
        "credentials_redacted": True,
        "expected_apk_sha256": expected_apk_sha256,
        "installed_apks": [
            {
                "serial": inventory.serial,
                "sha256": inventory.installed_apk_sha256,
            }
            for inventory in inventories
        ],
        "reports": [f"{inventory.serial}.json" for inventory in inventories],
        "checks": [check.as_json() for check in checks],
    }
    (output / "report.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    for check in checks:
        print(f"[{'PASS' if check.passed else 'FAIL'}] {check.name}: {check.message}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
