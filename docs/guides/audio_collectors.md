# Collect audio and amplifier data

The general Windows and Linux collectors are intended for new-board investigation, driver comparisons and issue reports. They gather available evidence without installing drivers or changing playback settings. Missing data is recorded, not treated as proof that a device is absent.

## Windows

Run Windows PowerShell 5.1 or newer as Administrator, from the repository folder:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Tools\Collectors\Collect-AudioAMP-Windows.ps1
```

`-ExecutionPolicy Bypass` applies to this PowerShell process; the command does not change the machine's persistent execution policy.

The default output is a new `AUDIO_AMP_WINDOWS_*` directory and ZIP in the running account's Documents folder. The tool prints both paths.

| Option | Purpose |
| --- | --- |
| `-OutputRoot <folder>` | Set the parent output folder |
| `-EventDays <days>` | Recent event window, 1–30 days; default 3 |
| `-MaxMB <number>` | Data budget, 8–128 MiB; default 24 |
| `-IncludeDriverFiles` | Copy selected DriverStore payloads within the file/data limits |
| `-NoZip` | Keep the directory without creating a ZIP |
| `-SpeakerTestNotes <text>` | Include the supplied listening/test notes |

For matching OEM driver payloads:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Tools\Collectors\Collect-AudioAMP-Windows.ps1 -IncludeDriverFiles
```

The collector gathers selected audio/AMP PnP devices and parent relationships, installed INF files, candidate extensions, selected registry metadata, related services and recent events. It attempts to read the single named `CirrusSmartAmpCalibrationData` EFI variable when relevant; it does not write firmware variables. Driver binaries are opt-in and limited to selected packages and allowed file types.

Extension candidates do not establish that an OEM effect was active. The collector does not capture a complete Windows DSP-memory image, ACPI table dump or all dynamic amplifier registers. Some PnP resource queries also depend on the installed Windows version.

## Linux

Python 3 is required. Run from the repository folder:

```bash
sudo bash Tools/Collectors/collect_audio_amp_linux.sh
```

The default output is a new `AUDIO_AMP_LINUX_*` directory and `.tar.gz` in the invoking user's home directory. When run through sudo, the tool attempts to return ownership to that user.

| Option | Purpose |
| --- | --- |
| `--output <folder>` | Use an existing parent output folder |
| `--max-mb <number>` | Data budget, 8–128 MiB; default 24 |
| `--include-firmware` | Copy matching firmware payloads within the file/data limits |
| `--no-acpi` | Skip raw DSDT/SSDT collection |
| `--trace-seconds <seconds>` | Optional trace, 0–15 seconds; default 0 |

For matching firmware files:

```bash
sudo bash Tools/Collectors/collect_audio_amp_linux.sh --include-firmware
```

The collector gathers ALSA devices and codec dumps, candidate audio/AMP devices, their bus/controller paths, driver metadata, available ACPI DSDT/SSDT tables, selected regmap snapshots, filtered audio kernel logs and a firmware-name inventory. Root permissions and the kernel configuration determine which sources are available. It does not mount debugfs or probe/rebind the devices.

Regmap snapshots read the driver's exposed register interface. Tracing is off by default. If requested, the tool creates a private ftrace instance, changes its tracing controls, captures supported events and attempts cleanup. This is an optional diagnostic operation, not part of normal collection.

## Read the result

- `status.json` records errors, truncation, skipped data and unavailable sources.
- `SHA256SUMS` identifies the files included in the directory.
- The archive may contain machine/device identifiers, calibration or proprietary driver/firmware files. Review it before sharing.

The limits apply to collected data; they are not a promise that every relevant file fits or that the final directory/archive has an exact maximum size. A missing file or empty log needs explanation from the status records and machine state.

For macOS, use `sudo bash Tools/Collectors/collect_macos.sh`; see [macOS diagnostics](../diagnostics/diagnostics.md).

## Full Windows driver packages

For complete selected DriverStore packages, run PowerShell as administrator:

```powershell
.\Tools\Collectors\Collect-AudioDriverPackages.ps1 -PlanOnly -NoZip
.\Tools\Collectors\Collect-AudioDriverPackages.ps1
```

The default payload budget is 512 MiB. `-MaxPayloadMB` changes it; `-OutputRoot` changes the destination. The default output is under `%PUBLIC%\Documents`, with a ZIP beside the directory. An over-budget selection records its plan without copying payload and exits with code 2. Keep `payload-plan.csv` with the deduplicated `payload/` files; it maps original paths to SHA-256 blobs. Check `status.csv` for missing data before sharing the ZIP.
