# Changelog

## [1.1.0] - Unreleased

### Added

- Native SPI support for profiled Intel LPSS SPT/BXT/CNL/TGL controllers and AMD AMDI0061/AMDI0062 controllers.
- ACPI I2C/SPI detection, SPI endpoint parsing, native/GPIO chip select and supported reset-GPIO handling.
- HP CSC3551 internal-boost configuration, including 103C89C6 firmware and separate left/right speaker tuning.
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
