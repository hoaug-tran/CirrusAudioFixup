# Extensible amplifier driver foundation

The goal is to add a silicon backend, controller backend and verified board data independently. A CPU vendor, an ACPI HID, a successful build or a register-format definition does not authorize amplifier power writes or establish playback support.

## Implemented layers

| Layer | Implementation | Responsibility |
| --- | --- | --- |
| Bus identity | `Core/Endpoint.hpp` | Controller source path plus I2C address or SPI CS/clock/mode; no fabricated I2C identity for SPI |
| Controller transactions | `Transport/I2CController.hpp`, `SPIController.hpp` | Combined repeated-START I2C or single-CS SPI command/exchange; bounded controller limits |
| Bus adapters | `VoodooI2CController.hpp`, `I2CRegisterBus`, `SPIRegisterBus.hpp` | Borrow controller and endpoint; preserve underlying errors |
| Register map | `Core/RegisterMap.hpp` | 8/16/24/32-bit addresses/values, independent endian/stride, padding and flags, range checks, chunking, access policy, optional operation lock |
| Board manifest | `Platform/BoardConfiguration.hpp` | Exact subsystem/HID identity, endpoints, channels, speaker IDs and tuning resources; reject duplicate endpoints and unverified wiring/power/tuning |
| Firmware identity | `Firmware/FirmwareBundle.hpp` | Borrow immutable assets and match silicon/board/speaker/channel/resource; parsing/integrity/calibration remain backend responsibilities |
| Lifecycle | `Core/AmplifierLifecycle.hpp` | Identify → authorize → initialize → firmware → verify → ready/playback; bounded backend hooks, suspend/reinitialize on wake and first-failure evidence |
| Multi-amp coordination | `Core/AmplifierGroup.hpp` | Stop/cleanup healthy peers on failure; reinitialize all members before restarting playback after wake |
| Owned runtime | `Platform/AmplifierRuntime.hpp` | Own factory-created backends and lifecycle instances; validate the entire topology, coordinate mixed buses/models and retain teardown evidence |
| Backend registry | `Platform/BackendRegistry.hpp` | Statically linked backend factory/destroy callbacks, transport formats, duplicate/capacity checks and exact board/firmware selection |

`CS35L41SPITransport` now configures the shared register-map implementation rather than maintaining separate framing/chunking/RMW code. Intel and AMD SPI tests exercise that production implementation. The existing custom VoodooI2C addressed ABI is adapted to `I2CController` and tested through the same register map. `AudioDevice` and `CS35L41Device` expose transport-neutral endpoints while retaining the legacy I2C address accessor for existing callers; SPI returns the invalid I2C sentinel 0xff.

## Register-map semantics

Scalar values use host byte order. Bulk buffers retain device transfer order. Address progression is `(payload offset / value width) * register stride`, so register width, value width and stride need not match. Validate the whole requested range/access policy before the first transfer. Chunk limits account for both controller constraints and fixed 262-byte stack frames. `RegisterIO::lastError()` exposes a native error for implemented transports (legacy implementations use an unspecified failure sentinel); capture it before cleanup changes the last result. Errors may leave earlier bulk chunks transferred; no retry/rollback is implied.

Writes default to disabled. `readable`/`writable` callbacks let a backend enforce silicon register tables. `precious` prevents diagnostic reads and read-modify-write of clear-on-read or otherwise destructive registers. Explicit `noIncrementRead`/`noIncrementWrite` require separate permission callbacks for FIFO ports. There is no generic cache: replaying power, mailbox, W1C or reset registers after sleep can corrupt device state.

Pass a `RegisterOperationLock` when the owner does not already serialize calls on a command gate. RMW holds this lock across both read and write. Polling releases the lock between attempts and sleeps; attempts are bounded, with bounded controller transfer latency additional. The owner must keep bus/controller/lock alive, block new calls during power transitions and join callers before teardown. Controller frame locking alone does not serialize register RMW.

