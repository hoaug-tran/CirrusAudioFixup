# Boot Arguments Reference

CirrusAudioFixup uses OpenCore boot arguments for initial setup, troubleshooting, and diagnostic overrides. For normal playback on a verified laptop, no extra arguments are needed.

Add arguments to your OpenCore configuration under `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`.

> [!NOTE]
> Supported laptops work out of the box with no extra boot arguments. Use `-cirrusro -cirrusdbg` when bringing up an unverified machine or diagnosing an issue.

---

## Core Arguments

| Argument       | Description                                                                                                                                                                                                             |
| :------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-cirrusro`    | **Read-only mode**: Inspects hardware without GPIO reset, register writes, or playback. Safe for initial bring-up or troubleshooting.                                                                                   |
| `-cirrusdbg`   | **Debug logging**: Enables routine `IOLog` messages in Release builds (already enabled in Debug). If `dmesg` shows no logs, install [DebugEnhancer](https://github.com/acidanthera/DebugEnhancer) and add `-dbgenhdbg`. |
| `-cirrusoff`   | **Disable kext**: Stops the driver before touching any hardware. Useful for isolating boot issues.                                                                                                                      |
| `-cirrusnodsp` | **DAC bypass mode (CS35L41 only)**: Skips DSP firmware upload and runs in direct DAC bypass without acoustic speaker protection.                                                                                        |

---

## Diagnostics & Probing

| Argument              | Description                                                                                                                                                                        |
| :-------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-cirrusphase=<name>` | Halts startup after a specific stage for debugging. CS35L41: `probe`, `otp`, `errata`, `clock`, `asp`, `gpio`, `platform`, `firmware`, `dsp`. CS35L56: `probe`, `firmware`, `dsp`. |
| `-cirrusdelay=<ms>`   | Snapshot delay in milliseconds for read-only inspection (default: `100`).                                                                                                          |
| `-cirrusdiag`         | Enables additional register snapshots and CRC checks during CS35L41 initialization.                                                                                                |
| `-cirruscompact`      | Omits register names in CS35L41 snapshots for more compact log output.                                                                                                             |
| `-cirrusdumptrace`    | Publishes `Cirrus_Trace_Dump` in the read-only IORegistry snapshot.                                                                                                                |
| `-cirruslegacyprobe`  | Permits read-only fixed-address probing outside automatic profile policy. Does not authorize writes.                                                                               |
| `-cirrusirq`          | Experimental GPIO84 protection interrupt delivery (polling remains default).                                                                                                       |

### Valid Debug Phases

When using `-cirrusphase=<name>`, execution stops after the specified phase:

- `probe`: Silicon device ID readout _(CS35L41, CS35L56)_.
- `otp`: One-Time-Programmable trim readout _(CS35L41)_.
- `errata`: Silicon errata patches _(CS35L41)_.
- `clock`: PLL and clock tree setup _(CS35L41)_.
- `asp`: Audio Serial Port configuration _(CS35L41)_.
- `gpio`: GPIO pin functions _(CS35L41)_.
- `platform`: Board profile resolution _(CS35L41)_.
- `firmware`: Stops before uploading WMFW image _(CS35L41, CS35L56)_.
- `dsp`: Halts DSP core immediately after boot checks _(CS35L41, CS35L56)_.

---

## Hardware Identity & Calibration Overrides

| Argument                    | Description                                                                                           |
| :-------------------------- | :---------------------------------------------------------------------------------------------------- |
| `-cirrusssid=<value>`       | Overrides detected subsystem ID for resource lookup (e.g., `0x103C89C6` or `103C89C6`).               |
| `-cirrusspkid=<value>`      | Overrides speaker ID (0-255) for all channels when selecting CS35L41 firmware.                        |
| `-cirrusspkidl=<value>`     | Overrides speaker ID for the left channel only on CS35L41 (takes precedence over shared ID).          |
| `-cirrusspkidr=<value>`     | Overrides speaker ID for the right channel only on CS35L41 (takes precedence over shared ID).         |
| `-cirrusnocal`              | Skips measured calibration injection on CS35L41 while keeping DSP firmware and speaker tuning.        |
| `-cirruscalr0=<value>`      | Shared fallback measured DC resistance ($R_0$, `1-65535`) on CS35L41 when EFI calibration is missing. |
| `-cirruscalr0l=<value>`     | Left channel measured $R_0$ fallback on CS35L41 (takes precedence over `-cirruscalr0`).               |
| `-cirruscalr0r=<value>`     | Right channel measured $R_0$ fallback on CS35L41 (takes precedence over `-cirruscalr0`).              |
| `-cirruscalstatus=<value>`  | Calibration status byte (`0-255`) for manual CS35L41 calibration (default: `1`).                      |
| `-cirruscalambient=<value>` | Ambient temperature in °C (`-128` to `127`); mandatory when supplying manual $R_0$ on CS35L41.        |

