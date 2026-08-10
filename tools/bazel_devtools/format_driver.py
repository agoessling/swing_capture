"""Run the Bazel-owned formatter with this repository's selected tool data."""

from tools.format_targets import main

if __name__ == "__main__":
    raise SystemExit(main())
