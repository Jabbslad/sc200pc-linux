# SC200PC Arch / Omarchy Packaging Plan

This document describes how to package the future `SC200PC` Linux support for Arch and how Omarchy should consume it once the driver exists.

## Packaging goals

Keep the solution split into layers so failures are easy to reason about:

1. bridge-side graph creation support
2. kernel-side sensor support
3. experimental vendor HAL / asset integration
4. working native `libcamera` bring-up / upstreaming path
5. Omarchy model-gated install logic

Do not collapse these into one opaque package.

## Current blocker

The bridge-side graph blocker is **resolved** (2026-04-17).

- [patches/ipu-bridge-add-sslc2000.patch](/home/jabbslad/dev/omarchy-extras/patches/ipu-bridge-add-sslc2000.patch)
  adds `SSLC2000` to `ipu_supported_sensors[]` in
  `drivers/media/pci/intel/ipu-bridge.c`
- [packaging/ipu-bridge-sslc2000/](/home/jabbslad/dev/omarchy-extras/packaging/ipu-bridge-sslc2000)
  ships the patched module via DKMS into
  `/lib/modules/$(uname -r)/updates/dkms/ipu-bridge.ko.zst`, which
  takes priority over the in-tree copy at `kernel/...`
- after reboot, `intel-ipu7` registers the sensor and `sc200pc` binds
  into the CSI-2 graph (see camera-bringup-plan.md for the verified
  `media-ctl -p` output)

Remaining blockers are now image-quality / userspace layering:

- `sc200pc` control coverage is incomplete
- native `libcamera` / PipeWire browser path works but image quality is poor
- the native `libcamera` path is the only working browser path today
- the vendor HAL path remains blocked in graph configuration

## Recommended package split

### 1. `ipu-bridge-sslc2000` — **shipped**

Purpose:

- ship the smallest possible bridge-side change that lets `ipu_bridge_init()`
  create software-node endpoints for `SSLC2000`

Contents (as built):

- `ipu-bridge.c` — patched copy of upstream v6.19.10
  `drivers/media/pci/intel/ipu-bridge.c` (single hunk, verified
  byte-identical to `archlinux/linux` tag `v6.19.10-arch1` before patch)
- `Kbuild`, `Makefile` — out-of-tree build glue
- `dkms.conf` — DKMS builds against installed kernel headers and drops
  the module into `updates/` (priority over in-tree `kernel/` copy)
- `PKGBUILD` — Arch package at version `6.19.10.r1-1`
- `README.md` — install / verification steps

Why it exists:

- shortest path from "sensor responds on I2C" to "sensor appears in the
  IPU7 graph"

Status:

- done; see camera-bringup-plan.md for the `media-ctl -p` proof

Rebase trigger:

- on every kernel version bump where `ipu-bridge.c` changes upstream.
  Reproduce the base source from the matching Linux tag, re-apply
  [patches/ipu-bridge-add-sslc2000.patch](/home/jabbslad/dev/omarchy-extras/patches/ipu-bridge-add-sslc2000.patch),
  bump `pkgver`

### 2. `sc200pc-dkms`

Purpose:

- ship the out-of-tree kernel sensor driver while development is ongoing

Contents:

- `sc200pc.ko`
- DKMS metadata

Expected source layout:

- `src/sc200pc.c`
- `src/Makefile`
- `src/dkms.conf`

Dependencies:

- `dkms`
- `linux-headers`

Optional:

- `linux-zen-headers`
- `linux-lts-headers`

Why DKMS first:

- avoids maintaining a full custom kernel package
- lets Omarchy users stay on their preferred Arch kernel
- reduces blast radius while the driver is unstable

### 3. `sc200pc-ipu75xa-config`

Purpose:

- ship the vendor HAL assets for explicit HAL investigation

Contents:

- `/etc/camera/ipu75xa/SC200PC_KAFC917_PTL.aiqb`
- vendor HAL sensor / graph config

Notes:

- this package is not the preferred end-user path
- do not let it disable the working native `libcamera` desktop path
- keep relay / policy switching manual while HAL is graph-blocked

