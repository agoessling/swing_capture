"""Isolated Ubuntu 24 compatibility closure for the pinned Playwright WebKit."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Iterable, Mapping
    from pathlib import Path

REQUIRED_SONAMES = (
    "libabsl_synchronization.so.20220623",
    "libavif.so.16",
    "libevent-2.1.so.7",
    "libgav1.so.1",
    "libicudata.so.74",
    "libicui18n.so.74",
    "libicuuc.so.74",
    "librav1e.so.0",
    "libSvtAv1Enc.so.1",
    "libvpx.so.9",
    "libwoff2dec.so.1.0.2",
    "libxml2.so.2",
)


def extract_compatibility_libraries(
    dpkg_deb: Path, archives: Iterable[Path], destination: Path
) -> Path:
    """Extracts pinned .debs into destination and returns the verified library path."""
    if not dpkg_deb.is_file() or not os.access(dpkg_deb, os.X_OK):
        message = f"dpkg-deb is not executable: {dpkg_deb}"
        raise ValueError(message)
    archive_list = tuple(archives)
    if not archive_list:
        message = "at least one compatibility archive is required"
        raise ValueError(message)
    for archive in archive_list:
        if not archive.is_file():
            message = f"compatibility archive is not a regular file: {archive}"
            raise ValueError(message)
    destination.mkdir(parents=True, exist_ok=False)
    for archive in archive_list:
        subprocess.run(
            [str(dpkg_deb), "-x", str(archive), str(destination)],
            check=True,
        )
    library_path = destination / "usr/lib/x86_64-linux-gnu"
    missing = [name for name in REQUIRED_SONAMES if not (library_path / name).exists()]
    if missing:
        raise RuntimeError(
            "Ubuntu 24 WebKit compatibility closure is incomplete: " + ", ".join(missing)
        )
    return library_path


def webkit_launch_environment(
    base: Mapping[str, str], webkit_root: Path, compatibility_library_path: Path
) -> dict[str, str]:
    """Builds the environment shared by Playwright validation and its WebKit child."""
    wpe_root = webkit_root / "minibrowser-wpe"
    executable = wpe_root / "bin/MiniBrowser"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        message = f"pinned WebKit MiniBrowser is not executable: {executable}"
        raise ValueError(message)
    if not compatibility_library_path.is_dir():
        message = (
            f"WebKit compatibility library path is not a directory: {compatibility_library_path}"
        )
        raise ValueError(message)
    inherited = base.get("LD_LIBRARY_PATH", "")
    library_entries = [
        str(compatibility_library_path),
        str(wpe_root / "lib"),
        str(wpe_root / "sys/lib"),
    ]
    if inherited:
        library_entries.append(inherited)
    environment = dict(base)
    environment.update(
        {
            "LD_LIBRARY_PATH": ":".join(library_entries),
            "WEBKIT_EXEC_PATH": str(wpe_root / "bin"),
            "WEBKIT_INJECTED_BUNDLE_PATH": str(wpe_root / "lib"),
            "WEBKIT_INSPECTOR_RESOURCES_PATH": str(wpe_root / "share"),
        }
    )
    return environment
