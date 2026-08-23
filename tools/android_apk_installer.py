"""Install an Android APK, optionally skipping an exact installed match."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import subprocess
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import TypeAlias, cast

PACKAGE_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$")
SERIAL_PATTERN = re.compile(r"^[A-Za-z0-9._:-]+$")
PACKAGE_PATH_PATTERN = re.compile(r"^package:(/data/app/[A-Za-z0-9_./~+=-]+/base\.apk)$")
SHA256_PATTERN = re.compile(r"^([0-9a-f]{64})\s+([^\s]+)$")
SHA256_BUFFER_BYTES = 1024 * 1024
MAXIMUM_DEADLINE_SECONDS = 15


@dataclasses.dataclass(frozen=True)
class CommandOutcome:
    """Bounded subprocess result used by the installer and its policy tests."""

    exit_code: int
    stdout: str
    stderr: str = ""


CommandRunner: TypeAlias = Callable[[Sequence[str]], CommandOutcome]


@dataclasses.dataclass(frozen=True)
class InstalledApk:
    """Identity of one installed base APK."""

    path: str
    sha256: str


@dataclasses.dataclass(frozen=True)
class InstallResult:
    """Machine-readable install decision and verified final identity."""

    installed: bool
    reason: str
    local_sha256: str
    installed_before_sha256: str | None
    installed_after_sha256: str
    elapsed_ms: int

    def as_json(self) -> dict[str, object]:
        """Return stable evidence without exposing device-private paths."""
        return dataclasses.asdict(self)


def sha256_file(path: Path) -> str:
    """Hash an APK without loading it into memory."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(SHA256_BUFFER_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def parse_package_path(output: str) -> str:
    """Require a monolithic install containing exactly one base APK path."""
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    match = PACKAGE_PATH_PATTERN.fullmatch(lines[0]) if len(lines) == 1 else None
    if match is None:
        message = "pm path did not return one monolithic base APK"
        raise ValueError(message)
    return match.group(1)


def parse_sha256(output: str, expected_path: str) -> str:
    """Parse sha256sum output and bind it to the requested installed path."""
    match = SHA256_PATTERN.fullmatch(output.strip())
    if match is None or match.group(2) != expected_path:
        message = "installed APK sha256 output is malformed or names another path"
        raise ValueError(message)
    return match.group(1)


def probe_installed_apk(
    run: CommandRunner,
    *,
    adb: Path,
    serial: str,
    package: str,
) -> InstalledApk | None:
    """Return the installed base APK identity, or None when absent/unreadable."""
    package_path = run((str(adb), "-s", serial, "shell", "pm", "path", package))
    if package_path.exit_code != 0:
        return None
    try:
        path = parse_package_path(package_path.stdout)
    except ValueError:
        return None
    checksum = run((str(adb), "-s", serial, "shell", "sha256sum", path))
    if checksum.exit_code != 0:
        return None
    try:
        return InstalledApk(path=path, sha256=parse_sha256(checksum.stdout, path))
    except ValueError:
        return None


