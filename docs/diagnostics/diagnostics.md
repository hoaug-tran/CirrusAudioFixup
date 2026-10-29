# Audio Diagnostics & Troubleshooting on macOS

This guide explains how to inspect the driver state, capture live logs, and diagnose issues on macOS.

> [!NOTE]
> Routine driver messages require a **Debug** build or booting a **Release** build with the `-cirrusdbg` boot argument.

---

## 1. Quick Inspection Commands

Run these commands in Terminal to check whether the kext is loaded and communicating:

```bash
# Check macOS version build
sw_vers

# Check if CirrusAudioFixup is registered in IORegistry
ioreg -lw0 -p IOService -r -c CirrusAudioFixup

# Check loaded kernel extensions
kmutil showloaded | grep -Ei 'Cirrus|AppleALC|Lilu|VoodooI2C'

# Inspect the AppleHDA controller and audio output status
ioreg -lw0 -p IOService -r -c AppleHDAController
system_profiler SPAudioDataType
```

### What to Look For

* **`CirrusAudioFixup` service present**: Confirms the kext matched your hardware and attached.
* **`Cirrus_Driver_Verdict`**: Should show `READY_DSP` during normal operation (or `READ_ONLY` when `-cirrusro` is set).
* **`Cirrus_Bus`**: Confirms the transport bus (`I2C` or `SPI`).
* **`Cirrus_Detected_Amplifiers`**: Shows the count of active amplifiers (usually `2`).

---

## 2. Retained Logs and Live Messages

All messages from this driver are prefixed with `CirrusAudioFixup:`.

### Save Driver Logs (dmesg)

The simplest and most reliable way to capture driver logs:

```bash
sudo dmesg > ~/Desktop/cirrus-dmesg.txt
```

