"""Hermetic tests for immutable field-evidence publication."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.field_evidence import output


class OutputTest(unittest.TestCase):
    """Verify immutable publication and cleanup after interrupted writes."""

    def test_publication_is_complete_and_refuses_replacement(self) -> None:
        """A complete publication cannot be replaced."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested" / "evidence.json"
            output.publish_json_exclusive(path, {"passed": True})
            first = path.read_bytes()

            with self.assertRaisesRegex(FileExistsError, "refusing to overwrite"):
                output.publish_json_exclusive(path, {"passed": False})

            self.assertEqual(path.read_bytes(), first)
            self.assertEqual(json.loads(first), {"passed": True})

    def test_failed_publish_leaves_neither_partial_output_nor_temporary_file(self) -> None:
        """An interrupted publication removes all partial evidence."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "evidence.json"
            with (
                mock.patch.object(
                    output.os,  # pyright: ignore[reportPrivateLocalImportUsage] - fault seam.
                    "link",
                    side_effect=OSError("interrupted"),
                ),
                self.assertRaisesRegex(OSError, "interrupted"),
            ):
                output.publish_json_exclusive(path, {"passed": True})

            self.assertFalse(path.exists())
            self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
