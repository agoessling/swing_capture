"""Bazel entry point for the Android APK installer."""

import sys

from tools.android_apk_installer import main


if __name__ == "__main__":
    sys.exit(main())
