# Boot Arguments Reference

CirrusAudioFixup uses OpenCore boot arguments for initial setup, troubleshooting, and diagnostic overrides. For normal playback on a verified laptop, no extra arguments are needed.

Add arguments to your OpenCore configuration under `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`.

> [!NOTE]
> Supported laptops work out of the box with no extra boot arguments. Use `-cirrusro -cirrusdbg` when bringing up an unverified machine or diagnosing an issue.

---

## Core Arguments

| Argument | Description |
| :--- | :--- |
| `-cirrusro` | **Read-only mode**: Inspects hardware without GPIO reset, register writes, or playback. Safe for initial bring-up or troubleshooting. |
| `-cirrusdbg` | **Debug logging**: Enables routine `IOLog` messages in Release builds (already enabled in Debug). If `dmesg` shows no logs, install [DebugEnhancer](https://github.com/acidanthera/DebugEnhancer) and add `-dbgenhdbg`. |
| `-cirrusoff` | **Disable kext**: Stops the driver before touching any hardware. Useful for isolating boot issues. |
| `-cirrusnodsp` | **DAC bypass mode**: Skips DSP firmware upload and runs in direct DAC bypass without acoustic speaker protection. |

---

## Diagnostics & Probing

| Argument | Description |
| :--- | :--- |
| `-cirrusphase=<name>` | Halts startup after a specific stage for debugging (`probe`, `otp`, `errata`, `clock`, `asp`, `gpio`, `platform`, `firmware`, `dsp`). |
| `-cirrusdelay=<ms>` | Snapshot delay in milliseconds for read-only inspection (default: `100`). |
| `-cirrusdiag` | Enables additional register snapshots and CRC checks during amplifier initialization. |
| `-cirruscompact` | Omits register names in snapshots for more compact log output. |
| `-cirrusdumptrace` | Publishes `Cirrus_Trace_Dump` in the read-only IORegistry snapshot. |
| `-cirruslegacyprobe` | Permits read-only fixed-address probing outside automatic profile policy. Does not authorize writes. |
| `-cirrusreset` | Permits GPIO hardware reset assertion under `-cirrusro` read-only probe mode to awaken uninitialized amplifiers for silicon ID verification. |
| `-cirrusirq` | Experimental GPIO84 protection interrupt delivery (polling remains default). |

### Valid Debug Phases

When using `-cirrusphase=<name>`, execution stops after the specified phase:

* `probe`: Silicon device ID readout.
* `otp`: One-Time-Programmable trim readout.
* `errata`: Silicon errata patches.
* `clock`: PLL and clock tree setup.
* `asp`: Audio Serial Port configuration.
* `gpio`: GPIO pin functions.
* `platform`: Board profile resolution.
* `firmware`: Stops before uploading WMFW image.
* `dsp`: Halts DSP core immediately after boot checks.

---

## Hardware Identity & Calibration Overrides

| Argument | Description |
| :--- | :--- |
| `-cirrusssid=<value>` | Overrides detected subsystem ID for resource lookup (e.g., `0x103C89C6` or `103C89C6`). |
| `-cirrusspkid=<value>` | Overrides speaker ID (0–255) for all channels when selecting firmware. |
| `-cirrusspkidl=<value>` | Overrides speaker ID for the left channel only (takes precedence over shared ID). |
| `-cirrusspkidr=<value>` | Overrides speaker ID for the right channel only (takes precedence over shared ID). |
| `-cirrusnocal` | Skips measured calibration injection while keeping DSP firmware and speaker tuning. |
| `-cirruscalr0=<value>` | Fallback measured DC resistance (R0) when EFI calibration is missing. |
| `-cirruscalr0l=<value>` | Left channel measured R0 fallback. |
| `-cirruscalr0r=<value>` | Right channel measured R0 fallback. |
| `-cirruscalstatus=<value>` | Calibration status byte for manual calibration (default: `1`). |
| `-cirruscalambient=<value>` | Ambient temperature value required when supplying manual R0. |

> [!WARNING]
> Calibration values must match the physical speaker impedance in firmware units. Never copy calibration values from an unrelated laptop model.

### HP ENVY x360 15-ey0xxx (`103C:8A31`) Configuration Notes

1. **Remove `-cirrusro`**: If present in `boot-args`, remove `-cirrusro`. This flag enforces read-only inspection and blocks all amplifier register writes and GPIO reset assertions, leaving speakers unpowered.
2. **AppleALC Layout**: Use `alcid=13` to select the HP ALC245 profile for speaker routing (Pin Complex Node `0x17`).
3. **Speaker Calibration**:
   - By default, CirrusAudioFixup operates without calibration overrides if no EFI calibration variable exists (`Cirrus_Calibration_Status = NOT_AVAILABLE`).
   - Smart-amplifier calibration is specific to each physical speaker unit. Never copy measured resistance values from another machine.
   - If you have extracted unit-specific factory calibration values from your own machine (e.g., via OEM diagnostic dumps or Windows driver registry), provide them via:
     ```text
     -cirruscalr0l=<left_r0> -cirruscalr0r=<right_r0> -cirruscalambient=<ambient_temp>
     ```
     - `-cirruscalr0l=<value>`: Unit-specific left speaker DC resistance ($R_0$) in firmware units.
     - `-cirruscalr0r=<value>`: Unit-specific right speaker DC resistance ($R_0$) in firmware units.
     - `-cirruscalambient=<value>`: Ambient temperature in °C recorded during calibration (mandatory when supplying manual $R_0$).
     - Optional status byte: `-cirruscalstatus=1` (defaults to `1`).

---

## Removed Arguments

* `-cirrusprobe`: Removed in 1.1.0. Use `-cirrusro` for read-only probing and `-cirrusdbg` for logging.

[Back to main guide](../../README.md#installation)
