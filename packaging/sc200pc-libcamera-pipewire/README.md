# sc200pc-libcamera-pipewire

Native `libcamera` path for the Samsung/SmartSens SC200PC front camera
on the Galaxy Book6 Pro. This is the only path that produces a working
Chromium / qcam / PipeWire stream today; the vendor HAL path in
`sc200pc-ipu75xa-config` remains blocked in graph configuration.

Current quality status: the camera is now mechanically usable through
the native path, including browser video, but indoor image quality is
still poor. The preview is low-saturation with an olive / near-
monochrome cast under warm room lighting, and the tuning should still be
treated as bring-up quality rather than a finished camera stack.

This package also installs a udev override for `Intel IPU7 ISYS Capture`
nodes. `intel-ipu7-camera` ships a rule that hides those nodes behind
`root:root 0600` for the vendor HAL path; the native `libcamera` path
needs them accessible to the logged-in user.

## What this package installs

- dependencies: `libcamera`, `libcamera-tools`, `gst-plugin-libcamera`,
  `pipewire-libcamera`
- `/etc/udev/rules.d/72-ipu7-native-isys.rules` — restore `video` /
  `uaccess` permissions on the raw IPU7 capture node for the native path
- `/usr/share/libcamera/ipa/simple/sc200pc.yaml` — IPA tuning config
- `/usr/bin/sc200pc-libcamera-check` — diagnostic helper
- `/usr/bin/rebuild-libcamera-with-sc200pc-support` — patched-libcamera
  rebuild helper (see below)
- `/usr/share/doc/sc200pc-libcamera-pipewire/libcamera-sc200pc.patch` —
  the patch applied by the rebuild helper

## Why stock libcamera is not enough

The Arch `libcamera` package does not know about the SC200PC. Without a
`CameraSensorHelper` registration and a `CameraSensorProperties` entry,
`cam -l` prints three warnings:

```
WARN  Recommended V4L2 control 0x009a0922 not supported
WARN  No static properties available for 'sc200pc'
WARN  IPASoft: Failed to create camera sensor helper for sc200pc
```

The last warning is load-bearing. The simple soft IPA's AGC reads from
and writes to `V4L2_CID_ANALOGUE_GAIN`, and when no helper is registered
it treats the raw V4L2 gain code as a linear gain multiplier. The
SC200PC advertises codes 0x10 (= 1.0×) through 0xff (= 15.94×), so a
"10–20 % gain bump" in AGC's loop moves the code by 1–2 counts instead
of by real-world exposure stops. AGC converges at effectively 1.0× gain
and AWB cannot find signal to correct with. The symptom is a dark,
green-tinted frame — exactly what you see through the simple path on
stock libcamera.

## Fixing it

Upstream libcamera is the correct long-term home. Until the patch lands
there, run the bundled helper on the target machine once:

```bash
rebuild-libcamera-with-sc200pc-support
```

This clones the current Arch libcamera package sources, splices
`libcamera-sc200pc.patch` into the PKGBUILD (adding the
`CameraSensorHelperSc200pc` class and the corresponding sensor
properties entry), builds, and installs the result. It operates on
`libcamera` + `libcamera-ipa` only; the other split packages are
rebuilt as a side effect of `makepkg -s` but you only need to install
those two.

If you previously enabled the experimental HAL path, undo that first:

```bash
sudo rm -f /etc/wireplumber/wireplumber.conf.d/10-disable-libcamera.conf \
           /etc/wireplumber/wireplumber.conf.d/60-hide-ipu7-v4l2.conf
sudo systemctl stop v4l2-relayd@ipu7
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=video4linux
systemctl --user restart wireplumber pipewire xdg-desktop-portal xdg-desktop-portal-hyprland
```

The patch is small (one helper class + one properties entry) and
intended for upstream submission.

## What still needs tuning

Even with the helper in place, the installed `sc200pc.yaml` is a first
pass. It gives AGC somewhere to converge, but:

- CCMs at 3200 K / 6500 K are hand-picked, not measured against a
  colour chart
- indoor chroma is still weak; warm lighting can look olive / almost
  monochrome
- no lens shading correction (simple IPA has no LSC algorithm)
- gamma LUT is stock

These are incremental improvements once AGC is alive.

## Verification

After installing the package and running the rebuild helper, check in
order:

```bash
# kernel graph still OK
media-ctl -p
v4l2-ctl --stream-mmap --stream-count=1 -d /dev/video0

# libcamera helper is now registered (no more "Failed to create ...")
cam -l

# combined diagnostic
sc200pc-libcamera-check

# full stack
qcam
```

Then test the browser path through PipeWire / the camera portal.

## Non-goals

- does not install `intel-ipu7-camera`
- does not depend on `sc200pc-ipu75xa-config`
- does not replace the `sc200pc-dkms` kernel driver
- this package does not add a `v4l2loopback` service yet

The point is to keep the native path packaged and testable because it is
the only working desktop/browser camera path right now.
