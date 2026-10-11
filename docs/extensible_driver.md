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

## Current integration boundary

The shipping IOService still performs CS35L41-specific bring-up, firmware/diagnostics and I2C transport selection. The new generic lifecycle/registry/board/firmware contracts are implemented and host-tested, and CS35L41 bring-up now uses the common lifecycle coordinator. An owned generic runtime implements registry selection, persistent lifecycle and group ownership; the shipping IOService has not yet switched its CS35L41 playback path to that runtime. No new amplifier is registered by a placeholder definition. An opt-in read-only SPI personality now performs ACPI discovery and native controller binding. SPI board authorization and complete controller power-parent coordination remain pending. Existing Lenovo authorization remains in place.

This foundation reduces duplicated transport work for new backends. It does not claim that every future amplifier/controller is supported or that all service code has already become generic.

## Source comparison and validation

Linux [regmap configuration](https://github.com/torvalds/linux/blob/v6.12/include/linux/regmap.h) motivates independent address/value width, stride, flags, access tables and precious/no-increment registers. [regmap I2C](https://github.com/torvalds/linux/blob/v6.12/drivers/base/regmap/regmap-i2c.c) motivates preserving a combined address/write/read transaction. [CS35L41 HDA](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/cs35l41_hda.c) keeps bus framing separate from board properties, DSP firmware/calibration, playback and power sequencing. These are reference facts and architecture, not copied Linux kernel implementation or a Linux ABI exposed on macOS.

`Tests/check_extensible_core.py` exercises production core/manifest/registry code normally and under ASan/UBSan. `check_transport.py` exercises the actual Voodoo adapter and shared register map. SPI framing, Intel/AMD engines and native owners retain their regression suites. Maintained suite: `python3 Tests/run.py --audit`; compile Debug/Release with the repository build checks. Host checks do not validate physical hardware.

### Runtime migration: CS35L41 bring-up

The shipping `initializeCS35L41` now executes its hardware, firmware and idle
verification callbacks through `AmplifierLifecycle`. Existing silicon checks,
board gates, phase diagnostics and DSP-bypass behavior remain in the owner.
The synchronous callback adapter borrows stack callbacks and does not create a
persistent backend. Initialization failures use one shared shutdown callback.
Waiting for firmware identity and intentional debug stops leave initialization
incomplete without recording a hardware failure or running failure cleanup.
A subsequent owner retry still performs the existing reset/initialization flow.

This is the first runtime migration step. Playback, interrupts, sleep/wake and
transport/controller ownership still use the existing service implementation.
`BackendRegistry` and `AmplifierGroup` are not yet used for shipping runtime
selection. Native SPI controllers now have opt-in read-only ACPI discovery/resource binding
and service suspend/resume hooks. Physical validation, controller publication/PM
coordination and board data remain necessary before SPI playback can be enabled.

Deferred bring-up propagates through `AmplifierGroup` as Waiting/DebugHalted,
including on wake. Prepared peers remain muted; later members and group playback
are blocked. This does not record a hardware failure or trigger failure cleanup.
The owner must explicitly detach/reset before retrying the group.


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
The generic runtime has 16 slots; the existing native SPI owner separately caps
its ACPI resource set at four endpoints. These capacities do not imply support
for unknown controller layouts or unimplemented amplifier backends.

Host regressions cover mixed buses, models and HIDs, temporary context lifetime,
duplicate endpoint/port rejection, factory rollback, deferred firmware, group
failure cleanup, destructor cleanup and retained shutdown evidence under
ASan/UBSan. Hardware playback validation remains a separate integration step.
