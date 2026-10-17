# Device configuration and driver reuse audit

The AMP module is a silicon driver, not a laptop driver. Matching CS35L41 identity permits reuse of register framing and silicon operations. It does not establish matching power topology, GPIO functions, DSP layout, coefficients, calibration, channel order or codec route.

## Evidence inspected

The supplied `CS35L41_UBUNTU_20261017-211223_8438.zip` has SHA-256 `cb723c4a281a86125d273327a022651ea3a3006535858183d1c87aaf6fa2f83d`. Its inventory contains ACPI tables, Linux runtime state and firmware, but no Windows `.sys`/`.inf` or Linux `.ko` driver binary. The original Windows package is no longer available from the tester. Neither Windows driver decompilation nor exact Ubuntu kernel binary equivalence is claimed.

Thirty raw DSDT/SSDT tables were checked for declared size and ACPI checksum. The DSDT is 330991 bytes, OEM `HPQOEM`, table `89C6`, SHA-256 `ebea02b6af3469f5303133f700c4de222ebe22f3e9c00739fe0a58ed2f299720`. ACPICA iASL 20210930 disassembled that bytecode. It warned about external methods; an all-table namespace pass failed with duplicate names. This is targeted inspection of the speaker device, not successful reconstruction or execution of the complete ACPI namespace. Local transcripts and hashes are retained under `.cache/hardware-review/` and are not shipped.

The disassembled `GSPK` device supplies `_HID=CSC3551`, `_SUB` through firmware `AUID`, two SPI resources and the controller path, clock, mode and selections. `_DSD` supplies `cirrus,dev-index={0,1}`, `cirrus,speaker-position={1,0}`, GPIO1 functions `{0,0}`, GPIO2 functions `{2,2}` and electrical values. The captured boost-type array is already `{0,0}` (internal), so an incorrect boost type is not an observed defect in this particular AML. It also describes shared reset and controller chip-select GPIO references. `_CRS` fills placeholder GPIO pins through `GNUM`; literal zero pins in the AML template are not live GPIO identities.

Captured module metadata identifies `snd-hda-scodec-cs35l41-spi`, kernel `6.8.0-138-generic`, srcversion `6240D1F33456349F3BC622E`. Source comparison uses Linux v6.12's separate I2C/SPI wrappers, shared HDA implementation, property handling and silicon library. That versioned source is a reference, not proof it matches the installed Ubuntu module byte for byte.

## Material differences

| Contract | Retained Lenovo evidence / existing path | Supplied SPI machine evidence |
| --- | --- | --- |
| Communication | VoodooI2C addressed transfers, reference addresses 0x40/0x41 | Intel LPSS SPI; CS0 native, CS1 GPIO selected through ACPI |
| Power | Legacy external boost without VSPK switch | Internal boost; 1000 nH, 4500 mA, 24 uF |
| Channel identity | Reference I2C wiring | Device index 0 Right, index 1 Left; resource order is insufficient |
| PLL / ASP | Retained snapshot: PLL 0x430, rate 0x21, format 0x20200200, word length 0x18 | Same observed values; not a universal CS35L41 machine contract |
| DSP | Older retained Linux table: firmware XM base 0xe6, one algorithm | Firmware 0.43.1, XM base 0x1f4, two algorithms |
| HALO state / heartbeat | 0x02800398 / 0x0280039c in that retained layout | 0x028007d0 / 0x028007d4 |
| Coefficients | Own board/speaker/channel assets | Own 103C89C6 channel assets; no cross-board substitution |
| Host audio | Existing codec route | ALC245 and Intel SOF/HDA routing are separate from AMP register control |

`reproduce_host` parses the actual SPI firmware and both coefficients against its recorded algorithm table. It resolves the different HALO addresses rather than substituting Lenovo addresses. Calibration control checks remain name/type/length/algorithm based. Decoding the WMFW/WMDR container and metadata is not disassembly of proprietary HALO DSP instructions or recovery of their signal-processing algorithm.

## Implementation boundaries

There is no per-laptop C++ header or fixed controller path for the new integration. `Platform/BoardDiscovery.hpp` defines an optional driver discovery contract. `AmplifierDriverRegistration::discoverBoard` lets another AMP interpret its own schema without a model switch in the service. `ACPIPlatformProperties` supplies bounded evaluated integer arrays; the host passes actual discovered endpoints.

The CS35L41 `PlatformConfiguration` resolves device indices to endpoint selections, reads channel/GPIO/power properties, copies paths into owned storage and requires matching generic board resources. The logical binding resource is `speaker-protection`, not a filename. Exact subsystem/speaker selection lives in the module asset catalog; names remain provenance metadata. Exact speaker entries outrank board-generic entries regardless of table order; ambiguous entries fail selection.

`Hardware/PropertyQuirks.hpp` contains corrections for documented firmware defects. The 103C89C6 entry corrects electrical properties only, following upstream. It supplies no controller path, GPIO pin, channel mapping or filename. Another correction is data maintenance, not another laptop driver.

Legacy Lenovo profiles, reset authorization and firmware identity waiting remain in place. Discovery does not reinterpret ACPI external-boost value 1 as the Lenovo no-VSPK-switch topology: those are different circuits.

## Current limits

This is an extensible path, not support for every machine from an OEM. Automatic property resolution currently implements internal boost, GPIO1 unused, GPIO2 unused or interrupt, and at most two unique channels because the asset schema represents L0/R0. Missing/inconsistent ACPI, unsupported power/GPIO modes, absent matching resources or unknown controllers keep the provider read-only. Adding firmware alone does not implement another electrical topology or audio route.

The native SPI provider invokes property discovery. Legacy provider-backed I2C remains supported; native ACPI I2C ownership and aggregation across independent I2C/SPI providers are not implemented. The shared runtime still accepts separately supplied mixed-bus/mixed-model ports. No other AMP model has acquired a production implementation merely because the registry accepts future modules.

Tests cover real ACPI decoding and the service callback, synthetic identities from multiple vendors, reordered resources, independent paths/clocks, malformed properties, missing tuning, exact corrections, driver/PM/fault handling and Lenovo identity isolation. Synthetic fixtures are not OEM hardware support. Native builds and physical load, left/right playback and sleep/wake are separate gates.

## Source references

- [Cirrus HDA SPI wrapper](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda_spi.c) and [I2C wrapper](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda_i2c.c).
- [Shared HDA configuration, firmware and channel handling](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda.c).
- [Subsystem property corrections](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda_property.c).
- [Silicon GPIO, reset and power contracts](https://github.com/torvalds/linux/blob/v6.12/sound/soc/codecs/cs35l41-lib.c).
