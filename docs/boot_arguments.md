# Boot argument reference

Start the first boot with `-cirrusro -cirrusdbg` for read-only inspection and logging. After checking the hardware, remove `-cirrusro` and reboot to allow initialization. Normal playback on a supported board needs no CirrusAudioFixup arguments. Configure the codec route separately through AppleALC.

Append options to OpenCore `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`, preserving existing arguments. Restart after changing them and remove diagnostic overrides after testing.

Flags with a value of `0` are treated as disabled. Debug builds enable routine logging by default; `-cirrusdbg=0` does not override that build default.

| Argument | Actual behavior |
| --- | --- |
| `-cirrusoff` | Reject attachment before hardware initialization |
| `-cirrusirq` | Experimental reference-board GPIO84 protection IRQ, with polling backup; disabled by default until target delivery and sleep/wake validation |
| `-cirrusdbg` | Enable routine `IOLog` messages in Release; already enabled by default in Debug |
| `-cirrusro` | Force read-only probing: skip GPIO reset, initialization and register writes; normal boot selects SPI or I2C automatically from ACPI `_CRS` |
| `-cirrusnodsp` | Skip firmware and coefficient upload; use the explicit DAC bypass path without DSP acoustic protection |
| `-cirrusphase=<name>` | Stop after a selected initialization stage; playback stays disabled |
| `-cirruslegacyprobe` | Permit read-only fixed-address discovery outside automatic profile policy; never invent endpoints or authorize initialization |
| `-cirrusdelay=<ms>` | Set the read-only snapshot delay, default 100 ms. Does not replace the normal 15 ms startup wait |
| `-cirrusdiag` | Enable additional initialization register snapshots and CRC checks; essential runtime checks remain active without it |
| `-cirruscompact` | Shorten register snapshot text by omitting register names; the readable-register set is unchanged |
| `-cirrusdumptrace` | Publish `Cirrus_Trace_Dump` during the read-only snapshot. Failure trace properties are captured separately |
| `-cirrusnocal` | Skip measured calibration injection. Firmware and channel tuning still upload in DSP mode |
| `-cirrusssid=<value>` | Override the detected subsystem ID for resource selection; does not verify speaker compatibility |
| `-cirrusspkid=<value>` | Verified physical speaker ID (0–255); overrides the board binding. Unknown identity never defaults to speaker 1 |
| `-cirrusspkidl=<value>` | Verified left speaker ID; takes precedence over the common speaker override |
| `-cirrusspkidr=<value>` | Verified right speaker ID; takes precedence over the common speaker override |
| `-cirruscalr0=<value>` | Fallback measured R0 value for either channel when no EFI calibration is available |
| `-cirruscalr0l=<value>` | Left-channel measured R0 override; takes precedence over the shared value |
| `-cirruscalr0r=<value>` | Right-channel measured R0 override; takes precedence over the shared value |
| `-cirruscalstatus=<value>` | Calibration status for boot-argument calibration, default 1 |
| `-cirruscalambient=<value>` | Required measured ambient value when supplying R0 through boot arguments |

Valid phases are `probe`, `otp`, `errata`, `clock`, `asp`, `gpio`, `platform`, `firmware` and `dsp`. Unknown names, including legacy numeric phase names, reject startup before reset. The `firmware` phase stops before DSP boot; the `dsp` phase halts the core after boot checks.

Calibration values must come from the hardware's calibration procedure and use the firmware's units. Do not copy an impedance value from another laptop. Missing measured calibration is reported as `NOT_AVAILABLE`; `READY_DSP` does not certify measured calibration or validate the acoustic tuning.

## Usage

Release logging: `-cirrusdbg`.

Read-only inspection: `-cirrusdbg -cirrusro`.

Stage stop on a supported board: `-cirrusdbg -cirrusphase=otp`. Stage stops intentionally prevent playback. DSP bypass is a bring-up option, not a sound-quality or volume setting.

## Identity and calibration

SSID overrides select embedded resources; they do not authorize wiring or import firmware. Speaker overrides accept IDs 0–255; channel-specific values take precedence over the common value. Generic tuning is separate from physical ID 0.

Boot calibration needs a nonzero uint16 R0 and an explicit ambient value in the firmware's units. Status defaults to 1 and must fit in one byte. EFI calibration has priority over the boot fallback. A nominal impedance copied from another laptop is not measured calibration.

The optional IRQ path is limited to the legacy reference GPIO interrupt; it does not enable arbitrary ACPI interrupts. Polling remains the default.

## Removed argument

`-cirrusprobe` was removed in 1.1.0. It only recorded a flag in IORegistry and never enabled a separate probe mode. Use `-cirrusro` for read-only inspection and `-cirrusdbg` for routine logs.

[Back to installation](../README.md#installation)
