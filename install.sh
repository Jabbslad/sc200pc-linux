#!/usr/bin/env bash
# One-shot installer for SC200PC camera support on the Samsung Galaxy
# Book6 Pro (NP940XJG-KGDUK). Delegates to paru; once the four packages
# are published to the AUR, a user can skip this script entirely and
# just run:
#
#     paru -S galaxybook6pro-camera
#
# This script exists to (a) bootstrap paru on bare Arch installs that
# don't already have an AUR helper, and (b) build-from-local-checkout
# for contributors iterating before pushing to AUR.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Ensure an AUR helper is available. Omarchy ships paru already;
# vanilla Arch usually doesn't.
if ! command -v paru >/dev/null 2>&1 && ! command -v yay >/dev/null 2>&1; then
  echo "==> No AUR helper found — bootstrapping paru from AUR"
  sudo pacman -S --needed --noconfirm base-devel git
  tmp="$(mktemp -d)"
  git clone https://aur.archlinux.org/paru-bin.git "$tmp/paru-bin"
  (cd "$tmp/paru-bin" && makepkg -si --noconfirm)
  rm -rf "$tmp"
fi

AUR_HELPER="$(command -v paru || command -v yay)"

# Remove packages that conflict with the native libcamera approach:
# - old-named packages from pre-AUR-rename layout
# - Intel HAL packages (mutually exclusive with libcamera)
_old=()
for p in ipu-bridge-sslc2000 sc200pc-libcamera-pipewire \
         intel-ipu7-camera intel-ipu7-camera-sc200pc \
         intel-ipu7-camera-sc200pc-debug sc200pc-ipu75xa-config \
         v4l2-relayd; do
  if pacman -Qq "$p" >/dev/null 2>&1; then
    _old+=("$p")
  fi
done
if (( ${#_old[@]} )); then
  echo "==> Removing conflicting packages: ${_old[*]}"
  sudo pacman -Rdd --noconfirm "${_old[@]}"
fi

# Clean up orphaned files left behind by removed old packages.
for f in \
    /etc/udev/rules.d/72-ipu7-native-isys.rules \
    /etc/wireplumber/wireplumber.conf.d/10-disable-libcamera.conf \
    /etc/wireplumber/wireplumber.conf.d/60-hide-ipu7-v4l2.conf \
    /etc/wireplumber/wireplumber.conf.d/99-disable-libcamera.conf
do
  if [[ -f "$f" ]] && ! pacman -Qo "$f" >/dev/null 2>&1; then
    echo "==> Removing orphaned $f"
    sudo rm -f "$f"
  fi
done

# Try the AUR-native path first. Once published, this is all users need.
if "$AUR_HELPER" -Si galaxybook6pro-camera >/dev/null 2>&1; then
  echo "==> Installing galaxybook6pro-camera from AUR"
  exec "$AUR_HELPER" -S --needed galaxybook6pro-camera
fi

# Fallback: build the packages from this checkout. Used by contributors
# iterating on the PKGBUILDs before pushing to AUR.
echo "==> galaxybook6pro-camera not yet on AUR; building from $REPO_ROOT/packaging/"
for pkg in \
    ipu-bridge-sslc2000-dkms \
    sc200pc-dkms \
    libcamera-sc200pc \
    galaxybook6pro-camera
do
  echo
  echo "    -> $pkg"
  # Don't use --noconfirm: libcamera-sc200pc triggers pacman replace-prompts
  # for stock libcamera/libcamera-ipa/etc. that must be accepted interactively.
  ( cd "$REPO_ROOT/packaging/$pkg" && makepkg -si --needed )
done

echo
echo "==> Done. Reboot to load the kernel modules, or:"
echo "    systemctl --user stop wireplumber pipewire"
echo "    sudo rmmod sc200pc intel_ipu7_isys intel_ipu7 ipu_bridge"
echo "    sudo modprobe intel_ipu7"
echo "    systemctl --user start pipewire"
echo
echo "    Then verify with: sc200pc-libcamera-check"
