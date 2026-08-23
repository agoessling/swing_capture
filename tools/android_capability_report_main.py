"""Bazel entry point for Android capability evidence collection."""

from tools.android_capability_report import main

if __name__ == "__main__":
    raise SystemExit(main())
