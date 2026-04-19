# galaxybook6pro-camera

Meta-package for the Samsung Galaxy Book6 Pro (NP940XJG-KGDUK, Panther
Lake) front-camera stack via stock libcamera. Installing this on a
fresh machine should be the only user-facing step.

## What it pulls in

- `ipu-bridge-sslc2000-dkms` — DKMS `ipu-bridge.ko` with `SSLC2000`
  added to the supported-sensors table.
- `sc200pc-dkms` — DKMS V4L2 sensor driver for the SC200PC.
- `libcamera` / `libcamera-ipa` — stock Arch packages, unmodified.
- `pipewire-libcamera` — PipeWire libcamera bridge (stock Arch package).

## What it ships

- `/etc/wireplumber/wireplumber.conf.d/50-ipu7-hide-v4l2.conf` — tells
  WirePlumber's V4L2 monitor to skip the ~32 raw IPU7 ISYS capture
  nodes. These are internal ISP endpoints, not user-facing cameras;
  the SC200PC is exposed through the libcamera monitor instead.
- `/usr/share/libcamera/ipa/simple/sc200pc.yaml` — tuning YAML for
  libcamera's simple soft-ISP. Defines the algorithm pipeline and the
  CCM table at 2700 K / 4800 K / 6500 K. Loaded by stock libcamera
  via sensor-model filename match, no patching required.
- `/usr/bin/sc200pc-libcamera-check` — diagnostic helper.

## What the scriptlet does

On `post_install` / `post_upgrade`:

- removes any WirePlumber overrides that disable libcamera (left
  behind by intel-ipu7-camera or distro defaults)
- disables `v4l2-relayd@ipu7` if present (legacy HAL service that
  crash-loops and breaks audio)
- reloads udev so the bundled rule takes effect without a reboot

## Conflicts

This package conflicts with:

- `intel-ipu7-camera` / `intel-ipu7-camera-sc200pc` — the proprietary
  Intel HAL camera path. Mutually exclusive: the HAL path disables
  libcamera and uses v4l2-relayd, which we don't use.
- `libcamera-sc200pc` (+ `-ipa`, `-tools`) and `gst-plugin-libcamera-sc200pc`
  — the previous in-repo libcamera fork. Superseded by stock libcamera
  + this package's tuning YAML.

Remove those before installing:

```bash
sudo pacman -Rdd intel-ipu7-camera      # or intel-ipu7-camera-sc200pc
sudo pacman -Rdd sc200pc-ipu75xa-config v4l2-relayd
```

## Updates

`paru -Syu` updates each component independently. This meta-package
itself rarely changes — only when a new component is added or an old
one is removed (e.g. when the sensor driver or bridge entry lands
upstream and its DKMS wrapper becomes redundant).
