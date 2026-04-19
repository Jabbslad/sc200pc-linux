# SC200PC Arch / Omarchy Packaging Plan

This document describes how the SC200PC Linux camera support is packaged
for Arch and how Omarchy should consume it.

## Architecture

The camera uses the **native libcamera path** — libcamera's "simple"
pipeline handler with software ISP. This is the same architecture that
mainline libcamera uses for IPU6/IPU7 webcams on Fedora / Ubuntu.

The proprietary Intel HAL path (`intel-ipu7-camera`, `v4l2-relayd`,
`icamerasrc`) was investigated and is a dead end on this sensor — the
graph compiler binary (`graphspec`) needed to produce a Linux-format
`.IPU75XA.bin` is Intel-internal. That work is archived at
[sc200pc-ipu7-hal-exploration](https://github.com/jabbslad/sc200pc-ipu7-hal-exploration).

## Package split

| Package | What it is |
|---|---|
| `ipu-bridge-sslc2000-dkms` | DKMS `ipu-bridge.ko` with `SSLC2000` in the sensor table |
| `sc200pc-dkms` | DKMS V4L2 sensor driver |
| `libcamera-sc200pc` | Arch `libcamera` fork with SC200PC helper + IPA YAML + udev rule |
| `galaxybook6pro-camera` | Meta-package: depends on the above + `pipewire-libcamera`; ships WirePlumber V4L2 filter; conflicts with `intel-ipu7-camera` |

Each package maps 1:1 to one upstream patch. When a patch lands
upstream, the corresponding package is deleted and removed from the
meta-package's `depends=()`.

## Conflicts with intel-ipu7-camera

The native libcamera path and the Intel HAL path are **mutually
exclusive**:

- `intel-ipu7-camera` disables libcamera in WirePlumber, hides IPU7 V4L2
  nodes, and runs `v4l2-relayd@ipu7` with `Restart=always`
- If `v4l2-relayd@ipu7` fails (which it does on SC200PC — no valid graph
  binary), its `ExecStartPost` restarts WirePlumber every 2 seconds,
  crash-looping audio

`galaxybook6pro-camera` declares `conflicts=('intel-ipu7-camera',
'intel-ipu7-camera-sc200pc', 'sc200pc-ipu75xa-config')`. Users must
remove the HAL packages before installing.

## Omarchy integration

Omarchy's hardware detection (`install/config/hardware/intel/ipu7-camera.sh`)
installs `intel-ipu7-camera` when it sees `OVTI08F4` in ACPI. For the
Galaxy Book6 Pro (sensor HID `SSLC2000`, not `OVTI08F4`), this detection
does not fire — so Omarchy won't install the conflicting HAL package
automatically.

Once the packages are published to AUR, Omarchy could add a model-gated
install:

```bash
if omarchy-hw-match "Samsung Galaxy Book6 Pro"; then
  paru -S --needed galaxybook6pro-camera
fi
```

## Rebase triggers

| Event | Action |
|---|---|
| Kernel release changes `ipu-bridge.c` | Re-extract source, re-apply patch, bump `ipu-bridge-sslc2000-dkms` version |
| Kernel V4L2 API churn | Fix `sc200pc.c`, bump `sc200pc-dkms` version |
| Arch bumps `libcamera` | Bump `_upstream_pkgver` in `libcamera-sc200pc`, resync patches |
| Patch lands upstream | Drop the corresponding package from AUR and meta-package |

## Verification

```bash
dkms status
modinfo sc200pc
sc200pc-libcamera-check
cam -l
v4l2-ctl --stream-mmap --stream-count=1 -d /dev/video0
```
