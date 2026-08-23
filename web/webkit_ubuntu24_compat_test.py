"""Tests for the isolated Ubuntu 24 Playwright WebKit compatibility closure."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from web.webkit_ubuntu24_compat import REQUIRED_SONAMES, webkit_launch_environment


class WebKitUbuntu24CompatTest(unittest.TestCase):
    """Verify the pinned launch environment and required SONAME inventory."""

    def test_launch_environment_is_isolated_and_preserves_inherited_path(self) -> None:
        """Prepend compatibility paths without discarding the inherited environment."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            webkit_root = root / "webkit"
            executable = webkit_root / "minibrowser-wpe/bin/MiniBrowser"
            executable.parent.mkdir(parents=True)
            executable.write_text("fixture")
            executable.chmod(0o755)
            (webkit_root / "minibrowser-wpe/lib").mkdir()
            (webkit_root / "minibrowser-wpe/sys/lib").mkdir(parents=True)
            (webkit_root / "minibrowser-wpe/share").mkdir()
            compatibility = root / "compat"
            compatibility.mkdir()

            environment = webkit_launch_environment(
                {"LD_LIBRARY_PATH": "/existing", "KEEP": "yes"},
                webkit_root,
                compatibility,
            )

            self.assertEqual(environment["KEEP"], "yes")
            self.assertEqual(
                environment["LD_LIBRARY_PATH"].split(":"),
                [
                    str(compatibility),
                    str(webkit_root / "minibrowser-wpe/lib"),
                    str(webkit_root / "minibrowser-wpe/sys/lib"),
                    "/existing",
                ],
            )
            self.assertEqual(
                environment["WEBKIT_EXEC_PATH"],
                str(webkit_root / "minibrowser-wpe/bin"),
            )

    def test_required_sonames_cover_the_observed_ubuntu_24_gap(self) -> None:
        """Lock the runtime libraries missing from the pinned Ubuntu 20 browser."""
        self.assertEqual(len(REQUIRED_SONAMES), 12)
        self.assertIn("libicudata.so.74", REQUIRED_SONAMES)
        self.assertIn("libxml2.so.2", REQUIRED_SONAMES)
        self.assertIn("libavif.so.16", REQUIRED_SONAMES)
        self.assertIn("libabsl_synchronization.so.20220623", REQUIRED_SONAMES)


if __name__ == "__main__":
    unittest.main()
