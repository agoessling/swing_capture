"""Deterministic tests for exact-match Android APK installation."""

# These standalone test functions preserve assertion context at each rejection boundary.
# ruff: noqa: D103, EM101, PT017, TRY003

from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from tools.android_apk_installer import (
    CommandOutcome,
    ensure_apk,
    parse_package_path,
    parse_sha256,
    resolve_input_path,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence


SERIAL = "22181FDF6005QH"
PACKAGE = "com.agoessling.swingcapture"
REMOTE_PATH = "/data/app/~~token/package-token/base.apk"


class _FakeAdb:
    def __init__(self, *, before: str | None, after: str | None = None) -> None:
        self.installed_sha: str | None = before
        self.after: str | None = after
        self.commands: list[tuple[str, ...]] = []

    def __call__(self, arguments: Sequence[str]) -> CommandOutcome:
        command = tuple(arguments)
        self.commands.append(command)
        if command[-4:-1] == ("shell", "pm", "path"):
            if self.installed_sha is None:
                return CommandOutcome(1, "", "package not found")
            return CommandOutcome(0, f"package:{REMOTE_PATH}\n")
        if command[-3:-1] == ("shell", "sha256sum"):
            if self.installed_sha is None:
                return CommandOutcome(1, "", "not installed")
            return CommandOutcome(0, f"{self.installed_sha}  {REMOTE_PATH}\n")
        if "install" in command:
            self.installed_sha = self.after
            return CommandOutcome(0, "Success\n")
        message = f"unexpected command: {command}"
        raise AssertionError(message)


def _fixture(tmp_path: Path) -> tuple[Path, Path, str]:
    adb = tmp_path / "adb"
    adb.write_bytes(b"adb")
    apk = tmp_path / "capture.apk"
    apk.write_bytes(b"exact apk bytes")
    return adb, apk, hashlib.sha256(apk.read_bytes()).hexdigest()


def test_exact_match_skips_install(tmp_path: Path) -> None:
    adb, apk, digest = _fixture(tmp_path)
    fake = _FakeAdb(before=digest)
    result = ensure_apk(
        fake,
        adb=adb,
        serial=SERIAL,
        package=PACKAGE,
        apk=apk,
        skip_exact_match=True,
        monotonic=lambda: 1.0,
    )
    assert not result.installed
    assert result.reason == "exact_match"
    assert not any("install" in command for command in fake.commands)


def test_changed_apk_installs_and_verifies(tmp_path: Path) -> None:
    adb, apk, digest = _fixture(tmp_path)
    fake = _FakeAdb(before="0" * 64, after=digest)
    result = ensure_apk(
        fake,
        adb=adb,
        serial=SERIAL,
        package=PACKAGE,
        apk=apk,
        skip_exact_match=True,
        monotonic=lambda: 2.0,
    )
    assert result.installed
    assert result.reason == "different_or_unverifiable"
    assert sum("install" in command for command in fake.commands) == 1


def test_required_exact_match_is_read_only(tmp_path: Path) -> None:
    adb, apk, digest = _fixture(tmp_path)
    exact = _FakeAdb(before=digest)
    result = ensure_apk(
        exact,
        adb=adb,
        serial=SERIAL,
        package=PACKAGE,
        apk=apk,
        skip_exact_match=False,
        require_exact_match=True,
    )
    assert not result.installed
    assert result.reason == "exact_match"
    assert not any("install" in command for command in exact.commands)

    different = _FakeAdb(before="0" * 64, after=digest)
    try:
        ensure_apk(
            different,
            adb=adb,
            serial=SERIAL,
            package=PACKAGE,
            apk=apk,
            skip_exact_match=False,
            require_exact_match=True,
        )
    except RuntimeError as failure:
        assert "does not exactly match" in str(failure)
    else:
        raise AssertionError("read-only APK qualification accepted a different install")
    assert not any("install" in command for command in different.commands)


def test_default_lane_installs_even_an_exact_match(tmp_path: Path) -> None:
    adb, apk, digest = _fixture(tmp_path)
    fake = _FakeAdb(before=digest, after=digest)
    result = ensure_apk(
        fake,
        adb=adb,
        serial=SERIAL,
        package=PACKAGE,
        apk=apk,
        skip_exact_match=False,
        monotonic=lambda: 3.0,
    )
    assert result.installed
    assert result.reason == "forced"


def test_failed_post_install_identity_is_rejected(tmp_path: Path) -> None:
    adb, apk, _ = _fixture(tmp_path)
    fake = _FakeAdb(before=None, after="f" * 64)
    try:
        ensure_apk(
            fake,
            adb=adb,
            serial=SERIAL,
            package=PACKAGE,
            apk=apk,
            skip_exact_match=True,
        )
    except RuntimeError as failure:
        assert "does not exactly match" in str(failure)
    else:
        raise AssertionError("mismatched installed APK was accepted")


def test_identity_parsers_reject_ambiguous_or_substituted_paths() -> None:
    assert parse_package_path(f"package:{REMOTE_PATH}\n") == REMOTE_PATH
    try:
        parse_package_path(f"package:{REMOTE_PATH}\npackage:/data/app/other/base.apk\n")
    except ValueError:
        pass
    else:
        raise AssertionError("multiple installed base APKs were accepted")
    try:
        parse_package_path(
            f"package:{REMOTE_PATH}\npackage:/data/app/package-token/split_config.arm64_v8a.apk\n"
        )
    except ValueError:
        pass
    else:
        raise AssertionError("a split APK install was accepted as an exact monolithic match")
    try:
        parse_package_path("package:/data/app/package-token;id/base.apk\n")
    except ValueError:
        pass
    else:
        raise AssertionError("a remote-shell metacharacter was accepted in the package path")
    try:
        parse_sha256(f"{'a' * 64}  /data/app/other/base.apk", REMOTE_PATH)
    except ValueError:
        pass
    else:
        raise AssertionError("checksum for another path was accepted")


def test_split_install_is_replaced_instead_of_skipped(tmp_path: Path) -> None:
    adb, apk, digest = _fixture(tmp_path)

    class SplitThenMonolithic(_FakeAdb):
        def __call__(  # pyright: ignore[reportImplicitOverride]
            self, arguments: Sequence[str]
        ) -> CommandOutcome:
            command = tuple(arguments)
            if command[-4:-1] == ("shell", "pm", "path") and self.installed_sha == "split":
                self.commands.append(command)
                return CommandOutcome(
                    0,
                    "".join(
                        (
                            f"package:{REMOTE_PATH}\n",
                            "package:/data/app/package-token/split_config.arm64_v8a.apk\n",
                        )
                    ),
                )
            return super().__call__(arguments)

    fake = SplitThenMonolithic(before="split", after=digest)
    result = ensure_apk(
        fake,
        adb=adb,
        serial=SERIAL,
        package=PACKAGE,
        apk=apk,
        skip_exact_match=True,
    )
    assert result.installed
    assert result.installed_before_sha256 is None


def test_workspace_relative_apk_path(tmp_path: Path) -> None:
    previous = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    os.environ["BUILD_WORKSPACE_DIRECTORY"] = str(tmp_path)
    try:
        assert resolve_input_path(Path("bazel-bin/app.apk")) == tmp_path / "bazel-bin/app.apk"
    finally:
        if previous is None:
            del os.environ["BUILD_WORKSPACE_DIRECTORY"]
        else:
            os.environ["BUILD_WORKSPACE_DIRECTORY"] = previous


def _run_with_temporary_directory(test: Callable[[Path], None]) -> None:
    with tempfile.TemporaryDirectory(prefix="android-apk-installer-test-") as directory:
        test(Path(directory))


if __name__ == "__main__":
    _run_with_temporary_directory(test_exact_match_skips_install)
    _run_with_temporary_directory(test_changed_apk_installs_and_verifies)
    _run_with_temporary_directory(test_required_exact_match_is_read_only)
    _run_with_temporary_directory(test_default_lane_installs_even_an_exact_match)
    _run_with_temporary_directory(test_failed_post_install_identity_is_rejected)
    test_identity_parsers_reject_ambiguous_or_substituted_paths()
    _run_with_temporary_directory(test_split_install_is_replaced_instead_of_skipped)
    _run_with_temporary_directory(test_workspace_relative_apk_path)
