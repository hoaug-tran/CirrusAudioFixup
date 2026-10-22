# CS35L56 support roadmap

Tracking: [hardware request #8](https://github.com/hoaug-tran/CirrusAudioFixup/issues/8).

CS35L56 is not supported yet. The first implementation target is read-only
diagnostics for the ASUS GU605MV; playback is a later, separately reviewed stage.
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
- [Linux CS35L56 HDA driver](https://github.com/torvalds/linux/blob/master/sound/hda/codecs/side-codecs/cs35l56_hda.c)
- [Linux CS35L56 shared implementation](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/cs35l56-shared.c)
- [Linux Meteor Point GPIO driver](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/intel/pinctrl-meteorpoint.c)
- [linux-firmware asset manifest](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/main/WHENCE)

Pin the relevant upstream revisions when implementing the corresponding code.
