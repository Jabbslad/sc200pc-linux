# SC200PC Kernel Patch Checklist

This is a patch-oriented checklist for porting the reference skeleton into a Linux kernel tree.

Target result:

- new V4L2 sensor driver for the Galaxy Book6 Pro internal camera
- ACPI match on `SSLC2000`
- initial probe on `I2C3` / `0x36`
- power-on and basic register reads before any streaming work

## Files to add or edit

### New file

- `drivers/media/i2c/sc200pc.c`

Start from:

- [sc200pc-driver-skeleton.c](/home/jabbslad/dev/omarchy-extras/sc200pc-driver-skeleton.c)

### Existing files to edit

- `drivers/media/i2c/Kconfig`
- `drivers/media/i2c/Makefile`

If the kernel tree needs a MAINTAINERS entry later:

- `MAINTAINERS`

## Kconfig

Add a new symbol near the other mobile camera sensors in `drivers/media/i2c/Kconfig`:

```kconfig
config VIDEO_SC200PC
	tristate "Samsung/Intel SC200PC sensor support"
	depends on I2C && VIDEO_DEV
	depends on ACPI
	select V4L2_CCI_I2C if VIDEO_V4L2_SUBDEV_API
	help
	  This is a V4L2 sensor driver for the Samsung/Intel SC200PC camera
	  exposed through ACPI as SSLC2000 on some Intel IPU7 laptops.
```

Notes:

- `select V4L2_CCI_I2C` is optional depending on the driver style you choose.
- If you keep raw `i2c_transfer()` helpers, you do not need that select.
- Keep dependencies conservative on the first patch.

## Makefile

Add to `drivers/media/i2c/Makefile`:

```make
obj-$(CONFIG_VIDEO_SC200PC) += sc200pc.o
```

## Driver structure

Put the new file at:

- `drivers/media/i2c/sc200pc.c`

Use this rough structure:

1. includes
2. register / constant definitions
3. private `struct sc200pc`
4. low-level I2C helpers
5. power-on / power-off helpers
6. identify routine
7. format / pad ops
8. stream op
9. firmware parsing helpers
10. probe / remove
11. ACPI match table
12. module boilerplate

## ACPI match

Add:

```c
static const struct acpi_device_id sc200pc_acpi_ids[] = {
	{ "SSLC2000" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, sc200pc_acpi_ids);
```

Wire it into the `i2c_driver`:

```c
.driver = {
	.name = "sc200pc",
	.acpi_match_table = sc200pc_acpi_ids,
},
```

## Probe milestone

The first patch should only aim for:

- bind to the ACPI-created I2C device
- request power resources
- power on
- attempt a few register reads
- register a subdevice

Avoid in patch 1:

- full mode tables
- streaming
- controls
- EEPROM parsing
- autofocus
- HAL integration

## Power resources

The Windows binary suggests these control names:

- `Reset`
- `Power0`
- `Power1`

And likely rails:

- `avdd`
- `dvdd`
- `dovdd`

So the first Linux file should request:

```c
sensor->avdd = devm_regulator_get_optional(dev, "avdd");
sensor->dvdd = devm_regulator_get_optional(dev, "dvdd");
sensor->dovdd = devm_regulator_get_optional(dev, "dovdd");

sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
sensor->power0_gpio = devm_gpiod_get_optional(dev, "power0", GPIOD_OUT_LOW);
sensor->power1_gpio = devm_gpiod_get_optional(dev, "power1", GPIOD_OUT_LOW);
```

Important:

- these names may not resolve immediately on Linux
- if they do not, log that clearly and continue iterating on ACPI parsing
- do not hard-fail too early until you know what the firmware exposes

## Firmware parsing

Patch 1 can get by with the instantiated I2C client.

Patch 2 should add parsing for:

- `_CRS`
- `SSDB`
- possibly `LNK0._DSM`

The minimum data Linux will eventually need:

- MIPI lane count
- CSI port
- data format
- clock / link frequency
- optional EEPROM presence

## Suggested commit split

### Commit 1

Title:

- `media: i2c: add skeleton driver for Samsung/Intel SC200PC sensor`

Contents:

- `Kconfig`
- `Makefile`
- `sc200pc.c`

Behavior:

- ACPI match
- power helper stubs
- identify reads
- minimal pad ops

### Commit 2

Title:

- `media: i2c: sc200pc: add ACPI firmware parsing for SSDB and board data`

Contents:

- parse firmware metadata
- replace hardcoded defaults

### Commit 3

Title:

- `media: i2c: sc200pc: add stream and mode configuration`

Contents:

- real mode table
- stream on/off registers
- controls

## Bring-up commands

After patching a kernel tree:

```bash
make olddefconfig
make menuconfig
```

Enable:

- `CONFIG_VIDEO_SC200PC=m`
- relevant IPU7 options already required by the platform

Build:

```bash
make -j$(nproc)
make modules_install
make install
```

Then verify:

```bash
dmesg | grep -i sc200pc
media-ctl -p
v4l2-ctl --list-devices
```

## Debugging priorities

If probe fails:

1. verify the driver actually binds to `SSLC2000`
2. verify the client address is `0x36`
3. log which regulators and GPIOs resolved
4. log each candidate ID register read
5. only then inspect ACPI resource parsing

If power-on appears to work but reads fail:

1. invert reset polarity
2. change `Power0` / `Power1` ordering
3. add longer delays
4. verify `xclk` handling
5. inspect whether the missing data is in `SSDB` / `_DSM`

## What not to put in `install.sh` yet

Do not try to "fix the camera automatically" in Omarchy until the kernel support exists.

Reason:

- the current blocker is kernel sensor support and board integration, not a missing package toggle

What `install.sh` can automate later, once a working driver exists:

- installing the patched kernel or DKMS package
- installing HAL graph / tuning assets
- enabling any required services
- dropping model-specific config gated by DMI detection
