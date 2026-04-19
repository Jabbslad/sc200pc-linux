# ipu-bridge-sslc2000

DKMS replacement for the in-tree `ipu-bridge` kernel module, adding ACPI
HID `SSLC2000` (Samsung/SmartSens SC200PC) to the `ipu_supported_sensors`
table.

Target hardware: Samsung Galaxy Book6 Pro (NP940XJG-KGDUK, Panther Lake)
internal front camera.

## Why this exists

`intel-ipu7` falls back to `ipu_bridge_init(..., ipu_bridge_parse_ssdb)`
when the IPU PCI device has no firmware-node graph. `ipu-bridge` only
builds a software-node graph for ACPI HIDs it recognizes. Stock v6.19.10
does not list `SSLC2000`, so the bridge skips it and `intel-ipu7`
reports "no subdev found in graph" at probe.

This package ships a patched `ipu-bridge.ko` and installs it into
`/lib/modules/$(uname -r)/updates/`, which takes priority over the
in-tree module at `kernel/drivers/media/pci/intel/ipu-bridge.ko`.

## Kernel compatibility

- Base source: Linux v6.19.10 (`drivers/media/pci/intel/ipu-bridge.c`)
- Verified byte-identical to `archlinux/linux` tag `v6.19.10-arch1`
- Running `linux-ptl 6.19.10.arch1-1` confirmed via module
  `srcversion` = `D9FFC3E284CBF4DE44D31F0` reproduced from this source
  before patching

Rebase required on any kernel bump where `ipu-bridge.c` changes
upstream.

## Patch

See [`patches/ipu-bridge-add-sslc2000.patch`](../../patches/ipu-bridge-add-sslc2000.patch)
at the repo root for the single hunk applied to this source.

## Build / install (manual)

```bash
make
sudo make install
sudo rmmod intel_ipu7_psys intel_ipu7_isys intel_ipu7 ipu_acpi ipu_bridge
sudo modprobe intel_ipu7
dmesg -t | grep -Ei 'ipu|sslc2000' | tail
```

## Verification

After load, check:

- `modinfo ipu_bridge | grep filename` points to the `updates/` path
- `dmesg` no longer contains "no subdev found in graph"
- `media-ctl -p` lists an entity for the sensor
