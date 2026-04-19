# libcamera-sc200pc

Fork of the Arch `libcamera` package with the Samsung/SmartSens SC200PC
sensor helper + `CameraSensorProperties` entry patched in. Drop-in
replacement for the stock `libcamera` split set — installing this
package set takes `libcamera`, `libcamera-ipa`, `libcamera-tools`,
`libcamera-docs`, `gst-plugin-libcamera`, and `python-libcamera` off the
system and replaces them with `*-sc200pc` variants that `provides=` /
`conflicts=` / `replaces=` their stock counterparts.

This is the right shape for Arch distribution: `pacman -Syu` respects
the pin and won't silently revert to stock libcamera. No post-install
rebuild dance, no sed-splicing of upstream PKGBUILDs.

## Why stock libcamera is not enough

Stock libcamera does not know about SC200PC. Without a
`CameraSensorHelper` registration and a `CameraSensorProperties` entry,
`cam -l` prints:

```
WARN  No static properties available for 'sc200pc'
WARN  IPASoft: Failed to create camera sensor helper for sc200pc
```

The simple soft IPA then treats the raw V4L2 gain code as a linear gain
multiplier. The SC200PC gain register is 1/16 linear (code 0x10 = 1.0×,
0xff = 15.94×), so AGC converges near 1.0× and AWB has no signal. Frames
come out dark and green-tinted.

## Patches applied

- [`patches/libcamera-sc200pc.patch`](../../patches/libcamera-sc200pc.patch)
  — adds `CameraSensorHelperSc200pc` (linear 1/16 gain model, BL 64@10b)
  and a `CameraSensorProperties` entry (1.75 µm unit cell, sensor delays).
- `libcamera-fix-python3.14-macro-redefinition.patch` — carried over
  from Arch's libcamera PKGBUILD, identical to upstream's Arch-side fix.

## Extra assets bundled

- `/usr/share/libcamera/ipa/simple/sc200pc.yaml` — first-pass tuning
  (BlackLevel, conservative CCMs, AGC/AWB enabled).
- `/usr/bin/sc200pc-libcamera-check` — diagnostic helper.

## Rebase on Arch libcamera bumps

When Arch pushes a new libcamera release:

1. `pkgctl repo clone --protocol=https libcamera` for the new Arch PKGBUILD
2. Bump `_upstream_pkgver` / `pkgver` in this PKGBUILD to match
3. Sync any new distro-specific patches Arch added (e.g. the
   `fix-python3.14-macro-redefinition.patch` may come and go)
4. Rebase `patches/libcamera-sc200pc.patch` if upstream moved the helper
   or properties files
5. `makepkg --printsrcinfo > .SRCINFO` and push to AUR
