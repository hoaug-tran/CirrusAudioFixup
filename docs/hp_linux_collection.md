# HP CS35L41 SPI: Ubuntu evidence

Boot an official Ubuntu Desktop USB directly from the HP firmware boot menu (F9), select Try Ubuntu. Bypassing OpenCore preserves the firmware ACPI tables for comparison. Installation to the internal disk is unnecessary.

Copy `Tools/collect_linux.sh` to the live session. In its directory run:

```sh
sudo apt update
sudo apt install -y python3 acpica-tools alsa-utils pciutils dmidecode kmod
sudo bash collect_linux.sh
```

Send the generated `HP-CS35L41-Linux-*.tar.gz`. Record whether internal speakers and headphones work in Ubuntu, which output is selected, and whether collection occurred before or after playback. If speakers work, play a short local sound normally through the desktop, then collect again; label the two archives. Collection does not itself play audio or change mixer state.

## Collected data

| Source | Contents |
| --- | --- |
| ACPI | Static/dynamic tables and iasl disassembly when available |
| Controllers and devices | PCI resources/IRQ, SPI and CSC3551 sysfs paths, module metadata |
| Audio | HDA codec dumps, ALSA controls/state and kernel logs |
| Machine | BIOS and model identifiers |
| Firmware | Matching `103c89c6`, `103c8ca4` and generic CS35L41 speaker-protection files, inventory and symlink targets |
| Calibration | The named Cirrus EFI calibration variable, if accessible |
| GPIO | Ownership information when debugfs is already accessible |

Both subsystem IDs are collected so their identity sources can be compared.

## Check the archive

Review `collection-status.txt` for missing tools, inaccessible files, driver errors, and ACPI disassembly failures. A missing driver does not prevent ACPI/PCI collection. This is diagnostic evidence, not a complete capture of live amplifier state: the script never reads MMIO or live regmap registers, probes a bus, unbinds drivers, mounts debugfs, or changes power state deliberately. No guarantee is made that all hardware-specific information is exposed by a given Linux kernel. Logs may contain identifying information; DMI serial/UUID fields and ACPI MSDM are omitted.

This HP-specific tool preserves the existing collection format. For a broader device report with a data budget, use the [general Linux collector](audio_collectors.md#linux).

Upstream references: [Ubuntu acpica-tools](https://packages.ubuntu.com/en/acpica-tools), [Intel LPSS PCI binding](https://github.com/torvalds/linux/blob/v6.12/drivers/mfd/intel-lpss-pci.c), [CS35L41 HDA SPI driver](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda_spi.c).