Non-byte-aligned packed protocols, bank/page selection, register windows and addresses/values wider than 32 bits require a custom `RegisterIO` backend or an explicit extension. Do not approximate them with a compatible-looking config.

## Adding a future backend

1. Establish ACPI identity, actual silicon/revision reads, I2C/SPI format and register access rules from authoritative source/datasheet plus machine evidence. Unknown identifiers remain unsupported.
2. Provide a controller backend only when its layout, capabilities, ownership, CS/repeated-START behavior and power resources are known. Existing Intel LPSS and AMD command controllers have separate implementations. Generic interfaces do not implement unknown controllers.
3. Implement `AmplifierLifecycleHooks` against `RegisterIO` and board/platform resource providers. Keep reset, boost, clock, DSP packing, mailbox, calibration, ASP routing, faults and shutdown specific to silicon/board. A no-DSP backend explicitly treats firmware loading as a no-op.
4. Supply a static `BackendDefinition`, factory/destroy pair and formats with explicit readable/writable policies; select immutable assets with `FirmwareBundle`. Factories use kernel allocation helpers. A registry definition is not automatic IOService discovery or permission to bypass identity checks.
5. Add a verified `BoardConfiguration`; resolve its source paths to owned controller instances from actual ACPI/IORegistry data. Controller ownership, GPIO/reset, regulators, clock and interrupt callback providers belong to the host IOService. Do not reuse another board's GPIO offsets or calibration.
6. Test format/endian/stride/chunk limits, missing resources, access policy, firmware identity, lifecycle failures, shutdown failures and sleep/wake. Then validate actual transfer, routing, both speaker channels, idle, faults and physical playback before advertising support.

## Lifecycle failure behavior

Identify and authorization failures perform no hardware shutdown writes. Once initialization may have touched hardware, failures attempt one backend-provided safe shutdown. A failed shutdown is not retried blindly; cleanup error and original failure are preserved. Faulted instances cannot automatically restart. The owner can call `powerLost()` after confirmed power removal/reset to clear local hardware state while retaining failure evidence. Resume repeats identity, authorization, initialization and firmware checks instead of replaying stale cached registers.

`AmplifierGroup` coordinates up to 16 borrowed lifecycles. Startup failure rolls back previously prepared members; playback failure stops/shuts down healthy peers. Suspend stops all playback before powering down members. Resume reinitializes every member before restarting group playback. Original group failure and individual cleanup failures remain available. Fault recovery requires owner-confirmed power removal/reset.

## Shipping service integration

The IOService owns a persistent `AmplifierRuntime`, selects definitions from
`Devices/BackendCatalog.hpp` and dispatches bring-up, firmware, grouped playback,
interrupt handling, diagnostics and suspend/resume through backend hooks.
`Devices/CS35L41/ServiceBackend.hpp` declares the implemented production factory;
its concrete backend delegates the existing CS35L41 silicon sequences to owner
helpers. CS35L41 register/firmware logic remains specific to that backend.
There is no common model-switch dispatch and no placeholder AMP support.

I2C uses an owned VoodooI2C controller adapter and one backend-formatted
`RegisterMap` per amplifier. SPI borrows maps from the native Intel/AMD host.
Verified manifests match actual subsystem/HID and endpoint identity before
writable binding. `-cirrusro` preserves read-only maps, including on verified
boards. Legacy Lenovo authorization continues to gate its existing reset and
firmware selection. No shipped writable ASUS/HP profile is inferred.

A profile can supply exact immutable `firmwareBundles`. A backend may explicitly
opt into `FirmwareFormat::Deferred` for its existing identity-aware discovery
and parser. Deferred describes a selection plan, not verified program bytes or
permission to bypass DSP checks. Firmware identity waits and debug stops retain
muted peers and prevent group playback without recording a hardware failure.

