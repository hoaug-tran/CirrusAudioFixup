# Attachment and support matrix — 2026-10-12

CPU vendor is metadata, not authorization or communication selection. A usable
path requires a matched provider, an implemented controller contract, a registered
AMP with a matching read-only identity, verified board/power/tuning data and valid
firmware/host audio routing. An AMP being supported does not implement every
controller on that CPU family.

| Machine / bus | Framework path exercised | Physical support limit |
| --- | --- | --- |
| Intel / I2C | Provider-backed `I2CController`, module RegisterMap, AMP runtime | Custom addressed VoodooI2C ABI or a verified selected factory; native DesignWare is not implied |
| AMD / I2C | Same provider/controller and AMP contracts | Custom addressed ABI or verified selected factory; native MP2 is not implied |
| Intel / SPI | ACPI resources, PCI controller owner, module framing, AMP runtime | Verified Intel LPSS IDs/resources and board data; selected automatically from `_CRS` |
| AMD / SPI | ACPI resources, AMD owner/D0 gate, module framing, AMP runtime | Verified AMD controller HID/IP/resources and board data; selected automatically from `_CRS` |

The host matrix exercises all four rows with registered synthetic AMP hooks and
real production discovery/binding/runtime code. Complete CS35L41 production
bring-up and PM also execute against both I2C and SPI RegisterIO endpoints.
Controller-engine, framing, firmware, calibration and playback fault sweeps remain
separate maintained suites. These are software evidence, not four new physically
validated laptop/controller combinations. Shipping matching and authorization
allowlists were not widened.

## Unsupported combinations

| Evidence | Result |
| --- | --- |
| Known AMP, provider has no addressed I2C contract | Reject before board reset or AMP initialization; `I2C_PROVIDER_ABI_UNAVAILABLE` |
| Known AMP, nub rejects the custom platform function | Read-only preflight returns the native error; `I2C_ADDRESSED_FUNCTION_UNSUPPORTED`; no board reset |
| Known AMP, native SPI owner rejects controller identity/resources | `COMMUNICATION` failure with module reason/native return; no AMP transfers |
| SPI controller works, AMP definition is absent | Controller-only inventory, no guessed register format/maps, no AMP transfers; `NO_REGISTERED_DRIVER` |
| Communication returns a readable unexpected silicon ID | `AMPLIFIER` / `AMP_IDENTITY_MISMATCH`; no AMP initialization |
| Direct ACPI node declares I2C without a selected provider/controller contract | `NATIVE_I2C_PROVIDER_CONTRACT_UNAVAILABLE`; it is not treated as SPI |
| Mixed I2C/SPI, malformed or unsupported ACPI serial resources | `RESOURCE` rejection before controller/AMP traffic |
| Board/firmware/topology is unverified or mismatched | Policy/binding rejection; registration alone cannot enable output |

I2C preflight reads a known non-destructive identity before an authorized board
reset. Unsupported private ABI and a readable wrong silicon prevent that reset.
Other initial read failures may represent a device still held in reset; the
existing verified board reset policy is retained, followed by normal identity
checks. No CPU-wide GPIO reset or default firmware identity is invented.

Unsupported or unbound diagnostic-only attachments do not acquire HDA mappings
or rearm background polling. Controller-only SPI inventory cannot be converted
to writable register access without an AMP format. Existing controller PM and
release ordering remain active, with maps/buses destroyed before owners/targets.

## Debug and machine information

The essential startup record is `ATTACH cpu=... brand=... provider=... class=...
path=...`. Provider name/class/path, bounded CPUID vendor/brand and firmware-reported
machine model are published in `Cirrus_Provider_*`, `Cirrus_CPU_*` and
`Cirrus_Machine_Model`. A reported SMBIOS/model string is not proof of physical
board wiring. `Cirrus_Board_Authorized` records board policy separately from the
legacy writable runtime authorization property.

`Cirrus_Communication_Status` and `Cirrus_Amplifier_Status` describe independent
layers. `CREATED` means a controller object exists, not successful wire traffic.
`REGISTERED_NOT_PROBED` means an AMP module is available, not confirmed silicon.
`READY`, `READ_ONLY`, `PARTIAL`, `IDENTITY_MISMATCH`, `UNSUPPORTED`, `FAULTED`,
`WAITING_RESOURCES` and runtime states preserve those distinctions. Use the driver
verdict and chip diagnostics for DSP/playback readiness; none proves audible
output.

`ATTACH_FAIL` includes stage, reason, endpoint, native return, profile, board SSID
and both layer states. `Cirrus_Attach_First_*` remains immutable; latest fields and
per-index attachment fields retain later failures. Index kinds distinguish endpoint/resource indices, AMP slots and controller
slots; PM errors cannot silently look like a different speaker. Repeated identical errors
publish/log every 32 occurrences, while changed errors publish immediately; the
counter saturates safely. Chip `DIAG_FAIL` and the flight recorder retain their
existing first/latest and precious-register policies.

`Cirrus_Debug_Enabled` records verbosity. `Cirrus_Debug_Phase` records the bounded
requested phase. Only the matched AMP definition accepts phase names; null or
unknown HIDs and invalid phases reject without dereferencing null pointers.
Identity/preflight transfers are captured as probe-source flight-recorder entries;
SPI also retains raw IDs before applying the module identity mask.
Provider property enumeration happens only in verbose mode. Read-only/debug
phases retain the existing AMP-write and initialization boundaries.

Failed `start()` can leave no attached service node. Inspect the essential kernel
startup/failure log as well as IORegistry; absence of an AMP node is not proof that
the AMP itself is unsupported. See [diagnostics](diagnostics.md).

## Live baseline observed without changes

This machine reports `AuthenticAMD`, Ryzen 9 5900HX, the Lenovo CLSA0100 profile,
PCI subsystem `17AA382B`, and `READY_DSP` with left/right firmware status `READY`.
The loaded CirrusAudioFixup UUID is `2564C407-4ED3-379A-8693-8C529B52434D`; VoodooI2C
2.9.1 has UUID `379C7711-B1CA-3D28-B9D5-A4E5A311894D`. This is an older loaded bundle,
not the new attachment fixes. No kext was installed/reloaded and no hardware reset,
playback stress or reboot was performed for this audit. New-build cold boot,
playback, headphone switching and sleep/wake still require target validation.


The maintained suite now includes `check_attachment.py` for bounded CPU metadata,
null/unknown debug phases, first/latest native failures, typed indices, rate limits
and saturated counters. The matrix tests compile real log arguments so a disabled
mock logger cannot hide malformed format strings or missing variables. Controller
PM publication is exercised through the production service wrappers.


Final checks: 28 maintained suites plus the optional Windows audit pass, including
normal and sanitizer matrix/ownership tests. Debug and Release, pinned formatter,
plist/metadata, OpenCore vtables, x86_64 and binary/dSYM UUID gates pass. The SDK's
existing kmod deployment-floor linker warnings remain unchanged.

| Build | UUID | Executable SHA-256 |
| --- | --- | --- |
| Debug | `2AC040FD-DC11-3A9B-ADFF-A8F406E98179` | `42ae4a3cc1640adabbd0857cc2e1f765bd8a76fb0aa9a706e82c94e9d06f21c3` |
| Release | `B4DF25A4-940B-3D06-8BEB-84426A8C7A78` | `5ce466def34b6a6022c3953879f96bd2b2e40e1730bc735be64c442359eb7cb2` |
