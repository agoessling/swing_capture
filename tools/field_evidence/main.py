"""Bazel entry point for the field-evidence manifest tool."""

from tools.field_evidence.manifest import main

if __name__ == "__main__":
    raise SystemExit(main())
