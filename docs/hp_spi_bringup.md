# HP ZBook Fury 16 G9 SPI bring-up

## Evidence and diagnosis

Source baseline: `7c45427`, branch `feat/cs35l41-spi-support`, with existing local changes retained. The supplied `CS35L41_UBUNTU_20261017-211223_8438.zip` is a Linux runtime reference, not a macOS success report.

| Evidence inside the archive | Observation |
| --- | --- |
| `kernel/dmi/product_name`, `board_name` | HP ZBook Fury 16 G9, 89C6 |
| `sysfs/spi_master/spi1/path.txt`, amp `udev.txt` | PCI 00:1e.2, Intel 7AAA, intel-lpss / pxa2xx-spi |
| `acpi/decompiled/dsdt.dsl`, SPI0.GSPK | CSC3551, CS0/CS1, 4 MHz, mode 0; `_SUB` calls AUID |
| SPI0 `_DSD` `cs-gpios` | `{Zero, GSPK, Zero, Zero, Zero}`: native CS0 and GPIO CS1 |
| `gpio/gpio.txt` | INTC1056, GPIO base 512; reset offset 8, CS1 offset 19 |
| `dsp/pinctrl/INTC1056:00/pins` | Pad 19: GPIO output `0x84000201`, configuration locked; pad 8 reset high |
| `kernel/dmesg_full.txt` | Both amps identify as 35a40/B2; shared reset; SSID 103C89C6; amp .0 Right, amp .1 Left |
| `regmap/before/amp{0,1}/registers` | PWR_CTRL2 `0x3021`, boost coefficients `0x4932`, slope `0x7500` |

The previous transport interpreted both DeviceSelection values as native CS. That cannot reproduce the captured GPIO CS1 wiring. Reading a logical CS number from `_CRS` is insufficient without the controller's `cs-gpios` mapping.

The CS35L41 runtime also assumed the existing external boost topology: it forced BST_EN off, expected active PWR_CTRL2 `0x3001` and used external supply transition registers. HP requires internal boost. The captured AML already reports internal boost (value 0). Upstream also documents an electrical-property correction for this subsystem; the correction remains idempotent for this capture: 1000 nH, 4500 mA, 24 uF. Applying the old playback path to this board is not valid.

The audio-controller/codec SSID is a separate identity source from the amplifier `_SUB`. The exact HP profile explicitly selects ACPI `_SUB`; an audio PCI SSID alone cannot authorize it. There is no blanket 103C8CA4 -> 103C89C6 translation.

## Implementation boundaries

- `Transport/ACPIDeviceProperties.hpp` owns `_DSD` data and resolves object, AML and IOACPIPlane reference paths. `ACPIGPIOResources.hpp` decodes evaluated GPIO resources with bounds/index/checksum checks. Unevaluated AML zero pins are never treated as live GPIO numbers.
- `Transport/Intel/IntelSPIChipSelects.hpp` resolves GPIO selections and owns exclusive GPIO provider opens/maps. `IntelGPIOOutput.hpp` contains the INTC1056/INTC1085 family mapping, resource indices and register constraints. It changes only TX state on an existing host-owned GPIO output; it does not unlock or remux pads. Other GPIO controller families fail binding. GPIO ownership is currently exclusive per SPI owner; sharing a GPIO provider across independent consumers needs a separate pin-arbitration implementation.
- `LPSSSPIEngine` borrows a generic `SPIChipSelect` for external selections. Native CS stays inactive throughout GPIO frames. Selection and transfer run under the same controller lock. Assertion failures, frame failures and release failures propagate; a failed release takes the engine offline. Known inaccessible SPI MMIO is not written during cleanup.
- `Platform/BoardDiscovery.hpp` defines optional driver-owned property discovery. `Devices/Amplifiers/CS35L41/PlatformConfiguration.hpp` derives channels, GPIO functions and power from evaluated `_DSD` and endpoints from `_CRS`, owning copies of the paths. No per-laptop header or fixed SPI path is used. `Hardware/PropertyQuirks.hpp` retains the documented subsystem electrical correction. See [configuration and binary evidence audit](platform_evidence_audit.md).
- `AmplifierBinding` carries an immutable driver-owned configuration view. The CS35L41 factory checks its schema, size and electrical limits before creating a writable runtime. `PowerConfiguration.hpp` implements the silicon-specific internal boost coefficients and readback, with GLOBAL_EN required off before programming.
- Muted preparation, group commit, periodic checks, synchronous cleanup, asynchronous shutdown and wake use the selected power topology. The existing external path remains the default for legacy bindings.
- The shared reset GPIO is not pulsed per amplifier. This path requires firmware to leave reset released, proves access through both identity probes, and uses individual silicon software resets. A held-low/reset ownership problem must be diagnosed on the target; no HP reset MMIO offset is hardcoded.

