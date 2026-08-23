"""Executes pinned WebKit with the isolated Ubuntu 24 compatibility closure."""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Never

from web.webkit_ubuntu24_compat import webkit_launch_environment


def main() -> Never:
    """Replace this process with the pinned MiniBrowser and compatibility environment."""
    webkit_root = Path(os.environ["SWING_CAPTURE_WEBKIT_ROOT"]).resolve()
    compatibility_library_path = Path(os.environ["SWING_CAPTURE_WEBKIT_COMPAT_LIB"]).resolve()
    environment = webkit_launch_environment(os.environ, webkit_root, compatibility_library_path)
    executable = webkit_root / "minibrowser-wpe/bin/MiniBrowser"
    os.execve(executable, [str(executable), *sys.argv[1:]], environment)  # noqa: S606


if __name__ == "__main__":
    main()
