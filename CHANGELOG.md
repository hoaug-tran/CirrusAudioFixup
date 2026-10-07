# Changelog

## [1.0.0] - 2026-10-07

### Added
- Initial production-grade release of CirrusAudioFixup for macOS (Ventura 13.x, Sonoma 14.x, Sequoia 15.x).
- Driver support for dual Cirrus Logic CS35L41 digital smart amplifiers over I2C (`CLSA0100`).
- Hardware platform reset quirk for AMD GPIO controllers (`AMDI0030`) asserting and releasing amplifier reset lines via MMIO.
- Non-invasive PCI BAR0 snooping engine monitoring host HD Audio controller DMA stream descriptors (`SDnCTL` run bit and format decoding).
- Silicon initialization flow with OTP trim unpacking and Cirrus revision B2 errata sequence under test key locks.
- Power rail sequencing for external boost converters (`0x742C` and `0x7438`).
- Phase-Locked Loop (PLL) configuration locking to Realtek High Definition Audio bit clock (BCLK).
- Audio Serial Port (ASP) setup for I2S bus communication (48 kHz / 44.1 kHz, 24-bit/32-bit slot allocations).
- WMFW v2 container parsing and firmware bytecode staging into Halo DSP core memory (Program Memory, Data Memory, Zero Memory).
- Per-channel coefficient calibration injection (`.bin`) for Left (`l0`) and Right (`r0`) internal speaker arrays with CRC32 verification.
- Pop-free playback lifecycle using Halo DSP soft-mute curve (`kCmdMailboxSpeakerOutputEnable` and `kCmdMailboxSpeakerOutputDisable`).
- Dynamic power management integrated with macOS `IOPowerManagement` for sleep and wake cycles.
- Direct DAC bypass mode available via `-cirrusnodsp` for bring-up and hardware verification.
- Real-time IORegistry telemetry publishing driver verdicts, playback states, and circular I2C transaction trace buffers.
- Reference platform support for Lenovo Legion 7 16ACHg6 (`17AA:3847`) with Realtek ALC287 layout 16.
- Automatic Subsystem ID (SSID) quirk mapping Lenovo Legion 5 Pro 16ACH6H (`17AA:382B`) to verified Legion 7 firmware profiles.
- Host-side Python validation harness in `Tests/` with 8 regression test suites.
- Diagnostic collection tool (`Tools/collect_macos.sh`) and firmware conversion utility (`Tools/import_firmware.py`).