The shipping service discovers one provider topology per instance. The generic
runtime accepts mixed buses/models/HIDs when its owner supplies distinct ports;
aggregation of independent SPI and I2C provider services is not implemented.

## Source comparison and validation

Linux [regmap configuration](https://github.com/torvalds/linux/blob/v6.12/include/linux/regmap.h) motivates independent address/value width, stride, flags, access tables and precious/no-increment registers. [regmap I2C](https://github.com/torvalds/linux/blob/v6.12/drivers/base/regmap/regmap-i2c.c) motivates preserving a combined address/write/read transaction. [CS35L41 HDA](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda.c) keeps bus framing separate from board properties, DSP firmware/calibration, playback and power sequencing. These are reference facts and architecture, not copied Linux kernel implementation or a Linux ABI exposed on macOS.

`Tests/check_extensible_core.py` exercises production core/manifest/registry code normally and under ASan/UBSan. `check_transport.py` exercises the actual Voodoo adapter and shared register map. SPI framing, Intel/AMD engines and native owners retain their regression suites. Maintained suite: `python3 Tests/run.py --audit`; compile Debug/Release with the repository build checks. Host checks do not validate physical hardware.

### Playback and power transitions

Asynchronous backends implement preparation, pending-transition, observed-state,
validation and cancellation hooks. The common monitor prepares every member
before committing group playback, checks route generation after commit and
cancels participants if the host route changes. CS35L41 also rechecks the host
stream before each member commit. A stale route is a cancelled start, not a
silicon failure. Synchronous backends use lifecycle start/stop; the runtime stops
them when audio disappears.

The shipping runtime disables automatic playback replay on wake. It shuts down
amplifiers before controllers suspend, restores controllers before cold amplifier
bring-up and waits for a fresh valid HDA route before playback. Backend IRQ
configuration is optional; unsupported wiring/hooks retain polling. GPIO/reset,
regulator and other platform resources use owner-provided `platformContext`;
the owner must keep these borrowed resources alive through backend teardown.

### Owned runtime and mixed amplifier boards

`AmplifierRuntime` validates the complete board topology before any factory runs,
then owns a copy of each context, its selected backend and its lifecycle. It
supports up to 16 members with different silicon IDs, device HIDs, register
formats and I2C/SPI endpoints. Each endpoint needs a distinct `RegisterIO` port;
aliasing one port across devices is rejected. The board may use subsystem
identity with per-amplifier HIDs instead of assuming one HID for every amplifier.
Firmware identity and verified wiring/power/tuning remain mandatory.

Factory failure releases all constructed objects without hardware writes.
Bring-up, playback, suspend and resume use the shared group coordinator.
Teardown captures per-member failure and cleanup evidence before destroying
backends through their matching destroy callbacks. Context metadata is owned;
register ports, registry definitions, board/firmware data and strings are borrowed
and must outlive the runtime. The host serializes calls, joins users before
teardown and restores controller power before resume.

Foundation development is independent of machine enablement. The reported
[ASUS G614JV](https://github.com/hoaug-tran/CirrusAudioFixup/issues/2) and
[HP Fury G9](https://github.com/hoaug-tran/CirrusAudioFixup/issues/3) are separate
board integrations. Neither supplies a universal register map, power sequence or
tuning profile. Native Intel PCI discovery also accepts an exact canonical
`acpi-path` when no `acpi-device` pointer is published, rejecting ambiguous matches.
The generic runtime and native SPI owner both support up to 16 endpoints. These capacities do not imply support
for unknown controller layouts or unimplemented amplifier backends.

Host regressions cover mixed buses, models and HIDs, temporary context lifetime,
duplicate endpoint/port rejection, factory rollback, deferred firmware, group
failure cleanup, destructor cleanup and retained shutdown evidence under
ASan/UBSan. Hardware playback validation remains a separate integration step.


### Native SPI backend execution

`Platform/NativeSPIAmplifierRuntime.hpp` joins the native controller owner to the
registered amplifier runtime. It reads the provider HID, resolves all ACPI SPI
resources and validates the entire verified board topology before replacing
read-only discovery maps with backend-specific writable maps. Resource order
need not match manifest order. Every endpoint uses its own silicon format and
access policy; unknown backends, mismatched mode/clock, firmware identities and
unverified board resources prevent successful binding. No amplifier writes occur
in bind or factories.

Bring-up executes the registered backend's read-only identity/authorization,
initialization, firmware and readiness hooks. Group playback, failure cleanup and
shutdown use those same ports. Suspend shuts down amplifiers before suspending
controllers; failed cleanup leaves controllers accessible for diagnostics. Resume
restores all controllers before any amplifier transaction or playback restart.
A failed controller resume leaves the amplifier runtime suspended for an explicit
owner retry. Backends are destroyed before their borrowed maps/controllers.

The host test drives this production path through Intel and AMD controller
fixtures, reversed resource ordering, different silicon formats, firmware writes,
playback, wake failure/retry, unknown IDs, rejected firmware/factories, shutdown
failure and destructor cleanup. Both ordinary and ASan/UBSan executions run.
These fixtures are not registered hardware support. The shipping service uses
its owned `AmplifierRuntime` with native SPI ports and the production CS35L41
factory; the standalone executor is also available to other owners. GPIO/reset/
regulator/IRQ/clock providers and firmware parsing/calibration remain concrete
backend/board responsibilities. Optional platform contexts follow manifest order,
even when ACPI resource order differs.


`notifyPowerLost()` is available only after the owner confirms physical amplifier
reset/power removal. It clears lifecycle hardware state, quiesces controllers and
requires full identity/authorization/firmware bring-up on the next successful
resume. Playback remains stopped. This is not a substitute for a GPIO/regulator
reset and does not erase recorded failure evidence.

## Failure reporting and fallbacks

The owner logs unsupported silicon, unmatched endpoints, missing SPI opt-in and
blocked board/topology initialization. SPI bind errors retain a reason, native
IOReturn, resource/controller index, copied ACPI source and parser/power detail
through rollback and shutdown. Intel controller rejection includes the actual
PCI vendor/device pair in `detail` (vendor in the upper 16 bits). A new
bind/resume resets that operation's failure
record. The standalone SPI executor exposes `transportFailure()` to its owner.

IORegistry exposes `Cirrus_SPI_Failure_Reason`, `_Index`, `_Source`, `_Detail`,
`Cirrus_SPI_Bind_Return` and `Cirrus_SPI_Wake_Return`. Runtime factory/topology
rejections expose `Cirrus_Runtime_Bind_Reason`, `_Index` and `_Return`.
Per-member lifecycle first-failure stage and cleanup return remain separate;
configured I2C probes publish `Cirrus_I2C_<index>_Probe_Return` and `_DeviceID`.
These properties are evidence of the attempted operation, not playback proof.

IRQ falls back to polling only when that polling path is available. Reasons
include opt-in disabled, read-only/unready state, absent verified wiring, missing
ACPI resources, GPIO mismatch and backend/source setup failure. Inspect
`Cirrus_Amp_IRQ_Fallback` and `Cirrus_Amp_IRQ_Mode`. A latched IRQ fault retains
`FAULT_LATCHED_IRQ_DISABLED`; a spurious level retains the unexplained-IRQ mode.
Controller resume failure keeps amplifier initialization blocked. Unknown boards
remain read-only; unsupported controllers/AMPs do not fall back to another chip's
register protocol or tuning. Failed shutdown is never blindly retried.

Release keeps error logs enabled. `-cirrusdbg` enables routine context logs;
bounded register trace and explicit diagnostic options supply detailed transfer
history without printing every periodic poll. Diagnostic register reads honor
precious-register policy. Keep the first transfer error even when a diagnostic
read or cleanup produces another error. These host-tested contracts do not
establish controller ownership or physical speaker behavior on untested hardware.
