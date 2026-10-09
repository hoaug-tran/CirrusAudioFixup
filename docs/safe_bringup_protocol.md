# Bring-up and diagnostic checks

Use this guide before testing a new board profile, firmware image or macOS build. The driver controls boost and speaker output stages. Register checks reduce the risk of a bad sequence; they cannot guarantee speaker safety.

Keep a known-good EFI that disables CirrusAudioFixup and can boot independently. Use low volume during playback checks. Stop if a speaker distorts, heats unexpectedly, or the driver reports a retained fault. Do not force another laptop's SSID or calibration to get past an error.

## 1. Inspect without initialization

Use these boot arguments for the first diagnostic boot:

```text
-cirrusro -cirrusdbg -cirrusdumptrace
```

Read-only mode skips board GPIO reset, amplifier initialization and register writes. It reads device identity and registers through I2C combined transfers; those reads still transmit an address prefix. It does not monitor HDA playback or initialize the DSP.

Inspect the actual driver service:

```bash
ioreg -lw0 -p IOService -r -c CirrusAudioFixup
sudo bash Tools/collect_macos.sh
```

Check the provider and ACPI HID, detected amplifier count, silicon/revision and both channels' first/latest failures. Keep the collector output before rebooting. Zero detected amplifiers is a discovery problem, not permission to force initialization.

The shipped personality matches `CLSA0100` only. A staged `CSC3551` profile does not make that device attach automatically.

## 2. Check initialization boundaries

Remove `-cirrusro` and select one phase at a time:

```text
-cirrusdbg -cirrusphase=otp
```

Valid names are `probe`, `otp`, `errata`, `clock`, `asp`, `gpio`, `platform`, `firmware` and `dsp`.

- `probe` discovers silicon without board GPIO reset or chip initialization.
- Later phases allow the preceding register operations. Verify the board profile before using them.
- `firmware` uploads and validates firmware, tuning and available calibration, then stops before DSP boot.
- `dsp` runs DSP boot checks and halts the core before returning.
- All phase modes disable playback. Unknown phase names reject startup before reset.

A completed phase reports `DEBUG_PHASE_HALTED`. A channel fault takes precedence and reports `FAULT_LATCHED`. Check both channels: one successful phase must not hide a failed transfer on the other amplifier.

A PLL may not lock while the HDA route is idle because BCLK is absent. This does not justify skipping the later playback lock check.

## 3. Validate the audio route and matching DSP profile

For the reference ALC287 layout, use `alcid=16` and the required custom VoodooI2C build. Other boards need their own verified route and speaker tuning.

Remove the phase argument only after checking the selected firmware and left/right coefficient resources. Keep `-cirrusdbg` while testing:

```text
alcid=16 -cirrusdbg
```

Before playback, inspect:

```bash
ioreg -lw0 -p IOService -r -c CirrusAudioFixup
sudo /usr/bin/log show --last 1h --style syslog --info --debug --predicate 'eventMessage CONTAINS "CirrusAudioFixup"'
```

`READY_DSP` means the driver's initialization and DSP checks passed. It does not certify measured speaker calibration or a correct acoustic profile. Inspect `Cirrus_Calibration_Status_left/right` separately. Missing measurements are reported; never fill them with guessed values.

On Tahoe, first establish a working host audio stack. AppleALC upstream notes that AppleHDA was removed from macOS 26 DP2. The kext's PCI scanner cannot replace the missing codec driver or CoreAudio device. See the [compatibility section](../README.md#macos-compatibility).

### Controlled bypass testing

`-cirrusnodsp` is an optional diagnostic mode for a board whose reset, boost and routing have already been verified. It skips DSP firmware and tuning, so DSP acoustic protection is absent.

Use a short, low-volume test only when it is needed to isolate the DSP path. A successful bypass test does not prove that a firmware image is wrong, and does not establish long-term speaker safety. Do not make bypass the default first step for an unknown board.

## 4. Exercise playback and power transitions

Begin with quiet stereo playback and verify left and right independently. Test repeated start/stop, headphone insertion/removal, idle and sleep/wake.

Also test the first microphone activation after idle and after wake, with playback stopped and with quiet playback running. Do not repeat a loud pop to collect more samples. Preserve the pre/post snapshots and timestamps described in [diagnostics.md](diagnostics.md); a pop is not automatically a layout-id fault.

`ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED` means the active register checks passed. Listen and verify the downstream route separately. `CLEANUP_UNVERIFIED` means the expected idle state was not established; stop the test and preserve the logs.

I/O, initialization, DSP and protection faults remain latched. Only an isolated PLL failure may receive bounded automatic retries. Repeated faults need investigation, not repeated forced reboots.

Record the exact commit, build configuration, macOS build, CPU/platform, codec layout, I2C provider build and firmware/tuning IDs. Both Debug and Release need this validation; their safety checks are the same.

## If a test fails

Do not continue playback. Collect evidence before restarting:

```bash
sudo bash Tools/collect_macos.sh
```

Then boot the fallback EFI, use `-cirrusoff`, or return to `-cirrusro -cirrusdbg` for inspection. Read-only mode avoids initialization writes by this driver; it cannot control another kext's writes or undo a power state left by earlier software.

Use the first failure to identify the starting point. Keep later failures too: an unsuccessful power-down or DSP halt can matter as much as the original error.