## Firmware provenance

The three protection assets below were imported with `Tools/import_firmware.py` from `firmware/original/cirrus/` in the supplied archive. Calibration firmware and other boards' tuning were not substituted. Speaker ID 0 is the board-generic resource key; it is not an invented measured speaker identity.

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `cs35l41-dsp1-spk-prot-103c89c6.wmfw` | 34068 | `c11cb3e25825c673a955a3519370ad77fbcd8bcf68808e8e34140a70caefcec4` |
| `cs35l41-dsp1-spk-prot-103c89c6-l0.bin` | 4000 | `7b6a432787c14a9ba1ec419672c9ff87d0101669ecda37dc14c8e902ed363c42` |
| `cs35l41-dsp1-spk-prot-103c89c6-r0.bin` | 4000 | `2f7e769606a77ef433e5c1ca35bb4e5679b2d01326aefd9fdc11cdfcb151d81d` |

The existing parser/uploader validates WMFW and per-channel coefficients. Existing EFI calibration validation remains in place; no calibration values are fabricated from this register dump. Asset provenance does not establish redistribution rights beyond the original firmware terms.

## Verification and target sequence

New maintained checks: `check_spi_chip_select`, `check_spi_gpio`, `check_spi_gpio_binding`, `check_cs35l41_power`, `check_platform_configuration`, `check_platform_discovery`. They exercise production mapping/ownership and state machines, captured pad/boost values, exact identity selection (including conflicting PCI SSID), channel order, asset SHA-256, invalid inputs, transfer faults and cleanup. Existing tests cover group rollback, firmware/calibration parsing, native controller PM and legacy playback. Sanitizer host checks and target-SDK syntax checks are source evidence only.

The Windows investigation ran the host suite in WSL Ubuntu with GCC and its ASan/UBSan runtimes. Test launchers named `clang++` used a temporary GCC compatibility alias; this is not an Apple Clang execution claim. Separate Windows Clang target-SDK syntax checks used macOS 11, kernel/freestanding mode, no RTTI/exceptions, and both DEBUG=0/1 for all six production translation units.

Build the exact modified tree on macOS with `bash Tools/build_macos.sh`; retain Debug/Release bundles, matching dSYMs and metadata. Build/load evidence must identify the current configuration-discovery revision; earlier CI bundles do not validate this refactor. Earlier bundle UUIDs in project history do not identify this tree.

1. Boot target with `-cirrusdbg` only. Automatic `_CRS` selection must report SPI, controller association/bind, CS map count 2, CS0 native, CS1 GPIO pin from evaluated ACPI, completed reset sequence, and both 35A40/B2 identity reads. Keep fallback EFI available.
2. If full initialization reaches `platform`, confirm ACPI SSID, two correct channels, muted output, boost coefficient readback and no playback publication before platform validation completes. Use `-cirrusphase=platform` only when intentionally stopping at that boundary.
3. Remove the phase halt for firmware/DSP, then playback at low volume. Record physical Right/Left separately, headphones, idle transitions and cold boot. Capture the actual loaded kext identity and HDA/ASP route/clocks.
4. Test sleep/wake with both GPIO/SPI providers accessible before transfers. GPIO mux, locks or power state changes are checked again; a failure keeps playback stopped.

Collect each result with `Tools/collect_macos.sh`. Essential properties include `Cirrus_Platform_Identity_Source`, `Cirrus_Platform_ACPI_SSID`, `Cirrus_Platform_PCI_SSID`, `Cirrus_Intel_SPI_GPIO_Bind_Return`, `Cirrus_Intel_SPI_CS_Map_Count`, per-CS source/pin, SPI bind/identity results and each amplifier's first/latest failure. GPIO D0/ownership/output configuration and physical playback remain target validation gates.

## References

- [Linux v6.12 HP property correction](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda_property.c)
- [Cirrus CS35L41 boost and power contracts](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs35l41-lib.c)
- [Intel Alder Lake GPIO family mapping](https://github.com/torvalds/linux/blob/v6.12/drivers/pinctrl/intel/pinctrl-alderlake.c)
- [Intel GPIO ownership, locks and pad registers](https://github.com/torvalds/linux/blob/v6.12/drivers/pinctrl/intel/pinctrl-intel.c)
- [ACPICA GPIO resource layout](https://github.com/acpica/acpica/blob/master/source/include/amlresrc.h)
