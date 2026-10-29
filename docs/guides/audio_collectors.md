# Audio & Amplifier Evidence Collectors

This guide describes how to run the automated Windows and Linux data collectors when bringing up a new laptop, comparing factory OEM settings, or reporting an issue.

The collectors gather ACPI tables, codec dumps, bus controllers, amplifier registers, and OEM calibration data without modifying any system settings.

---

## 1. Windows Collector

Run PowerShell 5.1 or newer **as Administrator** from the repository folder:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Tools\Collectors\Collect-AudioAMP-Windows.ps1
```

The output folder and ZIP archive will be created in your **Documents** folder (named `AUDIO_AMP_WINDOWS_*`).

### Options

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `-OutputRoot <folder>` | Documents | Custom output directory. |
| `-IncludeDriverFiles` | `false` | Copies matching OEM driver binaries from `DriverStore` (useful for firmware extraction). |
| `-EventDays <days>` | `3` | Number of days of Windows event logs to capture (1–30). |
| `-MaxMB <number>` | `24` | Maximum payload collection budget in MiB (8–128). |
| `-NoZip` | `false` | Keeps the folder without compressing into a `.zip` file. |

To include OEM driver binaries for firmware extraction:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Tools\Collectors\Collect-AudioAMP-Windows.ps1 -IncludeDriverFiles
```

---

## 2. Linux Collector

Boot any live Linux USB distribution with working sound, then run from the repository folder:

```bash
sudo bash Tools/Collectors/collect_audio_amp_linux.sh
```

The output folder and `.tar.gz` archive will be saved in your home directory (named `AUDIO_AMP_LINUX_*`).

### Options

| Flag | Default | Description |
| :--- | :--- | :--- |
| `--output <folder>` | `~` | Custom parent output directory. |
| `--include-firmware` | `false` | Copies matching firmware files from `/lib/firmware/cirrus`. |
| `--no-acpi` | `false` | Skips collecting raw DSDT and SSDT ACPI tables. |
| `--max-mb <number>` | `24` | Maximum collection budget in MiB (8–128). |
| `--trace-seconds <sec>` | `0` | Records live audio regmap activity for the specified duration (0–15s). |

To include matching Linux firmware binaries:

```bash
sudo bash Tools/Collectors/collect_audio_amp_linux.sh --include-firmware
```

---

## 3. What Gets Collected

* **Hardware Devices**: PnP hardware IDs, PCI subsystem IDs, and ACPI device handles (`CSC3551`, `CSC3556`, `CLSA0100`).
* **Bus & Controller Info**: SPI and I2C controller identities, clock frequencies, chip select pins, and GPIO reset lines.
* **Audio Codec Dumps**: ALSA `/proc/asound/card*/codec#*` descriptions and active pin connections.
* **Amplifier Registers**: Regmap dumps when exposed by the operating system kernel.
* **OEM Calibration**: Measured DC resistance (R0) and ambient temperature from the `CirrusSmartAmpCalibrationData` EFI variable.

[Back to main guide](../../README.md#collect-windows-or-linux-data)
