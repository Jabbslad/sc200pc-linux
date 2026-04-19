`sc200pc-dkms` is the out-of-tree V4L2 sensor driver for the
Samsung/SmartSens SC200PC front camera on the Galaxy Book6 Pro.

Current state:

- ACPI match on `SSLC2000` works
- chip ID probe works (`0x0b7101`)
- graph binding through `ipu-bridge-sslc2000` works
- stream-on works — captures real 1928×1088 raw10 BGGR frames from
  `/dev/video0`
- exposure, gain, flip, and test-pattern V4L2 controls
- orientation / rotation controls from ACPI fwnode
- pairs with `libcamera-sc200pc` for the full camera stack

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
