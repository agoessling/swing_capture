"""Tests for current-APK Android product-floor evidence collection."""

# Test names carry the scenarios, and explicit exception checks retain failure context.
# ruff: noqa: D103, EM101, PT017, TRY003

from __future__ import annotations

import datetime as dt
import hashlib
import json
import subprocess
import sys
from typing import TYPE_CHECKING, cast

from tools.android_capability_report import (
    _subprocess_apk_hasher,  # pyright: ignore[reportPrivateUsage]
    collect_inventory,
    prepare_output_directory,
    validate_inventory,
)

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


NOW = dt.datetime(2026, 8, 23, 7, 0, tzinfo=dt.UTC)
EXPECTED_APK = b"expected Bazel APK"
EXPECTED_APK_SHA256 = hashlib.sha256(EXPECTED_APK).hexdigest()
INSTALLED_APK_PATH = "/data/app/~~token/app-token/base.apk"


def _report(
    *, role: str = "face_on", created_at: str = "2026-08-23T06:59:30Z"
) -> dict[str, object]:
    return {
        "schema_version": 1,
        "report_type": "android_capability_inventory",
        "created_at_utc": created_at,
        "node_id": "node-face" if role == "face_on" else "node-dtl",
        "role": role,
        "capture_profile": "720p240",
        "device": {"model": "Pixel 6", "fingerprint": "google/device", "api_level": 36},
        "power": {"interactive": False},
        "product_floor_assessment": {
            "schema_version": 1,
            "minimum_api_level": 34,
            "selected_profile": "720p240",
            "ready": True,
            "probe_succeeded": True,
            "probe_diagnostic": None,
            "measured": {
                "api_level": 36,
                "camera_permission": True,
                "audio_permission": True,
                "rear_realtime_camera_720p240": True,
                "rear_realtime_camera_1080p240": True,
                "hardware_avc_720p240": True,
                "hardware_avc_1080p240": True,
                "pcm16_mono_48khz": True,
                "required_opengl_es_version_hex": "0x00030002",
            },
            "production_pose": {
                "model": "lite",
                "input_width": 640,
                "input_height": 360,
                "cadence_hz": 5,
                "delegate_selection_scope": "per_node",
                "device_fallback_can_affect_peer": False,
            },
            "issues": [],
        },
    }


def test_nominal_inventory_proves_every_product_floor_dimension() -> None:
    checks = validate_inventory("serial-1", _report(), now=NOW, maximum_age_seconds=60)
    assert checks
    assert all(check.passed for check in checks)


def test_each_product_floor_dimension_fails_independently() -> None:
    mutations = (
        ("camera", "camera_permission", False),
        ("camera", "rear_realtime_camera_720p240", False),
        ("encoder", "hardware_avc_720p240", False),
        ("audio", "audio_permission", False),
        ("audio", "pcm16_mono_48khz", False),
        ("platform", "api_level", 33),
        ("platform", "required_opengl_es_version_hex", "0x00030000"),
    )
    for check_suffix, field, replacement in mutations:
        report = _report()
        assessment = cast("dict[str, object]", report["product_floor_assessment"])
        measured = cast("dict[str, object]", assessment["measured"])
        measured[field] = replacement
        checks = validate_inventory("serial-1", report, now=NOW, maximum_age_seconds=60)
        failed = {check.name for check in checks if not check.passed}
        assert f"serial-1.product_floor.{check_suffix}" in failed


def test_stale_incomplete_and_cross_device_pose_fallback_reports_are_rejected() -> None:
    stale = _report(created_at="2026-08-23T06:00:00Z")
    assert not next(
        check
        for check in validate_inventory("s", stale, now=NOW, maximum_age_seconds=60)
        if check.name.endswith("freshness")
    ).passed

    incomplete = _report()
    del incomplete["product_floor_assessment"]
    assert any(
        not check.passed
        for check in validate_inventory("s", incomplete, now=NOW, maximum_age_seconds=60)
    )

    coupled = _report()
    assessment = cast("dict[str, object]", coupled["product_floor_assessment"])
    production_pose = cast("dict[str, object]", assessment["production_pose"])
    production_pose["device_fallback_can_affect_peer"] = True
    assert not next(
        check
        for check in validate_inventory("s", coupled, now=NOW, maximum_age_seconds=60)
        if check.name.endswith("pose_isolation")
    ).passed


