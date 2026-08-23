"""Runs the H.264 browser gate with pinned WebKit and isolated Ubuntu 24 libraries."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys

from web.webkit_ubuntu24_compat import (
    extract_compatibility_libraries,
    webkit_launch_environment,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser-test", type=Path, required=True)
    parser.add_argument("--browser-launcher", type=Path, required=True)
    parser.add_argument("--webkit-root", type=Path, required=True)
    parser.add_argument("--dpkg-deb", type=Path, default=Path("/usr/bin/dpkg-deb"))
    parser.add_argument("--deb", type=Path, action="append", required=True)
    parser.add_argument("browser_arguments", nargs=argparse.REMAINDER)
    return parser


def main() -> int:
    arguments = _parser().parse_args()
    browser_test = arguments.browser_test.resolve()
    # Preserve the launcher path inside this target's runfiles tree. The
    # rules_python bootstrap uses that path to recover its own sources when
    # Playwright starts it as a nested browser process.
    browser_launcher = Path(os.path.abspath(arguments.browser_launcher))
    webkit_root = arguments.webkit_root.resolve()
    if not browser_test.is_file() or not os.access(browser_test, os.X_OK):
        raise ValueError(f"browser test is not executable: {browser_test}")
    if not browser_launcher.is_file() or not os.access(browser_launcher, os.X_OK):
        raise ValueError(f"WebKit browser launcher is not executable: {browser_launcher}")
    browser_runfiles_root = Path(f"{browser_test.resolve()}.runfiles") / "_main"
    if not browser_runfiles_root.is_dir():
        raise ValueError(f"browser test runfiles root is not a directory: {browser_runfiles_root}")
    test_tmpdir = Path(os.environ["TEST_TMPDIR"]).resolve()
    compatibility_library_path = extract_compatibility_libraries(
        arguments.dpkg_deb.resolve(),
        (archive.resolve() for archive in arguments.deb),
        test_tmpdir / "webkit-ubuntu24-compat",
    )
    environment = webkit_launch_environment(os.environ, webkit_root, compatibility_library_path)
    # Do not leak this Python test's runfiles identity into the nested rules_js
    # launcher; it must discover the runfiles tree adjacent to browser_test.
    for variable in (
        "JAVA_RUNFILES",
        "PYTHON_RUNFILES",
        "RUNFILES_DIR",
        "RUNFILES_MANIFEST_FILE",
        "TEST_SRCDIR",
    ):
        environment.pop(variable, None)
    environment.update(
        {
            "SWING_CAPTURE_BROWSER_NAME": "webkit",
            "SWING_CAPTURE_BROWSER_EXECUTABLE": str(browser_launcher),
            "SWING_CAPTURE_REQUIRE_H264_DECODE": "1",
            "SWING_CAPTURE_WEBKIT_COMPAT_LIB": str(compatibility_library_path),
            "SWING_CAPTURE_WEBKIT_ROOT": str(webkit_root),
        }
    )
    browser_arguments = list(arguments.browser_arguments)
    if browser_arguments and browser_arguments[0] == "--":
        browser_arguments.pop(0)
    if not browser_arguments:
        raise ValueError("browser test arguments are required after --")
    if os.environ.get("SWING_CAPTURE_WEBKIT_DEBUG") == "1":
        print(
            f"WebKit browser cwd={browser_runfiles_root} argv={browser_arguments!r}",
            file=sys.stderr,
        )
    # The rules_js launcher resolves its sources and node_modules relative to
    # its own runfiles tree. A wrapper test has a different runfiles root, so
    # retain the browser target's coherent tree as its working directory.
    os.chdir(browser_runfiles_root)
    os.execve(browser_test, [str(browser_test), *browser_arguments], environment)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