> [!WARNING]
> Calibration values must match the physical speaker impedance in firmware units. Never copy measured resistance values from another machine.

### Manual Speaker Calibration (Missing EFI Calibration)

Some laptops do not expose the `CirrusSmartAmpCalibrationData` (or `SmartAmpCalibrationData`) EFI variable in macOS NVRAM, causing `Cirrus_Calibration_Status_<channel>` to report `NOT_AVAILABLE`.

- **Default behavior (CS35L41):** No boot arguments are required. CirrusAudioFixup continues operating with embedded DSP firmware and speaker tuning without calibration overrides.
- **Manual override (CS35L41 only):** If you have extracted unit-specific factory calibration values from your own machine, supply them via boot arguments (both decimal and `0x` hex are accepted by macOS):

    ```text
    -cirruscalr0l=<left_r0> -cirruscalr0r=<right_r0> -cirruscalambient=<ambient_temp>
    ```

    - `-cirruscalr0l=<value>` / `-cirruscalr0r=<value>`: Left and right speaker DC resistance ($R_0$, `1-65535`) in raw Cirrus DSP firmware units (or `-cirruscalr0=<value>` as a shared fallback).
    - `-cirruscalambient=<value>`: Ambient temperature in °C (`-128` to `127`) recorded during calibration. **Mandatory** when supplying manual $R_0$; omitting it or providing out-of-range values fails calibration with `INVALID_BOOT_ARG_DATA`.
    - `-cirruscalstatus=<value>`: Optional status byte (`0-255`, defaults to `1`).
    - The DSP checksum (`CAL_CHECKSUM = R0 + 1`) is computed automatically by the driver.

#### How to Extract and Decode Calibration Values from Your Machine

Do **not** estimate `R0` from a multimeter reading in Ohms: `CAL_R` uses Cirrus DSP fixed-point firmware units whose Ohm scaling depends on the amplifier's boost and current-sense gain in the tuning `.bin`. Always extract the factory values from your own machine:

1. **From Linux (`dmesg` or `efivars`):**
    - Check kernel logs (`dmesg | grep -iE 'cs35l41|cal_r|cal_ambient'`), which print the decoded `cal_r` and `cal_ambient` directly, or dump the raw EFI variable. For ex:
        ```bash
        xxd /sys/firmware/efi/efivars/CirrusSmartAmpCalibrationData-02f9af02-7734-4233-b43d-93fe5aa35db3
        # or on HP laptops:
        xxd /sys/firmware/efi/efivars/SmartAmpCalibrationData-53559579-8753-4f5c-9130-e82acfb8d893
        ```
        _(Note: Linux `/sys/firmware/efi/efivars/*` files start with a 4-byte EFI attribute header; skip those first 4 bytes before decoding.)_
2. **From Windows:**
    - Run `Tools/Collectors/Collect-AudioAMP-Windows.ps1` to capture the EFI calibration variable and Cirrus driver registry keys.
3. **Decoding the raw EFI calibration blob (Cirrus ABI):**
    - Bytes `0..3`: total payload size (`uint32` little-endian).
    - Bytes `4..7`: record count (`uint32` little-endian).
    - Each 20-byte record starts at byte `8 + 20 * index` (`index = 0` for Left, `1` for Right):

        | Record Offset | Type        | Field        | Mapping to Boot Argument                      |
        | :------------ | :---------- | :----------- | :-------------------------------------------- |
        | `+0..+7`      | `uint64` LE | `calTarget`  | Silicon UID (`0` = wildcard)                  |
        | `+8..+15`     | `uint64` LE | `calTime`    | Calibration timestamp (`0` = unused slot)     |
        | `+16`         | `int8`      | `calAmbient` | `-cirruscalambient=<dec>`                     |
        | `+17`         | `uint8`     | `calStatus`  | `-cirruscalstatus=<dec>` (`1` = valid)        |
        | `+18..+19`    | `uint16` LE | `calR`       | `-cirruscalr0l=<dec>` / `-cirruscalr0r=<dec>` |

    - **Example:** If bytes `+16..+19` of record 0 are `17 01 D6 16`:
        - `calAmbient` = `0x17` = `23` (°C)
        - `calStatus` = `0x01` = `1`
        - `calR` = little-endian `0x16D6` = `(0x16 × 256) + 0xD6` = **`5846`**
        - Resulting boot-args: `-cirruscalr0l=5846 -cirruscalr0r=<right_r0> -cirruscalambient=23`

- **CS35L56 note:** Manual calibration boot-argument overrides are not supported on CS35L56. CS35L56 reads calibration strictly from the `CirrusSmartAmpCalibrationData` EFI variable.

---

## Removed Arguments

- `-cirrusprobe`: Removed in 1.1.0. Use `-cirrusro` for read-only probing and `-cirrusdbg` for logging.

[Back to main guide](../../README.md#installation)