### 4. `sc200pc-libcamera-pipewire`

Purpose:

- ship the experimental native `libcamera` path for bring-up,
  validation, and upstreaming

Contents:

- dependencies on:
  - `libcamera`
  - `libcamera-tools`
  - `gst-plugin-libcamera`
  - `pipewire-libcamera`
- `/usr/share/libcamera/ipa/simple/sc200pc.yaml`
- docs and diagnostics

Dependencies:

- the IPU7 userspace stack already used on the machine

Notes:

- this path now works mechanically, including Chromium camera enumeration
- image quality is still not good enough to make it the default path
- keep it packaged so tuning and upstream work can continue

### 5. Optional meta-package: `galaxybook6pro-camera`

Purpose:

- convenience package depending on both:
  - `sc200pc-dkms`
  - `sc200pc-ipu75xa-config`

Use this only if the dependency graph stays clean.

## Suggested PKGBUILD split

### `sc200pc-dkms`

Package responsibilities:

- install driver source into `/usr/src/sc200pc-<version>/`
- let DKMS build against installed kernels

Files:

- `PKGBUILD`
- `dkms.conf`
- `sc200pc.c`
- `Makefile`

`dkms.conf` shape:

```bash
PACKAGE_NAME="sc200pc"
PACKAGE_VERSION="@PKGVER@"
BUILT_MODULE_NAME[0]="sc200pc"
DEST_MODULE_LOCATION[0]="/kernel/drivers/media/i2c"
AUTOINSTALL="yes"
```

Kernel `Makefile` shape:

```make
obj-m += sc200pc.o
```

### `sc200pc-ipu75xa-config`

Package responsibilities:

- install sensor JSON
- install graph/tuning assets
- patch or replace HAL config in a controlled way

Do not edit files in post-install with `sed` if you can avoid it.

Prefer:

- shipping full known-good config files
- or shipping a dedicated `*.json` file and a wrapper/merge mechanism if the HAL supports it

## Versioning strategy

Use separate versions for:

- driver package
- config package

Reason:

- kernel driver iteration and tuning asset iteration will not move in lockstep

Suggested format:

- `sc200pc-dkms 0.1.0`
- `sc200pc-ipu75xa-config 2026.04.17`

## Build / install flow on Arch

### Development phase

1. ~~patch or replace `ipu-bridge` so `SSLC2000` gets software-node endpoints~~
   — **done**, shipped as `packaging/ipu-bridge-sslc2000/`
2. ~~verify the IPU graph no longer reports `no subdev found in graph`~~
   — **done**; `sc200pc 2-0036` appears as an `ENABLED,IMMUTABLE` link
   to `Intel IPU7 CSI2 0:0`
3. ~~build `sc200pc-dkms`~~ — **done**, shipped as
   `packaging/sc200pc-dkms/` (version 0.3.0) with 141-entry init table
   reverse-engineered from the OEM Windows `sc200pc.sys`
4. ~~install on target machine~~ — **done**
5. ~~verify subdevice graph attachment~~ — **done**
6. ~~add stream-on sequence to `sc200pc`~~ — **done**; confirmed by
   capturing a 4 247 552-byte Bayer BGGR raw10 frame on `/dev/video0`
7. ~~populate `sc200pc-ipu75xa-config` with real assets~~ — **partial**,
   shipped as `packaging/sc200pc-ipu75xa-config/` (version 2026.04.17).
   Installs AIQB, sensor JSON (from `imx471-uf.json` template), and
   registers `sc200pc-uf-0` in `libcamhal_configs.json`
8. ~~verify HAL / graph / PipeWire integration end-to-end~~ —
   **BLOCKED**. libcamhal gets through JSON parsing, CSI port
   resolution, AIQB load, but fails at graph config: the Windows
   `graph_settings_SC200PC_KAFC917_PTL.bin` has a different container
   format (`magic 0x5C63B5E7`) from what Linux libcamhal expects
   (`magic 0x4229ABEE`). No publicly available SC200PC Linux
   `.IPU75XA.bin` exists; Intel's graph compiler (`graphspec`) is
   proprietary. See camera-bringup-plan.md §Phase 3 status.
