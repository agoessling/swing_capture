"""Tests for selective extraction of the Daheng SDK installer payload."""

from __future__ import annotations

import io
import tarfile
import tempfile
import unittest
from pathlib import Path

from third_party.daheng import extract_sdk


class ExtractSdkTest(unittest.TestCase):
    """Verify secure, selective extraction from the vendor installer."""

    def _write_installer(self, path: Path, members: dict[str, bytes]) -> None:
        payload = io.BytesIO()
        with tarfile.open(fileobj=payload, mode="w:gz") as archive:
            for name, contents in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(contents)
                info.mode = 0o640
                archive.addfile(info, io.BytesIO(contents))
        path.write_bytes(
            b"#!/bin/sh\necho vendor installer\n__ARCHIVE_FOLLOWS__\n" + payload.getvalue()
        )

    def test_extracts_only_explicitly_required_regular_files(self) -> None:
        """Extract only allow-listed regular files."""
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            installer = root / "Galaxy_camera.run"
            output = root / "output"
            self._write_installer(
                installer,
                {
                    "Galaxy_camera/inc/GxIAPI.h": b"header",
                    "Galaxy_camera/lib/x86_64/libgxiapi.so": b"library",
                    "Galaxy_camera/unneeded/installer.sh": b"do not extract",
                },
            )

            extract_sdk.extract_required_files(
                installer,
                output,
                required_members=(
                    "Galaxy_camera/inc/GxIAPI.h",
                    "Galaxy_camera/lib/x86_64/libgxiapi.so",
                ),
            )

            self.assertEqual((output / "Galaxy_camera/inc/GxIAPI.h").read_bytes(), b"header")
            self.assertEqual(
                (output / "Galaxy_camera/lib/x86_64/libgxiapi.so").read_bytes(),
                b"library",
            )
            self.assertFalse((output / "Galaxy_camera/unneeded").exists())

    def test_rejects_a_missing_archive_marker(self) -> None:
        """Reject an input that is not a Daheng self-extracting installer."""
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            installer = root / "Galaxy_camera.run"
            installer.write_bytes(b"not a self-extracting archive")

            with self.assertRaisesRegex(ValueError, "archive marker"):
                extract_sdk.extract_required_files(
                    installer,
                    root / "output",
                    required_members=("required",),
                )

    def test_rejects_missing_required_members(self) -> None:
        """Reject an installer payload missing an explicitly required file."""
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            installer = root / "Galaxy_camera.run"
            self._write_installer(installer, {"present": b"contents"})

            with self.assertRaisesRegex(ValueError, "missing"):
                extract_sdk.extract_required_files(
                    installer,
                    root / "output",
                    required_members=("present", "absent"),
                )


if __name__ == "__main__":
    unittest.main()
