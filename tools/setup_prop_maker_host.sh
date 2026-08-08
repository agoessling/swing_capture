#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
  echo "This host setup needs root access."
  echo "Run: sudo $0"
  exit 2
fi

if ! getent group plugdev >/dev/null; then
  echo "The plugdev group does not exist on this host."
  exit 1
fi

repository_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
rule_source="$repository_root/config/udev/60-swing-capture-rp2040.rules"
rule_target="/etc/udev/rules.d/60-swing-capture-rp2040.rules"

install -D -m 0644 "$rule_source" "$rule_target"
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --action=add
udevadm trigger --subsystem-match=tty --action=change
udevadm settle

echo "Installed $rule_target"
echo "Applied RP2040 bootloader, picotool, and serial permissions."
echo "If access did not update, unplug and reconnect the Feather once."