> [!TIP]
> If `dmesg` contains no driver logs (Apple suppresses routine kernel `IOLog` messages on modern macOS), install **[DebugEnhancer](https://github.com/acidanthera/DebugEnhancer)** and add the boot argument `-dbgenhdbg`.

### Stream Live Logs

To watch driver events and errors in real-time in Terminal:

```bash
sudo log stream --level debug --style syslog \
  --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "CirrusAudioFixup"'
```

### View Historical Logs

To query messages recorded during the last hour:

```bash
sudo log show --last 1h --style syslog --info --debug \
  --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "CirrusAudioFixup"'
```

---

## 3. Automated Report Collection

Use the automated collection script to gather all system evidence, IORegistry trees, unified logs, and crash reports into a single ZIP archive on your Desktop:

```bash
sudo bash Tools/Collectors/collect_macos.sh
```

When diagnosing a local build, pass the path to the `.kext` bundle to verify its binary metadata:

```bash
sudo bash Tools/Collectors/collect_macos.sh build/Local-Release/CirrusAudioFixup.kext
```

The output archive will be placed at `~/Desktop/cirrus-evidence.XXXXXX.zip`.

---

## 4. Driver and Playback Verdicts

### Driver Verdicts

| Verdict | Status | Description |
| :--- | :--- | :--- |
| `READY_DSP` | **Working** | Amplifiers initialized successfully with validated firmware and active DSP protection. |
| `READ_ONLY` | **Inspection** | Probing mode active (`-cirrusro`). Register writes and playback are disabled. |
| `READY_BYPASS_EXPLICIT` | **Warning** | Running in direct DAC bypass mode (`-cirrusnodsp`). Speaker protection is inactive. |
| `WAITING_FOR_CODEC_IDENTITY` | **Pending** | Waiting for AppleALC / AppleHDA to publish the codec subsystem ID. |
| `INCOMPLETE_TOPOLOGY` | **Error** | One or more expected amplifier endpoints could not be discovered on the bus. |
| `UNSUPPORTED_PLATFORM` | **Error** | The board subsystem ID does not match any authorized hardware profile. |
| `DEBUG_PHASE_HALTED` | **Debug** | Initialization stopped early due to the `-cirrusphase` argument. |
| `FAILED_INIT` | **Error** | General initialization failure during amplifier setup. |
| `FAULT_LATCHED` | **Error** | A protection fault or bus communication failure occurred; driver halted to protect hardware. |
| `SUSPENDED` | **Power** | Amplifiers are powered down in low-power sleep state. |

### Playback Verdicts

| Verdict | Status | Description |
| :--- | :--- | :--- |
| `SAFE_IDLE_VERIFIED` | **Idle** | Speakers are quiet; idle registers confirmed. |
| `ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED` | **Active** | Amplifiers unmuted and streaming; verify audible output with a listening test. |
| `CLEANUP_UNVERIFIED` | **Warning** | Post-playback shutdown did not reach the expected idle state. |
| `FAULT_LATCHED_OUTPUT_DISABLED` | **Error** | Playback disabled due to a recorded amplifier or power fault. |

---

## 5. Failure Codes

When an attachment or runtime failure occurs, `Cirrus_Attach_First_Reason` or `Cirrus_Diag_FirstFailure` records the primary cause:

| Code | Meaning | Recommended Action |
| :--- | :--- | :--- |
| `PROVIDER_MISSING` | Required SPI or I2C communication provider was not found. | Check ACPI tables, controller drivers, and VoodooI2C installation. |
| `I2C_TRANSFER` / `SPI_TRANSFER` | Low-level bus read/write failed. | Check bus wiring, ACPI resources, clock rates, and pull-up configuration. |
| `RESET_GPIO` / `RESET_WRITE` | Amplifier hardware reset pulse failed. | Check GPIO controller pin ownership and reset pin index in ACPI. |
| `DEVICE_ID` | Readout ID did not match a supported Cirrus chip (`0x35A40` or `0x35A56`). | Verify chip model; unsupported revisions (e.g., CS35L41 A0) are rejected. |
| `OTP_BOOT_ERROR` / `OTP_TIMEOUT` | OTP memory trim readout failed. | Inspect hardware supply voltages and power sequencing. |
| `FIRMWARE_MISSING` | No matching WMFW or BIN tuning files found in kext. | Check subsystem ID and speaker ID; verify resources in `FirmwareData.inc`. |
| `FIRMWARE_PARSE` / `COEFFICIENT_PARSE` | WMFW header, memory map, or BIN data corrupted. | File issue with original Linux/Windows firmware dump. |
| `DSP_BOOT` / `DSP_MAILBOX` | DSP core did not boot or mailbox command timed out. | Check clock delivery (ASP bit clock) and boost power supply. |
| `PLL_UNLOCKED` | Audio PLL lost lock on the incoming stream clock. | Verify AppleALC output format is set to **48 kHz stereo**. |
| `AMP_PROTECTION` | Silicon protection triggered (overcurrent, overtemperature). | Stop playback immediately; check speaker impedance and tuning coefficients. |
| `PLAYBACK_INVARIANT` | Register readback did not match active playback state. | Review unified logs for transfer errors during unmute. |

---

## 6. Common Issues

### No Sound or Distorted Audio: Verify 48 kHz Format

Cirrus smart amplifiers require a **48 kHz** hardware clock on the serial audio bus. If macOS defaults to 44.1 kHz, audio will not play or will distort:
1. Open **Audio MIDI Setup** (`/Applications/Utilities/Audio MIDI Setup.app`).
2. Select **Internal Speakers** and set **Format** to **48,000 Hz** (2 ch 16-bit or 24-bit).

### Speaker Pop on Microphone Activation

The driver monitors HDA output selectors, not microphone inputs. If opening an input pane causes an audible click:
1. Confirm whether `Cirrus_Audio_Event_Mode` reports `IOAUDIO_FAMILY_EVENTS` or `HDA_TIMER_FALLBACK`.
2. Inspect the AppleALC layout to ensure microphone input nodes do not toggle speaker amplifier mutes or switch the output clock.

### Sound Differences Compared to Windows

CirrusAudioFixup loads factory amplifier firmware and speaker protection coefficients. On Windows, OEMs often bundle userspace DSP processing (e.g., Waves MaxxAudio, Dolby Atmos, Nahimic) that applies EQ and multiband compression. These effects run on the CPU in Windows and are not part of the amplifier chip. Third-party macOS equalizer apps (such as SoundSource or eqMac) can help tune tonal balance.

[Back to main guide](../../README.md#installation)
