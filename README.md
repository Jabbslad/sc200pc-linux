# sc200pc-linux

Linux camera enablement for the Samsung Galaxy Book6 Pro (NP940XJG-KGDUK)
front camera — Samsung SC200PC sensor on Intel IPU7 (Panther Lake).

The sensor is declared in ACPI as HID `SSLC2000` and has no upstream
Linux driver. This repo carries an out-of-tree V4L2 sensor driver, a
patched IPU bridge module that recognises `SSLC2000`, a libcamera
helper / IPA, and an idempotent installer that wires it all together.

The camera works through the **native libcamera "simple" pipeline
handler with software ISP** — the same architecture that mainline
libcamera + Fedora / Ubuntu use for IPU6/IPU7 webcams generally. The
proprietary Intel IPU7 vendor HAL path was investigated in depth and
turned out to be a dead end on this sensor without Intel-internal
tooling; that work lives in a separate archive repo:
[sc200pc-ipu7-hal-exploration](https://github.com/jabbslad/sc200pc-ipu7-hal-exploration).

## What's in here

- **`packaging/ipu-bridge-sslc2000/`** — DKMS replacement for the
  in-tree `ipu-bridge.ko`, patched to recognise ACPI HID `SSLC2000`
  (the source patch is `patches/ipu-bridge-add-sslc2000.patch`).
- **`packaging/sc200pc-dkms/`** — DKMS out-of-tree V4L2 sensor driver
  (`sc200pc.c`). The 141-entry init table was reverse-engineered from
  the OEM Windows `sc200pc.sys` binary. Exposes V4L2 timing controls
  (HBLANK, VBLANK, pixel_rate, digital_gain) and orientation
  (FRONT/0°). Together with `ipu-bridge-sslc2000` this produces real
  `/dev/video0` raw10 BGGR frames at 1928×1088@30 fps.
- **`packaging/sc200pc-libcamera-pipewire/`** — a soft-IPA YAML, a udev
  rule that restores user access to the `Intel IPU7 ISYS Capture` node
  (which the vendor HAL workflow normally hides), and a
  `rebuild-libcamera-with-sc200pc-support` script that splices
  `patches/libcamera-sc200pc.patch` into the Arch `libcamera` PKGBUILD
  and rebuilds. The patch adds `CameraSensorHelperSc200pc`
  (`gain = code / 16`, black level 64-at-10-bit) and a matching
  `CameraSensorProperties` entry — without these, libcamera's AGC
  treats the V4L2 gain code as a linear gain multiplier and converges
  at effectively 1.0×, producing dark green-tinted frames.

## Install

On a freshly installed Galaxy Book6 Pro:

```bash
git clone https://github.com/jabbslad/sc200pc-linux
cd sc200pc-linux
bash install.sh
```

The installer is idempotent and safe to re-run. It will:
1. Build & install the three Arch packages (`makepkg -si`).
2. Run `rebuild-libcamera-with-sc200pc-support` to splice the helper
   patch into Arch `libcamera` and reinstall.
3. Drop the udev rule that exposes `Intel IPU7 ISYS Capture *` to the
   user (so PipeWire / Chromium / qcam can open `/dev/video0`).
4. Remove any HAL-only WirePlumber overrides
   (`10-disable-libcamera.conf`, `60-hide-ipu7-v4l2.conf`) that would
   otherwise hide the camera from libcamera consumers.
5. Restart `wireplumber`, `pipewire`, and the desktop portal so the
   camera shows up in browser pickers without a logout.

## Verifying

```bash
sc200pc-libcamera-check                                      # full diagnostic
v4l2-ctl --stream-mmap --stream-count=1 -d /dev/video0       # raw frame
cam -l                                                       # libcamera enumeration
```

If `cam -l` warns "Failed to create camera sensor helper for sc200pc"
or "No static properties available for 'sc200pc'", run
`rebuild-libcamera-with-sc200pc-support` and restart the user services.

## Status

- **Functional today** for Chromium, qcam, PipeWire-fed apps, and
  anything else that goes through libcamera.
- **Image quality is "first pass" not "production":** indoor scenes
  have a slight olive / monochrome cast, exposure is hand-tuned, and
  the IPA YAML is a first-pass approximation rather than measured
  calibration. Improving it requires real tuning work against a colour
  chart — pull requests welcome.
- **CPU cost:** software ISP runs on the CPU, costing ~5–15% of one
  core continuously while the camera is in use. That's the trade for
  not relying on Intel's closed-source HAL stack.

## Upstreaming intent

The patches in `patches/` are deliberately kept small and shaped for
upstream submission:

- `ipu-bridge-add-sslc2000.patch` — adds `SSLC2000` to
  `ipu_supported_sensors[]` in `drivers/media/pci/intel/ipu-bridge.c`.
- `libcamera-sc200pc.patch` — adds `CameraSensorHelperSc200pc` and a
  matching `CameraSensorProperties` entry to libcamera.
- `sc200pc-libcamera-enum.patch` — additional libcamera enumeration
  tweak.

The kernel sensor driver (`packaging/sc200pc-dkms/sc200pc.c`) needs
some clean-up before upstream submission — see
[`sc200pc-kernel-patch-checklist.md`](sc200pc-kernel-patch-checklist.md).

## See also

- [`sc200pc-arch-packaging-plan.md`](sc200pc-arch-packaging-plan.md)
  — packaging design notes.
- [`sc200pc-kernel-patch-checklist.md`](sc200pc-kernel-patch-checklist.md)
  — what the upstream kernel submission would need.
- [`sc200pc-driver-skeleton.c`](sc200pc-driver-skeleton.c) — early
  driver skeleton from before the Windows reverse-engineering work.
