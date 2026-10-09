# Changelog

## [Unreleased]

### Fixed
- Keep verified, muted PLL-loss recovery available when a headphone transition causes power-up completion to time out with the speaker clock still absent; retain I/O, protection, cleanup and clock-present power-up faults.
- Preserve specific playback-start failure diagnostics instead of overwriting them with a generic invariant failure.

### Validation
- Host regressions cover prolonged headphone clock absence, speaker recovery after clock return, and retained protection/clock-present power-up fault latches.
- Target macOS build and physical headphone insertion/removal validation remain required.

## [1.0.1] - 2026-10-10

### Fixed
- Read-only startup no longer asserts or releases the platform GPIO reset.
- Restrict initialization to verified stereo endpoints and route slots/tuning by explicit channel identity.
- Enforce debug stage stops in the production initialization flow; reject unknown phase names.
- Limit automatic recovery to transient PLL failures; retain initialization, firmware, protection, and I/O faults.
- Cache HDA controller/BAR ownership until suspend or termination and publish status only on changes.
- Yield between playback power/PLL/mailbox polls rather than busy-waiting; stop transport polling at the first bus error.
- Publish releases only from matching version tags, with checksum and commit metadata; existing releases cannot be overwritten.
- Release the parsed firmware image when stopping at the firmware debug phase.

### Changed
- Dispatch initialization by silicon model before entering the CS35L41 lifecycle.
- Debug enables routine logging by default; Release enables it with `-cirrusdbg`. Safety checks remain identical.
- Retain diagnostic symbols in both builds and package matching dSYMs with UUID verification in CI.
- Lower the x86_64 deployment target to macOS 11.0; Big Sur through Tahoe remains a compatibility target, not a tested OS matrix.
- Document code ownership and hardware sequencing; correct installation, calibration, safety and macOS compatibility claims.
- Clarify Tahoe host-stack restoration, untested VoodooHDA combinations and microphone-transition evidence collection.
- Preserve collector command failures and identify empty service queries; remove misleading AppleALC/CS35L41 class queries.
- Reject invalid right-channel tuning during validation and no longer silently substitute left tuning for an explicitly missing right-channel file.

### Validation
- Host regressions cover stage stops, unsupported backends, HDA cache ownership, and playback fault handling.
- Target macOS build and physical playback/sleep-wake validation are required before tagging this version.

## [1.0.0] - 2026-10-07

The scope descriptions below have been corrected against the source: this entry is not an OS validation matrix or a guarantee of pop-free playback.

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
