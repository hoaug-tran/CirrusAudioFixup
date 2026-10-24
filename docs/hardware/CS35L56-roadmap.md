# CS35L56 support roadmap

Tracking: [hardware request #8](https://github.com/hoaug-tran/CirrusAudioFixup/issues/8).

CS35L56 playback is not supported yet. Experimental read-only diagnostics are
implemented for SPI `CSC3556` and await target validation on the ASUS GU605MV.
The reporter currently accepts read-only builds only. Writable testing requires
a willing tester and a recoverable EFI before that stage can be validated.

## Evidence baseline

The supplied Linux logs identify two CS35L56 B0 OTP3 amplifiers on SPI,
`CSC3556`, subsystem `10431C63`, and DSP system name `10431C63-spkid0`.
Linux reports shared reset, firmware 3.11.16 and calibration applied to both amps.
Audible woofer operation is reported by the tester; it has not been independently
verified by the project. The supplied macOS capture predates kext installation.

The three upstream firmware/tuning assets match the hashes supplied in #8:

| Asset | SHA-256 |
| --- | --- |
| `CS35L56_Rev3.11.16.wmfw` | `12c6d4fe63dc6a471473182dfdb72b1a91092e62db0038c48d9676d9566eff58` |
| `cs35l56-b0-dsp1-misc-10431c63-spkid0-amp1.bin` | `5732e3bab79d239dcbc937c1b1c1ef7518d32a2eb5737c6efca5c66819d699fa` |
| `cs35l56-b0-dsp1-misc-10431c63-spkid0-amp2.bin` | `4000b8794a0995c3a8b26a0ffcb39e7f1082047cb95dd1ab6fc1ebd22a3cac86` |

Firmware, calibration records and machine dumps stay outside this roadmap.
Confirm upstream licensing and required notices before embedding any new assets.

## Read-only implementation

Boot with `-cirrusro -cirrusdbg`, keeping the existing AppleALC configuration.
Without `-cirrusro`, diagnostic-only attachment is rejected before SPI ownership.
Read transactions configure the controller and toggle native/GPIO chip select;
they never write amplifier registers, release reset, wake the amplifier, load
firmware or start playback. A sleeping or reset-held device may not respond.

The SPI map permits only four identity registers: `DEVID=0x0`, `REVID=0x4`,
`RELID=0xc`, `OTPID=0x10`. Frames use 32-bit big-endian addresses and values,
two padding bytes and read flag `0x80`. Identity compares `DEVID & 0x00ffffff`
with `0x035a56`; revision `REVID & 0xff == 0xb0` is the captured B0 revision.
Other revisions are reported as unverified, never authorized for initialization.

GPIO layout comes from Linux's full Meteor Lake-P groups, including numbering
holes, the `0xb0` ownership base and the per-community group ownership words.
For the supplied DSDT, GPIO `0x82` maps to memory resource 1 / pad 27,
reset `0x181` to resource 4 / pad 26, and speaker ID `0x168` to resource 4 / pad 8.
Memory resources are resolved from the live ACPI provider; no physical MMIO
addresses or GU605MV-only wiring is embedded in the driver.

`Cirrus_SPI_*` properties preserve initial transfer results.
`Cirrus_CS35L56_<endpoint>_*` properties report identity reads and their validity
mask (bits 0..3 correspond to DEVID, REVID, RELID, OTPID).
`Cirrus_SPI_Reset_*` observes the reset output latch without toggling it;
`Cirrus_GPIO_Input_*` and `Cirrus_Platform_Discovery` report speaker ID sampling.
The Linux regmap captures contain `XXXXXXXX`; they do not establish live register
values and are not used to invent reset defaults or power sequences.

Collect target results from the repository root:

```sh
ioreg -lw0 -p IOService -r -c CirrusAudioFixup
sudo bash Tools/Collectors/collect_macos.sh
```

Host fixtures cover wire bytes, transfer failures, write/lifecycle rejection,
and the supplied ACPI cross-community topology. They do not prove target kext
load, actual SPI communication or physical audio behavior.

## Delivery gates

| Stage | Work | Completion evidence |
| --- | --- | --- |
| 1. Read-only bring-up | Verify ACPI endpoints, add the verified `CSC3556` attachment path, implement CS35L56 SPI framing/register reads and Meteor Lake `INTC1083` GPIO layout handling | Focused host checks, native Debug/Release builds, and target macOS logs identifying both amps; document controller/chip-select operations and prohibit amplifier writes, reset toggles and DSP loading in this stage |
| 2. DSP and resources | Implement a dedicated CS35L56 lifecycle, revision checks, firmware/tuning selection and measured EFI calibration handling | Fault/cleanup checks and a native build; separately authorized target tests before claiming DSP operation |
| 3. Audio route and playback | Verify ALC285 speaker routing, ASP clocks/slots, channel mapping and coordinated amplifier start/stop | Low-volume physical left/right woofer tests, with exact loaded artifact and logs |
| 4. Power and regression | Validate idle, output switching, suspend/resume and recovery; check existing CS35L41 paths | Target cold-boot and sleep/wake evidence plus existing Lenovo I2C/HP SPI regression results or explicit untested boundaries |

The existing `8086:7e27` controller profile is a starting point, not proof of
working transfers on this machine. Do not reuse CS35L41 register definitions or
power sequences for CS35L56. Pin mappings must come from verified ACPI resources
and upstream controller layouts rather than a global GU605MV-only mapping.

## Branch and pull request flow

The milestone tracks initial CS35L56 support across focused PRs. The initial
`feature/cs35l56-bringup` draft PR targets `develop`; this roadmap is its initial
change, and read-only implementation can be added to the same branch. Keep its
title, description and check results current as code lands.

Merge the first PR only after its read-only scope is reviewable and validated;
create subsequent feature branches from the updated `develop` for later stages.
Use `Related to #8` until the hardware request's supported playback scope is
actually complete. Release changes follow `feature/* -> develop -> main`.

## Primary references

- [Issue #8 and supplied logs](https://github.com/hoaug-tran/CirrusAudioFixup/issues/8)
- [Linux CS35L56 HDA driver](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/sound/hda/codecs/side-codecs/cs35l56_hda.c)
- [Linux CS35L56 register definitions](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/include/sound/cs35l56.h)
- [Linux CS35L56 shared implementation](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/sound/soc/codecs/cs35l56-shared.c)
- [Linux SPI regmap bus](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/base/regmap/regmap-spi.c)
- [Linux Meteor Lake GPIO driver](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/pinctrl/intel/pinctrl-meteorlake.c)
- [Linux Intel GPIO ownership and revision handling](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/pinctrl/intel/pinctrl-intel.c)
- [Linux LPSS PCI controller profiles](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/mfd/intel-lpss-pci.c)
- [linux-firmware asset manifest](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/main/WHENCE)

Register and GPIO facts above are pinned to Linux commit
`7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614`. Further DSP and playback work must pin
its sources and validate each lifecycle stage separately.
