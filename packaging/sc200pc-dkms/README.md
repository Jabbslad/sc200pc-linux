`sc200pc-dkms` is the working out-of-tree V4L2 sensor driver for the
Samsung/SmartSens SC200PC front camera on the Galaxy Book6 Pro.

Current state:

- ACPI match on `SSLC2000` works
- chip ID probe works (`0x0b7101`)
- graph binding through `ipu-bridge-sslc2000` works
- stream-on works well enough to capture real 1928x1088 raw10 BGGR
  frames from `/dev/video0`
- intended as the kernel-side half of the preferred open-source stack:
  `ipu-bridge-sslc2000` + `sc200pc-dkms` + `libcamera`

Still incomplete:

- V4L2 controls are minimal; exposure / gain / flip / test-pattern
  controls are still pending
- no claim of upstream quality or ABI stability yet
- no promise that Intel `libcamhal` userspace will work with this
  sensor

Suggested local workflow:

```bash
cd packaging/sc200pc-dkms
makepkg -si
dkms status
modinfo sc200pc
sudo modprobe sc200pc
dmesg | grep -i sc200pc
```

Kernel verification after install:

```bash
media-ctl -p
v4l2-ctl --stream-mmap --stream-count=1 -d /dev/video0
```

If the module fails to build, fix the kernel API mismatches in
`sc200pc.c` first.

If the module builds but never binds, verify:

- `SSLC2000` still appears in `/sys/bus/acpi/devices/`
- the I2C device is instantiated from ACPI
- the running kernel includes the surrounding IPU7 stack needed by the
  platform

This package is not the browser-facing solution by itself. The preferred
userspace target is now `libcamera` + `pipewire-libcamera`, not
`libcamhal`.
