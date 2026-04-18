#!/usr/bin/env bash
# Idempotent installer for SC200PC camera support on Galaxy Book6 Pro.
# Safe to re-run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Building and installing packages from $REPO_ROOT/packaging/"

# Build + install each package via makepkg. Order matters: the bridge
# and the sensor driver both have to be installed before libcamera is
# rebuilt with the helper.
for pkg in ipu-bridge-sslc2000 sc200pc-dkms sc200pc-libcamera-pipewire; do
  echo
  echo "    -> $pkg"
  pushd "$REPO_ROOT/packaging/$pkg" >/dev/null
  makepkg -si --needed --noconfirm
  popd >/dev/null
done

# Rebuild Arch libcamera with the SC200PC sensor helper + properties
# patch so AGC actually converges. The script ships in
# sc200pc-libcamera-pipewire and was just installed to /usr/bin.
echo
echo "==> Rebuilding libcamera with SC200PC sensor helper"
if command -v rebuild-libcamera-with-sc200pc-support >/dev/null; then
  rebuild-libcamera-with-sc200pc-support
else
  echo "    rebuild-libcamera-with-sc200pc-support not on PATH; skipping"
  echo "    Run it manually after this install completes."
fi

# Tear down any HAL-only WirePlumber overrides — these come from old
# attempts to use the proprietary Intel IPU7 vendor HAL, which is not
# the path this repo takes.
echo
echo "==> Removing HAL-only WirePlumber overrides (if present)"
sudo rm -f \
  /etc/wireplumber/wireplumber.conf.d/10-disable-libcamera.conf \
  /etc/wireplumber/wireplumber.conf.d/60-hide-ipu7-v4l2.conf

# udev rule for IPU7 ISYS node user access (the rule itself is also
# packaged inside sc200pc-libcamera-pipewire; this just makes sure it's
# active without waiting for a reboot).
echo "==> Reloading udev"
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=video4linux

# Stop any v4l2-relayd that the HAL path may have left running
sudo systemctl stop v4l2-relayd@ipu7 2>/dev/null || true

# Restart the user-side services so the camera shows up in browser
# pickers immediately, without logout.
echo "==> Restarting user services (PipeWire, WirePlumber, portal)"
systemctl --user restart wireplumber pipewire xdg-desktop-portal \
  xdg-desktop-portal-hyprland 2>/dev/null || true

echo
echo "==> Done. Verify with:"
echo "    sc200pc-libcamera-check"
echo "    cam -l"
echo "    v4l2-ctl --stream-mmap --stream-count=1 -d /dev/video0"
