"""Safely extracts required files from Daheng's self-extracting installer."""

from __future__ import annotations

import argparse
import shutil
import tarfile
from pathlib import Path
from typing import TYPE_CHECKING, cast

if TYPE_CHECKING:
    from collections.abc import Iterable

ARCHIVE_MARKER = b"__ARCHIVE_FOLLOWS__"
REQUIRED_MEMBERS = (
    "Galaxy_camera/inc/DxImageProc.h",
    "Galaxy_camera/inc/DxMediaProcDef.h",
    "Galaxy_camera/inc/GXDef.h",
    "Galaxy_camera/inc/GXErrorList.h",
    "Galaxy_camera/inc/GxIAPI.h",
    "Galaxy_camera/inc/GxIAPILegacy.h",
    "Galaxy_camera/inc/GxPixelFormat.h",
    "Galaxy_camera/lib/x86_64/GxU3VTL.cti",
    "Galaxy_camera/lib/x86_64/libgxiapi.so",
    "Galaxy_camera/lib/x86_64/libgxlogutil.so",
)


def extract_required_files(
    installer: Path,
    output_root: Path,
    required_members: Iterable[str] = REQUIRED_MEMBERS,
) -> None:
    """Extract the explicitly required SDK files without running the installer."""
    required = frozenset(required_members)
    extracted: set[str] = set()

    with installer.open("rb") as installer_file:
        for line in installer_file:
            if line.rstrip(b"\r\n") == ARCHIVE_MARKER:
                break
        else:
            message = f"{installer} does not contain the Daheng archive marker"
            raise ValueError(message)

        with tarfile.open(fileobj=installer_file, mode="r|gz") as archive:
            for member in archive:
                if member.name not in required:
                    continue
                if not member.isfile():
                    message = f"required SDK member is not a regular file: {member.name}"
                    raise ValueError(message)
                if member.name in extracted:
                    message = f"duplicate SDK member: {member.name}"
                    raise ValueError(message)

                source = archive.extractfile(member)
                if source is None:
                    message = f"cannot read SDK member: {member.name}"
                    raise ValueError(message)
                destination = output_root / member.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                with source, destination.open("wb") as destination_file:
                    shutil.copyfileobj(source, destination_file)
                destination.chmod(member.mode & 0o777)
                extracted.add(member.name)

    missing = sorted(required - extracted)
    if missing:
        raise ValueError("Daheng SDK payload is missing: " + ", ".join(missing))


def main() -> None:
    """Extract the files requested by the Bazel repository rule."""
    parser = argparse.ArgumentParser()
    parser.add_argument("installer", type=Path)
    parser.add_argument("output_root", type=Path)
    args = parser.parse_args()
    extract_required_files(
        cast("Path", args.installer),
        cast("Path", args.output_root),
    )


if __name__ == "__main__":
    main()
