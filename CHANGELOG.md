# Changelog

## [1.2.1]

### CS35L41

- Added Lenovo Legion Slim 7 Gen 7 (2022) support (`CSC3551`, subsystem `17AA:3877` / PCI `17AA:385C`) over I2C with unswitched external boost topology ([#21](https://github.com/hoaug-tran/CirrusAudioFixup/issues/21)).
- Added support for unswitched external boost topology (`ExternalWithoutSwitch`) and software-only reset over I2C on CS35L41 amplifiers.

### Platform & Infrastructure

- Enforced fail-closed power management transitions on startup failure.
- Hardened service lifecycle teardown idempotency and added null guards for audio event observers and HDA controller references.
- Fixed `RegisterMap` non-incrementing FIFO transfers to properly evaluate bounds without scalar permission rejection.
- Enabled develop push builds and automatic kext artifact uploads in CI workflows.

## [1.2.0] - 2026-11-06

### CS35L41

- Added HP ENVY x360 15-ey0xxx support (`CSC3551`, subsystem `103C:8A31`) over I2C with internal boost topology.
- Added Alienware x17 R2 / Dell Horizon 17 support (`CSC3551`, subsystem `1028:0B28`) over SPI with Tiger Lake-LP GPIO chip select.
- Added ASUS Zenbook 14 UM3402Y support (`CSC3551`, subsystem `1043:1683`) over I2C with switched external boost.
- Added Tiger Lake-LP GPIO controller support (`INTC1055`, `INT34C5`) for native and GPIO-driven SPI chip select lines.
- Fixed ACPI `_DSD` metadata evaluation to handle `kIOReturnLockedWrite` (`0xE00002C4`) and `kIOReturnUnsupported`, ensuring clean fallback to verified board metadata.
- Corrected HP ZBook Fury 16 G9 GPIO resources in board tests.
- Added clock wait recovery on cold start power-up timeout.
- Added authorized AMD GPIO reset preparation before I2C amplifier identity reads.
- Corrected AMD GPIO reset pulse polarity to assert low and release high for active-low lines.
- Synchronized GPIO debounce revision threshold to `0x92` to match Linux driver specifications.

Implementation: [PR #11](https://github.com/hoaug-tran/CirrusAudioFixup/pull/11), [PR #13](https://github.com/hoaug-tran/CirrusAudioFixup/pull/13) (closes [#12](https://github.com/hoaug-tran/CirrusAudioFixup/issues/12)), [PR #15](https://github.com/hoaug-tran/CirrusAudioFixup/pull/15) (closes [#14](https://github.com/hoaug-tran/CirrusAudioFixup/issues/14)).

### CS35L56

- Added initial CS35L56 B0 smart amplifier family driver for both SPI and I2C buses.
- Added ASUS ROG Zephyrus G16 GU605MV (2024) support (`CSC3556`, subsystem `1043:1C63`) over SPI.
- Added Lenovo ThinkBook 16p G6 IAX support (`CSC3556`, subsystem `17AA:391E`) over I2C (`0x31` Left, `0x30` Right) with software register reset.
- Added ASUS ROG Zephyrus G14 GA403UV support (`CSC3556`, subsystem `1043:1B13`) over I2C with 4-channel HDA stream support.
- Added multi-frame sequence write (`writeSequence`) on `I2CRegisterBus` for chunked WMFW uploads over I2C.
- Filtered broadcast I2C addresses (`0x3A`, `0x3C`) from endpoint discovery.
- Added dynamic ASP RX channel routing based on amplifier binding index.
- Added per-amplifier EFI calibration decoding (`CirrusSmartAmpCalibrationData`) and runtime verification.
- Fixed firmware-boot ordering before device identification and IRQ mask readback validation.

Implementation: [PR #10](https://github.com/hoaug-tran/CirrusAudioFixup/pull/10) (closes [#8](https://github.com/hoaug-tran/CirrusAudioFixup/issues/8)), [PR #17](https://github.com/hoaug-tran/CirrusAudioFixup/pull/17) (closes [#16](https://github.com/hoaug-tran/CirrusAudioFixup/issues/16)), [PR #19](https://github.com/hoaug-tran/CirrusAudioFixup/pull/19) (closes [#18](https://github.com/hoaug-tran/CirrusAudioFixup/issues/18)).

### Platform & Infrastructure

- Added native ACPI `_DSD` reference evaluation and `_CRS` serial-bus/GPIO resource discovery.
- Supported 4-channel HDA stream formats (`0x0033`: 48 kHz, 24-bit, 4 channels) in `HDAStreamWatcher` and playback validation.
- Deferred startup for dynamic ACPI profiles (`CSC3551`, `CSC3556`) when PCI subsystem ID publication is pending.
- Added Meteor Lake-P GPIO controller support (`INTC1083`, `INTC105E`).
- Extended bounded WMFW uploads for larger images and region counts, keeping workspaces off the kernel stack.
- Automated release publishing and draft management in CI workflows.
- Added native macOS Debug and Release builds to pull request verification.
- Documented quad-speaker codec routing and HDA verb sequences in `docs/hardware/codec_route.md`.

## [1.1.1] - 2026-10-21

[Pull request #9](https://github.com/hoaug-tran/CirrusAudioFixup/pull/9)

### Added

- Added CS35L41 support for ASUS ROG Strix G16 G614JI (`1043:1C9F`) over SPI.
- Added automatic board configuration using ACPI resources and built-in metadata, without requiring a custom SSDT.
- Added GPIO-based speaker ID detection and validation.
- Improved diagnostic logs for amplifier initialization, firmware loading and SPI errors.

### Fixed

- Fixed Intel SPI power management, including PCI D3hot handling and delayed power requests.
- Improved SPI controller initialization, chip-select handling and transfer error detection.
- Fixed playback handling when the HDA stream changes or uses an unsupported format.
- Improved amplifier startup, mute, shutdown and wake handling.

### Changed

- Reorganized the driver into separate service, platform, transport and amplifier modules.
- Moved embedded firmware data into a single compilation unit.
- Updated build tools, diagnostics and regression tests.

## [1.1.0] - 2026-10-19

Released in [#7](https://github.com/hoaug-tran/CirrusAudioFixup/pull/7), including native SPI support from [#6](https://github.com/hoaug-tran/CirrusAudioFixup/pull/6).

### Added

- Native SPI support for profiled Intel LPSS SPT/BXT/CNL/TGL controllers and AMD AMDI0061/AMDI0062 controllers.
- ACPI I2C/SPI detection, SPI endpoint parsing, native/GPIO chip select and supported reset-GPIO handling.
- Added HP ZBook Fury 16 G9 (`103C:89C6`) support over SPI with internal boost topology.
- Shared amplifier registration and runtime for I2C and SPI, with separate silicon, board and controller modules.
- Per-channel speaker ID overrides and an explicit distinction between generic tuning and physical speaker ID 0.
- Linux and Windows audio collectors for device identity, ACPI, controller and OEM firmware data, including the general `collect_audio_amp_linux.sh` and `Collect-AudioAMP-Windows.ps1` tools.
- Attachment status, boot-flow markers and per-channel diagnostics that retain the original failure and native I/O result.

### Fixed

#### Initialization and firmware

- Require an authorized board identity and the complete configured amplifier topology before initialization; probing no longer invents missing endpoints.
- Keep read-only startup free of platform reset and amplifier writes.
- Reject unsupported amplifier models before CS35L41 register access.
- Wait for late codec/board identity publication before selecting firmware; stop at a bounded timeout when identity remains unavailable.
- Preserve the explicit Lenovo speaker ID without requiring a boot argument, and keep the legacy firmware alias from replacing an explicit board firmware identity.
- Reject missing, empty, dummy or misaligned channel coefficients instead of substituting left tuning for right.
- Validate firmware upload bounds, calibration records and UID reads; keep measured calibration separate from speaker tuning.
- Enforce named debug phase stops, reject unknown phases before reset and release the parsed image when stopping before DSP boot.
- Reject malformed or duplicate ACPI property packages and handle retained invalid pairs without a null dereference.

#### Playback and power

- Advance runtime power, PLL and mailbox waits through timed phases; keep synchronous shutdown for power management.
- Prepare both speakers before unmute, cancel stale transitions and roll back the group after a failed commit.
- Restore and verify the complete playback signal path, including RX6 and the unused outputs in DSP bypass.
- Select DSP supply-monitor sources by boost topology.
- Remove undefined mailbox command 8; use supported pause commands and check the actual acknowledgment bank and error states.
- Correct the low-level stop helper to mute and clear AMP_EN; prevent the start helper from bypassing the verified lifecycle.
- Check power-down readback and silicon identity instead of reporting success from incomplete helper checks.
- Recover from supported transient PLL loss after headphone switching, including prolonged clock absence; retain protection, I/O and clock-present power-up faults.
- Keep the original playback failure when later checks or cleanup also fail.
- Convert Mach absolute time before reporting reset duration.

#### Controllers and HDA

- Limit CS35L41 register access to 0x03804FE8 and reject bulk transfers that cross the limit.
- Bound SPI polling and keep firmware evaluation outside transaction locks.
- Acquire Intel PCI ownership and power before MMIO, check memory decoding, and prepare LPSS reset and clocks before SSP access.
- Use ACPI chip-select/reset resources and retain failed endpoint responses for diagnosis.
- Apply AMD SPI polling, lock-order and power-state checks to the native controller lifecycle.
- Keep HDA controller/BAR ownership until suspend or removal and release it on failed mappings or teardown.
- Retry an unknown output route without replacing a newer audio event; wait for a fresh route after wake.
- Bound diagnostic register snapshots and preserve partial results after a bus failure.

### Changed

- Target macOS 11.0 and newer on x86_64. Ventura 13 and Sequoia 15 are confirmed working on the supported configurations.
- Enable routine logs by default in Debug and with `-cirrusdbg` in Release; keep essential checks in both builds.
- Remove `-cirrusprobe`, which only recorded a flag and did not enable a separate probe mode.
- Save macOS diagnostic captures on the invoking user's Desktop with readable ownership; retain failed commands and empty results.
- Limit Windows collection to matching audio devices/packages, preserve matching SetupAPI sections and apply a payload size budget.
- Require explicit speaker identity and right-channel tuning during firmware import; allow symmetric tuning only when requested.
- Fix the importer's default resource path, reject duplicate/conflicting entries and handle filenames containing punctuation.
- Rewrite installation and bus-detection instructions; add Linux codec-dump and AppleALC route checks, with advanced boot options in a separate reference.
- Update bug, hardware and improvement forms and the PR template for I2C/SPI, exact build identity and collector evidence; document first-boot values and stop conditions.
- Remove the old prebuilt dependency archive from the source tree.

### Build and tests

- Pin MacKernelSDK and Lilu headers, verify kernel class metadata, and build Debug/Release artifacts for every branch.
- Keep matching dSYMs in both configurations and verify their UUIDs against the kext executable.
- Check version tags, bundle versions, changelog and main-branch ancestry before publishing; include checksums and build metadata and reject release replacement.
- Correct the `lipo -verify_arch` argument order in build verification.
- Add regressions for native Intel/AMD SPI, ACPI/GPIO ownership, firmware identity, calibration, grouped playback, fault rollback, PM and collectors.
- Allow host fixtures to run on Windows while explicitly skipping unavailable sanitizer runtimes; retain sanitizer variants on supported hosts.

## [1.0.0] - 2026-10-07

Initial driver implementation: [#1](https://github.com/hoaug-tran/CirrusAudioFixup/pull/1).

### Added

- Initial release of CirrusAudioFixup with a macOS Ventura deployment target.
- Driver support for dual Cirrus Logic CS35L41 digital smart amplifiers over I2C (`CLSA0100`).
- Hardware platform reset quirk for AMD GPIO controllers (`AMDI0030`) asserting and releasing amplifier reset lines via MMIO.
- Non-invasive PCI BAR0 snooping engine monitoring host HD Audio controller DMA stream descriptors (`SDnCTL` run bit and format decoding).
- Silicon initialization flow with OTP trim unpacking and Cirrus revision B2 errata sequence under test key locks.
- Power rail sequencing for external boost converters (`0x742C` and `0x7438`).
- Phase-Locked Loop (PLL) configuration locking to Realtek High Definition Audio bit clock (BCLK).
- Audio Serial Port (ASP) setup with fixed slot timing; the HDA format gate accepts 48 kHz stereo, not a 44.1 kHz hardware stream.
- WMFW v2 container parsing and firmware bytecode staging into Halo DSP core memory (Program Memory, Data Memory, Zero Memory).
- Per-channel tuning coefficient injection (`.bin`) for left (`l0`) and right (`r0`) speakers with CRC32 checks; measured calibration is separate.
- Playback lifecycle using Halo DSP speaker-output mailbox commands; these commands alone do not establish pop-free hardware behavior.
- Dynamic power management integrated with macOS `IOPowerManagement` for sleep and wake cycles.
- Direct DAC bypass mode available via `-cirrusnodsp` for bring-up and hardware verification.
- Real-time IORegistry telemetry publishing driver verdicts, playback states, and circular I2C transaction trace buffers.
- Reference platform support for Lenovo Legion 7 16ACHg6 (`17AA:3847`) with Realtek ALC287 layout 16.
- Automatic Subsystem ID (SSID) mapping Lenovo Legion 5 Pro 16ACH6H (`17AA:382B`) to Legion 7 firmware resources; physical speaker/boost equivalence requires validation.
- Host-side Python validation harness in `Tests/` with 8 regression test suites.
- Diagnostic collection tool (`Tools/collect_macos.sh`) and firmware conversion utility (`Tools/import_firmware.py`).
