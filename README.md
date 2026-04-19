# sc200pc-linux

Linux camera enablement for the Samsung Galaxy Book6 Pro (NP940XJG-KGDUK)
front camera — Samsung SC200PC sensor on Intel IPU7 (Panther Lake).

The sensor is declared in ACPI as HID `SSLC2000` and has no upstream
Linux driver. This repo carries an out-of-tree V4L2 sensor driver, a
patched IPU bridge module that recognises `SSLC2000`, and a libcamera
fork with the SC200PC helper / properties entry baked in.

The camera works through the **native libcamera "simple" pipeline
handler with software ISP** — the same architecture that mainline
libcamera + Fedora / Ubuntu use for IPU6/IPU7 webcams generally.

## Install

Once published to the AUR:

```bash
paru -S galaxybook6pro-camera
```

That's it. `paru` resolves the three component packages, pulls them
from AUR, builds them via `makepkg`, and installs. Updates happen the
same way every other Arch package updates — `paru -Syu`.

Until the packages are published to the AUR (or on a fresh Arch install
without an AUR helper), clone and run the bootstrap:

```bash
git clone https://github.com/jabbslad/sc200pc-linux
cd sc200pc-linux
bash install.sh
```

`install.sh` installs `paru` if missing, then either pulls
`galaxybook6pro-camera` from AUR (once published) or builds the four
packages locally from `packaging/`.

## After install

The DKMS modules build on install but load at next boot. Either reboot
or reload without rebooting:

```bash
systemctl --user stop wireplumber pipewire
sudo rmmod sc200pc intel_ipu7_isys intel_ipu7 ipu_bridge
sudo modprobe intel_ipu7
systemctl --user start pipewire
```

Verify:

```bash
sc200pc-libcamera-check
cam -l
```

## Package layout

Four AUR packages, one-to-one with the three upstream patches plus a
meta:

| Package | What it is | Upstream destination |
|---|---|---|
| `ipu-bridge-sslc2000-dkms` | DKMS replacement for `ipu-bridge.ko` with the 2-line `SSLC2000` table entry | Linux kernel media subsystem |
| `sc200pc-dkms` | DKMS out-of-tree V4L2 sensor driver (141-entry init table reverse-engineered from OEM Windows `sc200pc.sys`) | Linux kernel media subsystem |
| `libcamera-sc200pc` | Fork of Arch's `libcamera` package with the SC200PC `CameraSensorHelper` and `CameraSensorProperties` patched in. `provides=libcamera conflicts=libcamera replaces=libcamera` so `pacman -Syu` respects the pin | libcamera upstream |
| `galaxybook6pro-camera` | Thin meta-package depending on the three above + `pipewire-libcamera`; ships WirePlumber config to hide raw IPU7 V4L2 nodes; conflicts with `intel-ipu7-camera` | — |

Canonical sources live in `packaging/<pkgname>/` in this repo. Each AUR
git repo mirrors one of those directories.

## What changes with each upstream update

| Event | What happens |
|---|---|
| Kernel patch release, `ipu-bridge.c` unchanged | DKMS auto-rebuilds both modules on kernel install. Nothing else needed. |
| Kernel release where `ipu-bridge.c` changed | Re-extract the new `ipu-bridge.c`, re-apply `patches/ipu-bridge-add-sslc2000.patch`, bump `ipu-bridge-sslc2000-dkms` `pkgver`, push to AUR. |
| Kernel V4L2 / media API churn | Update `sc200pc.c` if it no longer builds; bump `sc200pc-dkms` `pkgver`. |
| Arch `libcamera` bump | Bump `_upstream_pkgver` in `libcamera-sc200pc`'s PKGBUILD, resync carried distro patches (`libcamera-fix-python3.14-macro-redefinition.patch` etc.), rebase `libcamera-sc200pc.patch` if upstream moved the target files, push to AUR. |
| A patch lands upstream | Drop the corresponding package from AUR and from `galaxybook6pro-camera`'s `depends=()`. |

The distribution strategy is designed to dissolve: when `SSLC2000`
lands in the media subsystem, `ipu-bridge-sslc2000-dkms` is deleted.
When the libcamera helper merges, `libcamera-sc200pc` is deleted and
users get stock `libcamera` back. When the sensor driver lands,
`sc200pc-dkms` is deleted. The meta-package's `depends=()` tracks the
remaining pieces.

## Status

- **Functional today** for Chromium, qcam, PipeWire-fed apps, and
  anything else that goes through libcamera.
- **Image quality is "first pass" not "production":** indoor scenes
  have a slight olive / monochrome cast, exposure is hand-tuned, and
  the IPA YAML is a first-pass approximation rather than measured
  calibration. Improving it requires real tuning work against a colour
  chart — pull requests welcome.
- **CPU cost:** software ISP runs on the CPU, costing ~5–15% of one
  core continuously while the camera is in use.

## IPA tuning

The IPA YAML (`packaging/libcamera-sc200pc/sc200pc.yaml`) controls
black level, colour correction matrices, AWB, and AGC for the simple
software ISP. The current values are a first-pass heuristic — proper
tuning against a colour chart is the main remaining work item.