def ensure_apk(  # noqa: PLR0913 - explicit install policy inputs keep call sites auditable.
    run: CommandRunner,
    *,
    adb: Path,
    serial: str,
    package: str,
    apk: Path,
    skip_exact_match: bool,
    require_exact_match: bool = False,
    monotonic: Callable[[], float] = time.monotonic,
) -> InstallResult:
    """Install unless an explicitly allowed, byte-exact installed APK is present.

    ``require_exact_match`` is the read-only qualification lane: it rejects an absent or
    different installed APK without invoking ``adb install``.  Reboot/lifecycle tests use this
    lane so qualifying a durable installation cannot itself replace that installation.
    """
    if not adb.is_file() or not apk.is_file():
        message = "adb and APK must be regular files"
        raise ValueError(message)
    if SERIAL_PATTERN.fullmatch(serial) is None or PACKAGE_PATTERN.fullmatch(package) is None:
        message = "Android serial or package name is invalid"
        raise ValueError(message)

    started = monotonic()
    local_sha256 = sha256_file(apk)
    before = probe_installed_apk(run, adb=adb, serial=serial, package=package)
    if (
        (skip_exact_match or require_exact_match)
        and before is not None
        and before.sha256 == local_sha256
    ):
        return InstallResult(
            installed=False,
            reason="exact_match",
            local_sha256=local_sha256,
            installed_before_sha256=before.sha256,
            installed_after_sha256=before.sha256,
            elapsed_ms=max(0, round((monotonic() - started) * 1000)),
        )

    if require_exact_match:
        message = "installed Android APK does not exactly match the Bazel-built APK"
        raise RuntimeError(message)

    installed = run(
        (
            str(adb),
            "-s",
            serial,
            "install",
            "--no-streaming",
            "-r",
            "-t",
            str(apk),
        )
    )
    if installed.exit_code != 0:
        message = f"adb install failed with exit {installed.exit_code}"
        raise RuntimeError(message)
    after = probe_installed_apk(run, adb=adb, serial=serial, package=package)
    if after is None or after.sha256 != local_sha256:
        message = "installed Android APK does not exactly match the Bazel-built APK"
        raise RuntimeError(message)
    return InstallResult(
        installed=True,
        reason="forced" if not skip_exact_match else "different_or_unverifiable",
        local_sha256=local_sha256,
        installed_before_sha256=None if before is None else before.sha256,
        installed_after_sha256=after.sha256,
        elapsed_ms=max(0, round((monotonic() - started) * 1000)),
    )


def subprocess_runner(deadline: float) -> CommandRunner:
    """Create a runner whose commands share one monotonic deadline."""

    def run(arguments: Sequence[str]) -> CommandOutcome:
        remaining_seconds = deadline - time.monotonic()
        if remaining_seconds <= 0:
            raise subprocess.TimeoutExpired(arguments[0], 0)
        completed = subprocess.run(
            arguments,
            check=False,
            capture_output=True,
            text=True,
            timeout=remaining_seconds,
        )
        return CommandOutcome(completed.returncode, completed.stdout, completed.stderr)

    return run


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("adb", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--package", default="com.agoessling.swingcapture")
    parser.add_argument("--deadline-seconds", type=float, default=15.0)
    parser.add_argument(
        "--skip-exact-match",
        action="store_true",
        help="skip installation only when the installed base APK has the exact local SHA-256",
    )
    parser.add_argument(
        "--require-exact-match",
        action="store_true",
        help="fail without installing unless the installed base APK exactly matches",
    )
    return parser


def resolve_input_path(path: Path) -> Path:
    """Resolve an absolute, Bazel-runfile, or invoking-workspace input path."""
    if path.is_absolute():
        return path
    if path.is_file():
        return path
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace) / path if workspace else path


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the exact-identity installer and publish one JSON result."""
    options = _parser().parse_args(arguments)
    skip_exact_match = cast("bool", options.skip_exact_match)
    require_exact_match = cast("bool", options.require_exact_match)
    deadline_seconds = cast("float", options.deadline_seconds)
    if skip_exact_match and require_exact_match:
        _parser().error("--skip-exact-match and --require-exact-match are mutually exclusive")
    if not 0 < deadline_seconds <= MAXIMUM_DEADLINE_SECONDS:
        _parser().error("--deadline-seconds must be greater than zero and no more than 15")
    try:
        result = ensure_apk(
            subprocess_runner(time.monotonic() + deadline_seconds),
            adb=cast("Path", options.adb),
            serial=cast("str", options.serial),
            package=cast("str", options.package),
            apk=resolve_input_path(cast("Path", options.apk)),
            skip_exact_match=skip_exact_match,
            require_exact_match=require_exact_match,
        )
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as failure:
        diagnostic = (
            str(failure)
            if isinstance(failure, (RuntimeError, ValueError))
            else "Android APK installer subprocess failed"
        )
        print(
            json.dumps(
                {"passed": False, "error": diagnostic, "failure_type": type(failure).__name__},
                sort_keys=True,
            )
        )
        return 2
    print(json.dumps({"passed": True, **result.as_json()}, sort_keys=True))
    return 0