9. ~~Add and verify the native open-source userspace path:
   `libcamera` + `pipewire-libcamera` + portal.~~
   **DONE for streaming / browser access, NOT DONE for image quality.**
   Chromium can see the camera once PipeWire camera support is enabled,
   but the simple IPA path still produces poor images. Keep this path
   experimental until tuning improves.

### Stable phase

Once the driver is reliable:

1. keep `sc200pc-dkms` if upstreaming is still incomplete
2. move to in-kernel support when accepted upstream
3. keep the Arch package only for userspace assets if needed

## Omarchy integration plan

`install.sh` should not try to solve this today.

It should only automate the camera once these packages exist and are tested.

### Future `install.sh` behavior

Inside the existing `is_galaxybook6_pro` block:

1. install the bridge-side package or patched kernel that adds `SSLC2000`
2. install `sc200pc-dkms`
3. install the working `sc200pc-libcamera-pipewire` package
4. ensure required camera services / packages are present
5. print a targeted status message if the browser path is not yet available

Example shape:

```bash
if is_galaxybook6_pro; then
  yay -S --noconfirm --needed ipu-bridge-sslc2000 sc200pc-dkms sc200pc-libcamera-pipewire
fi
```

### What `install.sh` should not do

- patch random kernel files in-place
- decompile ACPI tables on the user machine
- download Windows blobs ad hoc
- overwrite generic IPU7 configs blindly
- pretend a userspace package alone can fix missing graph endpoints
- make the blocked `libcamhal` path the default integration target

Keep `install.sh` as a consumer of packaged artifacts, not as the build system.

## Interaction with existing `intel-ipu7-camera`

The current machine has:

- `intel-ipu7-camera`

That package is oriented around:

- `OV08X40`
- `OV13B10`
- `IMX471`

So there are two clean options:

### Option A: separate package set

- keep `intel-ipu7-camera`
- add `sc200pc-dkms`
- add `sc200pc-ipu75xa-config` only for explicit vendor-HAL experiments
- add `sc200pc-libcamera-pipewire` for the current default path

Pros:

- least disruptive

Cons:

- possible config overlap if both install competing HAL files

### Option B: forked replacement package

- create a new package such as `intel-ipu7-camera-sc200pc`
- vendor the HAL layout with `SC200PC` support integrated

Pros:

- one coherent package

Cons:

- higher maintenance burden
- greater conflict risk with future package updates

Recommendation:

- keep the bridge change separate from the userspace asset package
- keep `sc200pc-ipu75xa-config` experimental until a valid Linux graph
  asset exists
- treat `libcamera` + `pipewire-libcamera` as the current default path,
  even though image quality is still poor
- do not wire `install.sh` to install camera support until the native
  path is good enough to support by default
- start with Option A, but keep the native `libcamera` package clearly
  separated from the experimental HAL package

## Collision handling

Before extending the vendor-HAL package set further, decide one of:

1. package owns the full `/etc/camera/ipu75xa/` tree
2. package owns only `SC200PC` files and a fully merged `libcamhal_configs.json`
3. package conflicts with `intel-ipu7-camera`

Recommendation:

- if the existing package cannot be extended cleanly, declare an explicit conflict

Silent partial merges are risky here.

## Verification checklist after packaging

After installing packages on the target machine:

```bash
dkms status
modinfo sc200pc
dmesg | grep -i sc200pc
media-ctl -p
v4l2-ctl --list-devices
cam -l
```

If the preferred libcamera userspace path fails:

```bash
systemctl --user status pipewire
wpctl status
qcam
```

If the blocked vendor-HAL path is being inspected explicitly:

```bash
ls /etc/camera/ipu75xa
ls /etc/camera/ipu75xa/gcss
cat /etc/camera/ipu75xa/libcamhal_configs.json
systemctl status v4l2-relayd@ipu7
```