def test_collection_uses_private_report_without_mutating_phone(tmp_path: Path) -> None:
    commands: list[tuple[str, ...]] = []

    def run(arguments: Sequence[str]) -> str:
        command = tuple(arguments)
        commands.append(command)
        if command[-1] == "get-state":
            return "device\n"
        if command[-3:] == ("pm", "path", "com.agoessling.swingcapture"):
            return f"package:{INSTALLED_APK_PATH}\n"
        if command[-2:] == ("cat", "files/reports/latest.json"):
            return json.dumps(_report())
        raise AssertionError(command)

    def hash_apk(arguments: Sequence[str]) -> str:
        command = tuple(arguments)
        commands.append(command)
        assert command[-4:] == (
            "run-as",
            "com.agoessling.swingcapture",
            "cat",
            INSTALLED_APK_PATH,
        )
        return EXPECTED_APK_SHA256

    adb = tmp_path / "adb"
    inventory = collect_inventory(
        run,
        hash_apk=hash_apk,
        adb=adb,
        serial="serial-1",
        expected_apk_sha256=EXPECTED_APK_SHA256,
        now=NOW,
        maximum_age_seconds=60,
    )
    assert inventory.report["report_type"] == "android_capability_inventory"
    assert inventory.installed_apk_sha256 == EXPECTED_APK_SHA256
    assert all(check.passed for check in inventory.checks)
    assert not any("start" in command or "stop" in command for command in commands)


def test_installed_apk_is_hashed_from_pulled_bytes() -> None:
    digest = _subprocess_apk_hasher(
        (
            sys.executable,
            "-c",
            "import sys; sys.stdout.buffer.write(b'expected Bazel APK')",
        )
    )
    assert digest == EXPECTED_APK_SHA256


def test_collection_rejects_installed_apk_digest_mismatch(tmp_path: Path) -> None:
    def run(arguments: Sequence[str]) -> str:
        command = tuple(arguments)
        if command[-1] == "get-state":
            return "device\n"
        if command[-3:] == ("pm", "path", "com.agoessling.swingcapture"):
            return f"package:{INSTALLED_APK_PATH}\n"
        if command[-2:] == ("cat", "files/reports/latest.json"):
            return json.dumps(_report())
        raise AssertionError(command)

    inventory = collect_inventory(
        run,
        hash_apk=lambda _: hashlib.sha256(b"different APK").hexdigest(),
        adb=tmp_path / "adb",
        serial="serial-1",
        expected_apk_sha256=EXPECTED_APK_SHA256,
        now=NOW,
        maximum_age_seconds=60,
    )
    exact_identity = next(
        check for check in inventory.checks if check.name.endswith("apk.exact_identity")
    )
    assert not exact_identity.passed


def test_collection_rejects_missing_installed_apk(tmp_path: Path) -> None:
    def run(arguments: Sequence[str]) -> str:
        command = tuple(arguments)
        if command[-1] == "get-state":
            return "device\n"
        if command[-3:] == ("pm", "path", "com.agoessling.swingcapture"):
            return ""
        raise AssertionError(command)

    try:
        collect_inventory(
            run,
            hash_apk=lambda _: EXPECTED_APK_SHA256,
            adb=tmp_path / "adb",
            serial="serial-1",
            expected_apk_sha256=EXPECTED_APK_SHA256,
            now=NOW,
            maximum_age_seconds=60,
        )
    except RuntimeError as failure:
        assert "absent, split, or has no verifiable base APK" in str(failure)
    else:
        raise AssertionError("missing installed APK was accepted")


def test_collection_rejects_unverifiable_installed_apk(tmp_path: Path) -> None:
    def run(arguments: Sequence[str]) -> str:
        command = tuple(arguments)
        if command[-1] == "get-state":
            return "device\n"
        if command[-3:] == ("pm", "path", "com.agoessling.swingcapture"):
            return f"package:{INSTALLED_APK_PATH}\n"
        raise AssertionError(command)

    def fail_pull(arguments: Sequence[str]) -> str:
        raise subprocess.CalledProcessError(1, arguments)

    try:
        collect_inventory(
            run,
            hash_apk=fail_pull,
            adb=tmp_path / "adb",
            serial="serial-1",
            expected_apk_sha256=EXPECTED_APK_SHA256,
            now=NOW,
            maximum_age_seconds=60,
        )
    except RuntimeError as failure:
        assert str(failure) == "installed base APK on serial-1 could not be pulled and hashed"
    else:
        raise AssertionError("unverifiable installed APK was accepted")


def test_evidence_directory_never_overwrites_a_prior_run(tmp_path: Path) -> None:
    new_directory = tmp_path / "new"
    prepare_output_directory(new_directory)
    assert new_directory.is_dir()
    prepare_output_directory(new_directory)
    (new_directory / "report.json").write_text("prior evidence")
    try:
        prepare_output_directory(new_directory)
    except ValueError as failure:
        assert "already contains evidence" in str(failure)
    else:
        raise AssertionError("prior product-floor evidence was accepted for overwrite")
