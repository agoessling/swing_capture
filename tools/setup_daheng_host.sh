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
rule_source="$repository_root/config/udev/99-swing-capture-daheng.rules"
rule_target="/etc/udev/rules.d/99-swing-capture-daheng.rules"
service_name="swing-capture-usbfs-memory.service"
service_source="$repository_root/config/systemd/$service_name"
service_target="/etc/systemd/system/$service_name"
usbfs_memory="/sys/module/usbcore/parameters/usbfs_memory_mb"

if ! command -v systemctl >/dev/null; then
  echo "systemd is required to configure usbfs memory persistently." >&2
  exit 1
fi
if [[ ! -w "$usbfs_memory" ]]; then
  echo "The writable usbfs memory parameter was not found at $usbfs_memory." >&2
  exit 1
fi

install -D -m 0644 "$rule_source" "$rule_target"
install -D -m 0644 "$service_source" "$service_target"
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --attr-match=idVendor=2ba2 --action=add
udevadm settle

systemctl daemon-reload
systemctl enable --now "$service_name"

configured_usbfs_memory="$(<"$usbfs_memory")"
if [[ "$configured_usbfs_memory" != "2000" ]]; then
  echo "Failed to set usbfs memory to 2000 MB; current value is $configured_usbfs_memory." >&2
  exit 1
fi

echo "Installed $rule_target"
echo "Installed and enabled $service_target"
echo "Applied Daheng USB permissions and set the persistent USB buffer limit to 2000 MB."
echo "If the device node did not update, unplug and reconnect the camera once."
