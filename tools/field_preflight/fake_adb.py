"""Hermetic ADB command boundary for the executable smoke test."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence


CONNECT_ARGUMENT_COUNT = 2
SERIAL_COMMAND_ARGUMENT_COUNT = 3
RESPONSES: dict[tuple[str, ...], tuple[str, ...]] = {
    ("get-state",): ("device",),
    ("shell", "getprop", "sys.user.0.ce_available"): ("true",),
    (
        "shell",
        "am",
        "start",
        "-W",
        "-n",
        "com.agoessling.swingcapture/.MainActivity",
    ): ("Starting: Intent", "Status: ok", "Complete"),
    ("shell", "input", "keyevent", "KEYCODE_SLEEP"): (),
    ("shell", "dumpsys", "power"): ("mWakefulness=Asleep",),
}


def main(arguments: Sequence[str]) -> int:
    """Return production-shaped responses for the bounded preflight ceremony."""
    if len(arguments) == CONNECT_ARGUMENT_COUNT and arguments[0] == "connect":
        print(f"connected to {arguments[1]}")
        return 0
    if len(arguments) < SERIAL_COMMAND_ARGUMENT_COUNT or arguments[0] != "-s":
        print(f"unexpected fake ADB arguments: {arguments!r}", file=sys.stderr)
        return 64

    command = tuple(arguments[2:])
    response = RESPONSES.get(command)
    if response is None:
        print(f"unexpected fake ADB arguments: {arguments!r}", file=sys.stderr)
        return 64
    for line in response:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
