# sc200pc-linux

Linux camera enablement for the Samsung Galaxy Book6 Pro (NP940XJG-KGDUK)
front camera — Samsung SC200PC sensor on Intel IPU7 (Panther Lake).

The sensor is declared in ACPI as HID `SSLC2000` and has no upstream
Linux driver. This repo carries an out-of-tree V4L2 sensor driver, a
DKMS IPU-bridge module that recognises `SSLC2000`, and a small tuning
YAML so stock libcamera's simple soft-ISP knows how to colour-correct
the sensor.

**This project carries zero libcamera patches.** The camera runs on
unmodified upstream / Arch libcamera via the simple pipeline handler.

## Install

Once published to the AUR:

```bash
paru -S galaxybook6pro-camera
```

That's it. `paru` resolves the component packages, pulls them from
AUR, builds them via `makepkg`, and installs. Updates happen the same
way every other Arch package updates — `paru -Syu`.

Until the packages are published to the AUR (or on a fresh Arch
install without an AUR helper), clone and run the bootstrap:

```bash
git clone https://github.com/jabbslad/sc200pc-linux
cd sc200pc-linux
bash install.sh
```

`install.sh` installs `paru` if missing, then either pulls
`galaxybook6pro-camera` from AUR (once published) or builds the three
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

Three AUR packages:

| Package | What it is | Upstream destination |
|---|---|---|
| `ipu-bridge-sslc2000-dkms` | DKMS replacement for `ipu-bridge.ko` with the 2-line `SSLC2000` table entry | Linux kernel media subsystem |
| `sc200pc-dkms` | DKMS out-of-tree V4L2 sensor driver (141-entry init table reverse-engineered from OEM Windows `sc200pc.sys`) | Linux kernel media subsystem |
| `galaxybook6pro-camera` | Meta-package. Depends on the two DKMS packages plus stock `libcamera` / `libcamera-ipa` / `pipewire-libcamera`. Ships the WirePlumber rule hiding raw IPU7 V4L2 nodes, the SC200PC simple-IPA tuning YAML, and the `sc200pc-libcamera-check` diagnostic. | — |

Canonical sources live in `packaging/<pkgname>/` in this repo. Each AUR
git repo mirrors one of those directories.

## What we give up vs patching libcamera

Because we don't register a `CameraSensorHelper` or
`CameraSensorProperties` entry for SC200PC, libcamera logs:

```
WARN No static properties available for 'sc200pc'
WARN IPASoft: Failed to create camera sensor helper for sc200pc
```

These are expected and non-fatal. The simple IPA falls back to its
default gain model, which is mathematically equivalent to ours because
the SC200PC driver exposes a linear `code / 16` gain scale. The
tradeoff:

- slightly less accurate black-level normalisation
- default unit cell / sensor delays instead of measured values

Net effect in real preview: a small contrast/flatness difference, no
broken colour, no AGC failure.

## What changes with each upstream update

| Event | What happens |
|---|---|
| Kernel release, `ipu-bridge.c` unchanged | DKMS auto-rebuilds both modules. Nothing else needed. |
| Kernel release where `ipu-bridge.c` changed | Re-extract `ipu-bridge.c`, re-apply `patches/ipu-bridge-add-sslc2000.patch`, bump `ipu-bridge-sslc2000-dkms` `pkgver`, push to AUR. |
| Kernel V4L2 / media API churn | Update `sc200pc.c` if it no longer builds; bump `sc200pc-dkms` `pkgver`. |
| Arch `libcamera` bump | Nothing to do. Stock libcamera continues to work. |

## Status

- **Functional** for Chromium, qcam, PipeWire-fed apps, and anything
  else that goes through libcamera.
- **Image quality is "first pass" not "production":** indoor scenes
  have a slight flatness, and the IPA YAML is a first-pass
  approximation rather than measured calibration. Improving it
  requires real tuning work against a colour chart.
- **CPU cost:** depends on libcamera's SoftISP mode.
  - **GPU mode** (default on Arch libcamera ≥ 0.7.0): ~9% of one
    core; debayer + colour-correction matrix run on the iGPU via
    OpenGL ES 2.0. Measured on Panther Lake Xe3 at 1080p30: 0.64s
    user + 0.34s sys over 10.48s wall.
  - **CPU mode** (force with `LIBCAMERA_SOFTISP_MODE=cpu`): ~63% of
    one core under the same workload. Pre-0.7.0 libcamera only
    offered this path.

  The full IPA (AE, AWB, lens shading) still runs on the CPU in both
  modes, which is why GPU mode isn't closer to zero.

## IPA tuning

`packaging/galaxybook6pro-camera/sc200pc.yaml` controls the simple
software ISP's black level handling, colour correction matrices, AWB,
and AGC. The current values are a first-pass heuristic — proper tuning
against a colour chart is the main remaining work item.
