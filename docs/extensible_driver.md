# Extensible amplifier driver foundation

The goal is to add a silicon driver, controller driver and verified board data independently. A CPU vendor, an ACPI HID, a successful build or a register-format definition does not authorize amplifier power writes or establish playback support.

## Current software boundary (2026-10-12)

The independent production AMP ownership migration is complete. CS35L41 now
follows the same module contract intended for a new AMP. The implementation
history below records earlier transitional milestones; their bridge/state TODOs
are superseded by the final ownership section at the end.

## Implemented layers

| Layer | Implementation | Responsibility |
| --- | --- | --- |
| Bus identity | `Core/BusEndpoint.hpp` | Controller source path plus I2C address or SPI CS/clock/mode; no fabricated I2C identity for SPI |
| Controller transactions | `Transport/I2CController.hpp`, `SPIController.hpp` | Combined repeated-START I2C or single-CS SPI command/exchange; bounded controller limits |
| Bus adapters | `VoodooI2CController.hpp`, `I2CRegisterBus`, `SPIRegisterBus.hpp` | Borrow controller and endpoint; preserve underlying errors |
| Register map | `Core/RegisterMap.hpp` | 8/16/24/32-bit addresses/values, independent endian/stride, padding and flags, range checks, chunking, access policy, optional operation lock |
| Board manifest | `Platform/BoardConfiguration.hpp` | Exact subsystem/HID identity, endpoints, channels, speaker IDs and tuning resources; reject duplicate endpoints and unverified wiring/power/tuning |
| Firmware identity | `Firmware/FirmwareBundle.hpp` | Borrow immutable assets and match silicon/board/speaker/channel/resource; parsing/integrity/calibration remain driver responsibilities |
| Lifecycle | `Core/AmplifierLifecycle.hpp` | Identify → authorize → initialize → firmware → verify → ready/playback; bounded driver hooks, suspend/reinitialize on wake and first-failure evidence |
| Multi-amp coordination | `Core/AmplifierGroup.hpp` | Stop/cleanup healthy peers on failure; reinitialize all members before restarting playback after wake |
| Owned runtime | `Platform/AmplifierManager.hpp` | Own factory-created drivers and lifecycle instances; validate the entire topology, coordinate mixed buses/models and retain teardown evidence |
| Driver registry | `Platform/AmplifierDriverRegistry.hpp` | Statically linked driver factory/destroy callbacks, transport formats, duplicate/capacity checks and exact board/firmware selection |

`CS35L41SPIRegisterIO` now configures the shared register-map implementation rather than maintaining separate framing/chunking/RMW code. Intel and AMD SPI tests exercise that production implementation. The existing custom VoodooI2C addressed ABI is adapted to `I2CController` and tested through the same register map. `AmplifierDevice` and `CS35L41Device` expose transport-neutral endpoints while retaining the legacy I2C address accessor for existing callers; SPI returns the invalid I2C sentinel 0xff.

## Register-map semantics

Scalar values use host byte order. Bulk buffers retain device transfer order. Address progression is `(payload offset / value width) * register stride`, so register width, value width and stride need not match. Validate the whole requested range/access policy before the first transfer. Chunk limits account for both controller constraints and fixed 262-byte stack frames. `RegisterIO::lastError()` exposes a native error for implemented transports (legacy implementations use an unspecified failure sentinel); capture it before cleanup changes the last result. Errors may leave earlier bulk chunks transferred; no retry/rollback is implied.

Writes default to disabled. `readable`/`writable` callbacks let a driver enforce silicon register tables. `precious` prevents diagnostic reads and read-modify-write of clear-on-read or otherwise destructive registers. Explicit `noIncrementRead`/`noIncrementWrite` require separate permission callbacks for FIFO ports. There is no generic cache: replaying power, mailbox, W1C or reset registers after sleep can corrupt device state.

Pass a `RegisterOperationLock` when the owner does not already serialize calls on a command gate. RMW holds this lock across both read and write. Polling releases the lock between attempts and sleeps; attempts are bounded, with bounded controller transfer latency additional. The owner must keep bus/controller/lock alive, block new calls during power transitions and join callers before teardown. Controller frame locking alone does not serialize register RMW.

Non-byte-aligned packed protocols, bank/page selection, register windows and addresses/values wider than 32 bits require a custom `RegisterIO` driver or an explicit extension. Do not approximate them with a compatible-looking config.

## Adding a future driver

1. Establish ACPI identity, actual silicon/revision reads, I2C/SPI format and register access rules from authoritative source/datasheet plus machine evidence. Unknown identifiers remain unsupported.
2. Provide a controller driver only when its layout, capabilities, ownership, CS/repeated-START behavior and power resources are known. Existing Intel LPSS and AMD command controllers have separate implementations. Generic interfaces do not implement unknown controllers.
3. Implement `AmplifierLifecycleHooks` against `RegisterIO` and board/platform resource providers. Keep reset, boost, clock, DSP packing, mailbox, calibration, ASP routing, faults and shutdown specific to silicon/board. A no-DSP driver explicitly treats firmware loading as a no-op.
4. Supply a static `AmplifierDriverRegistration`, factory/destroy pair and formats with explicit readable/writable policies; select immutable assets with `FirmwareBundle`. Factories use kernel allocation helpers. A registry definition is not automatic IOService discovery or permission to bypass identity checks.
5. Add a verified `BoardConfiguration`; resolve its source paths to owned controller instances from actual ACPI/IORegistry data. Controller ownership, GPIO/reset, regulators, clock and interrupt callback providers belong to the host IOService. Do not reuse another board's GPIO offsets or calibration.
6. Test format/endian/stride/chunk limits, missing resources, access policy, firmware identity, lifecycle failures, shutdown failures and sleep/wake. Then validate actual transfer, routing, both speaker channels, idle, faults and physical playback before advertising support.

## Lifecycle failure behavior

Identify and authorization failures perform no hardware shutdown writes. Once initialization may have touched hardware, failures attempt one driver-provided safe shutdown. A failed shutdown is not retried blindly; cleanup error and original failure are preserved. Faulted instances cannot automatically restart. The owner can call `powerLost()` after confirmed power removal/reset to clear local hardware state while retaining failure evidence. Resume repeats identity, authorization, initialization and firmware checks instead of replaying stale cached registers.

`AmplifierGroup` coordinates up to 16 borrowed lifecycles. Startup failure rolls back previously prepared members; playback failure stops/shuts down healthy peers. Suspend stops all playback before powering down members. Resume reinitializes every member before restarting group playback. Original group failure and individual cleanup failures remain available. Fault recovery requires owner-confirmed power removal/reset.

## Shipping service integration

The IOService owns a persistent `AmplifierManager`, selects definitions from
`Devices/AmplifierDrivers.hpp` and dispatches bring-up, firmware, grouped playback,
interrupt handling, diagnostics and suspend/resume through driver hooks.
`Devices/Amplifiers/CS35L41/CS35L41DriverFactory.hpp` declares the implemented production factory;
its concrete `CS35L41Driver` owns live state, firmware/calibration, silicon,
protection, monitoring and playback. It uses only common host/register/board/asset
contracts. The service retains generic endpoint metadata and diagnostic publication.
There is no common model-switch dispatch and no placeholder AMP support.

I2C uses an owned VoodooI2C controller adapter and one driver-formatted
`RegisterMap` per amplifier. SPI borrows maps from the native Intel/AMD host.
Verified manifests match actual subsystem/HID and endpoint identity before
writable binding. `-cirrusro` preserves read-only maps, including on verified
boards. Legacy Lenovo authorization continues to gate its existing reset and
firmware selection. No writable profile is inferred from an OEM name. Native SPI can obtain a module-owned configuration from evaluated ACPI properties only when matching resources and supported electrical semantics exist.

A profile can supply exact immutable `firmwareBundles`. A driver may explicitly
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

Asynchronous drivers implement preparation, pending-transition, observed-state,
validation and cancellation hooks. The common monitor prepares every member
before committing group playback, checks route generation after commit and
cancels participants if the host route changes. CS35L41 also rechecks the host
stream before each member commit. A stale route is a cancelled start, not a
silicon failure. Synchronous drivers use lifecycle start/stop; the runtime stops
them when audio disappears.

The shipping runtime disables automatic playback replay on wake. It shuts down
amplifiers before controllers suspend, restores controllers before cold amplifier
bring-up and waits for a fresh valid HDA route before playback. Driver IRQ
configuration is optional; unsupported wiring/hooks retain polling. GPIO/reset,
regulator and other platform resources use owner-provided `platformContext`;
the owner must keep these borrowed resources alive through driver teardown.

### Owned runtime and mixed amplifier boards

`AmplifierManager` validates the complete board topology before any factory runs,
then owns a copy of each context, its selected driver and its lifecycle. It
supports up to 16 members with different silicon IDs, device HIDs, register
formats and I2C/SPI endpoints. Each endpoint needs a distinct `RegisterIO` port;
aliasing one port across devices is rejected. The board may use subsystem
identity with per-amplifier HIDs instead of assuming one HID for every amplifier.
Firmware identity and verified wiring/power/tuning remain mandatory.

Factory failure releases all constructed objects without hardware writes.
Bring-up, playback, suspend and resume use the shared group coordinator.
Teardown captures per-member failure and cleanup evidence before destroying
drivers through their matching destroy callbacks. Context metadata is owned;
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
for unknown controller layouts or unimplemented amplifier drivers.

Host regressions cover mixed buses, models and HIDs, temporary context lifetime,
duplicate endpoint/port rejection, factory rollback, deferred firmware, group
failure cleanup, destructor cleanup and retained shutdown evidence under
ASan/UBSan. Hardware playback validation remains a separate integration step.


### Native SPI driver execution

`Platform/SPIAmplifierManager.hpp` joins the native controller owner to the
registered amplifier runtime. It reads the provider HID, resolves all ACPI SPI
resources and validates the entire verified board topology before replacing
read-only discovery maps with driver-specific writable maps. Resource order
need not match manifest order. Every endpoint uses its own silicon format and
access policy; unknown drivers, mismatched mode/clock, firmware identities and
unverified board resources prevent successful binding. No amplifier writes occur
in bind or factories.

Bring-up executes the registered driver's read-only identity/authorization,
initialization, firmware and readiness hooks. Group playback, failure cleanup and
shutdown use those same ports. Suspend shuts down amplifiers before suspending
controllers; failed cleanup leaves controllers accessible for diagnostics. Resume
restores all controllers before any amplifier transaction or playback restart.
A failed controller resume leaves the amplifier runtime suspended for an explicit
owner retry. Drivers are destroyed before their borrowed maps/controllers.

The host test drives this production path through Intel and AMD controller
fixtures, reversed resource ordering, different silicon formats, firmware writes,
playback, wake failure/retry, unknown IDs, rejected firmware/factories, shutdown
failure and destructor cleanup. Both ordinary and ASan/UBSan executions run.
These fixtures are not registered hardware support. The shipping service uses
its owned `AmplifierManager` with native SPI ports and the production CS35L41
factory; the standalone executor is also available to other owners. GPIO/reset/
regulator/IRQ/clock providers and firmware parsing/calibration remain concrete
driver/board responsibilities. Optional platform contexts follow manifest order,
even when ACPI resource order differs.


`notifyPowerLost()` is available only after the owner confirms physical amplifier
reset/power removal. It clears lifecycle hardware state, quiesces controllers and
requires full identity/authorization/firmware bring-up on the next successful
resume. Playback remains stopped. This is not a substitute for a GPIO/regulator
reset and does not erase recorded failure evidence.

## Failure reporting and fallbacks

The owner logs unsupported silicon, unmatched endpoints, rejected serial-bus resources and
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
ACPI resources, GPIO mismatch and driver/source setup failure. Inspect
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

## CS35L41 state boundary (2026-10-12)

`Devices/Amplifiers/CS35L41/SiliconState.hpp` now defines the live silicon, firmware, protection,
playback-transition and diagnostic state without including IOService, transport
providers or platform bindings. The service endpoint binding inherits this state
and retains endpoint labels, channels and borrowed register ports. Member defaults
and register sequences are preserved. This internal C++ layout is not a provider
ABI; all source consumers must rebuild together.

The host bring-up and runtime fixtures include the production state header rather
than copying its fields. Their existing fault, deferred identity, playback and PM
coverage therefore exercises the separated state. Source-layout gates reject
platform/transport includes and endpoint ownership fields in the chip schema.

All 26 maintained host suites and the optional Windows payload audit pass.
Debug and Release Xcode builds pass metadata, OpenCore vtable, x86_64 and
binary/dSYM UUID checks. The new state header passes a clang-format dry-run
with explicit LLVM/4-space/140-column/left-pointer settings; this checkout has
no pinned formatter config. No bundle was installed or loaded by this change.

This is a schema boundary, not independent driver lifetime ownership. The service
still allocates endpoint state, orchestrates firmware and supplies silicon helper
methods. At that schema-only milestone, the next steps were an explicit serialized
platform contract, driver-owned state allocation and lifetime coverage. The
subsequent contract extraction is recorded below. Native I2C/MP2,
new amplifiers and SoundWire remain subject to the existing evidence gates.

## CS35L41 platform contract and lifetime audit (2026-10-12)

The production CS35L41 driver now includes its own factory/contract header and
compiles without `CirrusAudioFixup.hpp`. It borrows `Platform`, `State` and the
runtime-owned context instead of casting to a private service struct. The service
implements that contract through an embedded adapter; factory friendship and the
nested driver class are removed. This internal source contract does not change
the installed VoodooI2C ABI or establish its provenance.

| Audited boundary | Result and regression |
| --- | --- |
| Construction | Null platform/state, wrong model, endpoint/channel mismatch and invalid binding reject without register I/O or state reset |
| Identity | Failed read captures native `lastError()` before diagnostic callbacks; unknown ID after a successful read remains `NotFound`; unspecified/zero failure uses `NotReady` |
| Context lifetime | Runtime copies temporary context metadata and destroys driver before context, adapter and register ports |
| Teardown/rebind | Service clears borrowed adapter pointers only after runtime unbind; actual bring-up fixture checks cleared views and subsequent live-state rebind |
| PM | Driver fixture exercises playback, suspend, reinitialize on resume and destruction while platform/state/port remain alive |
| Cleanup failure | Initialization failure and separate shutdown failure survive runtime release; failed cleanup is not automatically retried |
| DSP halt/reset | `DSPControl::halt` owns state invalidation and the existing RMW/RMW/1000-us-delay/readback sequence; reset is attempted after halt failure and readback remains conditional |

All calls still require owner serialization. State identity and its address remain
fixed while bound; platform resources stay accessible through shutdown and failure
diagnostics. The contract follows the existing timer/command-gate design and
Apple's [workloop and event-source lifetime guidance](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/HandlingEvents/HandlingEvents.html).
It does not make synchronous firmware upload or provider transactions asynchronous.

`Tests/check_cs35l41_driver.py` compiles the complete production driver as a
separate translation unit against a non-IOService platform. Ordinary and ASan/UBSan
runs cover the boundaries above, including DSP transfer faults and active-core
readback. Existing bring-up/runtime/native-SPI fixtures now use the production
platform adapter. The maintained runner includes the new suite: all 27 host suites
plus the optional Windows payload audit pass. Debug/Release builds, plist/release
metadata, x86_64, OpenCore vtables and binary/dSYM UUID gates pass. The new/changed
chip files pass an explicit clang-format dry-run; no repository-wide formatting
configuration was introduced.

Independent allocation of chip state and migration of firmware, protection,
monitoring and other silicon helpers remain open software work. The bridge still
delegates those operations to the service. Register sequences, board authorization,
firmware assets and matching personalities are unchanged. No new bundle was
installed or loaded; controller/provider provenance, firmware licensing and target
cold boot/playback/headphone/sleep-wake validation remain separate evidence gates.

## Driver-owned lifecycle orchestration (2026-10-12)

Initialization, firmware-stage sequencing, readiness verification and safe
shutdown now execute in `CS35L41DriverFactory.cpp`. The corresponding four service
methods and its chip debug-disposition helper are removed. The driver owns
readiness invalidation, stage progression, debug stops, deferred firmware identity,
bypass policy sequencing and partial-initialization cleanup. The platform contract
has no callbacks that return complete initialize/load/verify/shutdown outcomes.

A chip-specific `Operation` enum temporarily delegates the existing lower silicon
helpers; the service adapter also supplies firmware selection/upload, debug policy,
logging and diagnostic publication. Each operation maps to the same helper as
before. This is an intermediate boundary, not a new generic controller framework
or proof that those helpers are already independent of the service.

All four moved function bodies pass a non-whitespace parity comparison after
mapping their owner/helper calls to the explicit contract. Register sequences,
callback order, error recording, debug-phase gates and cleanup order are preserved.
`Tests/check_bringup.py` executes the complete production driver and production
service adapter, retaining the existing 11-stage faults, firmware delay, bypass,
debug stops, cleanup failure and wake coverage. The standalone driver suite adds
nine operation failure cases under ASan/UBSan, checks first versus cleanup failure
and verifies that teardown does not retry an already-faulted cleanup.

Runtime fault positions continue to measure playback transactions. Its fixture
uses an explicit simulated boot wrapper, as it previously used mocked whole boot
methods, and delegates all playback/status/diagnostic/IRQ hooks to the production
driver. Full boot semantics are verified separately by the production bring-up
and standalone driver suites. Native resource tests retain synthetic registered
drivers and assert that they do not execute chip operations.

All 27 maintained host suites plus the optional Windows payload audit pass.
Debug/Release builds, metadata/plists, x86_64, OpenCore vtables, binary/dSYM UUIDs,
source-layout gates and the explicit chip-file formatter dry-run pass. No bundle
was installed or loaded.

State allocation still belongs to the service endpoint binding. Firmware parsing/
upload, protection, monitoring, playback helpers and several silicon operations
remain to migrate. Independent allocation must preserve diagnostic snapshots and
borrowed firmware/register/platform lifetimes across teardown/rebind before that
ownership milestone can be marked complete. Hardware/provider/license evidence
gates remain unchanged.

## Framework extension target and service integration (2026-10-12)

The target is one shared owner/runtime with independently implemented
communication and AMP drivers. An ordinary AMP extension supplies its driver,
registry definition and verified board/firmware data, without adding a silicon
branch to the service. Communication adapters implement the existing controller
transaction interface; AMP code consumes `RegisterIO`. This target is now an
explicit contributor rule in `CONTRIBUTING.md`.

### Implemented extension boundaries

- CS35L41 registration, identity, access policy and bus framing now live in its
  own `Devices/Amplifiers/CS35L41/CS35L41DriverRegistration.hpp`. `Devices/AmplifierDrivers.hpp` composes
  module registration without defining silicon data. A new AMP follows that
  same self-contained module shape.
- Manifest-backed I2C/SPI discovery uses the registry definition's identity
  register, mask, format and optional model tag. It does not require another
  allowlist entry in `Core/AmplifierDescriptors.hpp`. A successful read must match
  the manifest's expected silicon; a missing registration or mismatched ID rejects
  that endpoint. Unconfigured legacy scans retain their narrow read-only behavior.
- `Platform/AmplifierHost.hpp` supplies common authorization, endpoint labels and
  diagnostic publication without chip/firmware-format dependencies. Every service
  context receives this borrowed host. Drivers retain their own register, firmware,
  lifecycle and playback implementations.
- A driver-specific private context requires a declared `platformContract`.
  Independent definitions request none and receive a null private context from
  the service. CS35L41 explicitly requests its temporary service bridge; registry
  and factory reject absent/wrong contracts before casting it. Unknown requested
  contracts fail instead of receiving the CS35L41 bridge by default.
- `PlatformProfile::i2cControllerFactory` permits an independently implemented
  provider-backed communication adapter. The service owns the abstract
  `I2CController` rather than the Voodoo concrete type and retains one controller
  from identity discovery through binding. A selected factory failure does not
  fall back to another provider ABI. The default custom Voodoo adapter is retained.
- Rediscovery cannot replace ports/state while a runtime is bound. An unbound
  read-only rescan releases earlier discovery resources before constructing new
  ones. Controller release follows driver/port/map/bus release.

### Concrete extension evidence

The production service discovery/binder is exercised with fixture AMP IDs
`0x1111`, `0x2222` and `0x3333`, all absent from the descriptor table. SPI fixtures
use different framing and reversed resource order. The I2C fixture uses a masked
vendor identity at register `0x10`, one-byte addresses and a completely independent
`I2CController` implementation. Their factories consume the common host, perform
initialization/firmware/playback through their own hooks, publish diagnostics and
complete suspend/resume/teardown without invoking a CS35L41 helper. Tests also
cover missing registration, identity mismatch, factory failure, stable controller
ownership while bound and repeated read-only discovery under ASan/UBSan.

These fixture IDs/controllers are not registered in the shipping device table.
All 27 maintained suites plus the optional Windows audit, Debug/Release builds,
source/Xcode gates, metadata/plists, x86_64, vtables and binary/dSYM UUIDs pass.
The host/CS35L41 files pass the explicit formatter dry-run. No bundle is installed
or loaded by this change.

### Remaining work at that milestone (superseded by completion below)

The production CS35L41 implementation still borrows service-allocated chip state
and delegates silicon/firmware/monitoring helpers through its explicitly marked
bridge. Moving those responsibilities into the AMP driver and replacing the
service's inherited CS35L41 storage with generic endpoint/status data remain open.
Generic deferred-firmware/resource/route capabilities must also be completed where
an independent production driver needs them; existing CS-specific retry behavior
is not a universal contract.

The optional I2C factory is scoped to provider-backed adapters whose provider owns
PM. It does not establish a native MMIO/DMA power/arbitration/cancellation owner.
Native I2C/MP2 and new silicon still need their specifications, provider/resource
contracts, firmware provenance/licenses and board validation. SoundWire requires
its own stream/controller subsystem. Preserve evidence gates while migrating the
current AMP to the same independent contract expected of future implementations.

## Completed independent production ownership (2026-10-12)

`CS35L41Driver` now owns `DeviceState` by value. It owns the complete four-stage
lifecycle and 42 silicon/firmware/calibration/protection/diagnostic/playback
methods previously implemented as service members. The firmware resource schema
also belongs to the AMP module. There is no chip-state pointer borrowed from the
service and no `Operation` bridge calling service hardware methods.

The service's endpoint record contains routing/identity metadata, a borrowed
register port and generic diagnostic state. `AmplifierHost` supplies authorization,
boot arguments, property publication, retained audio-controller access, HDA/route
observations and diagnostic capture guards. Kernel registry/resource lookup stays
inside the chip implementation where firmware selection needs it; the driver is
not an IOService and does not depend on `CirrusAudioFixup`.

All discovery, including legacy I2C/SPI, uses registered module identity/access/
framing metadata. Read-only identity maps are released before writable binding.
The service performs no address/value encoding and infers no channel from an
address/display label. Module callbacks supply read-only inspection and accepted
debug phases. No descriptor-table edit or silicon service branch is required for
a registered AMP. Provider-backed communication adapters implement the common
controller interface and are selected by verified platform data.

`AmplifierReport` lets service publication aggregate chip readiness without
reading DSP/firmware fields. Waiting lifecycle resources use the generic runtime
retry hook. Polling performs resource discovery only; completion detaches and
re-enters group initialization on the same driver instances, retaining context,
state/diagnostic lifetimes. Retry failure preserves the original fault and rolls
back peers. Suspend/wake and release still use the existing lifecycle safety
policy.

Validation covers the full production driver linked with fake kernel resources,
225 transfer fault positions, OTP/errata/idle, exact native identity failures,
firmware identity timeout, every debug phase, PM, diagnostic tracing and repeated
teardown/rebind, both normally and under ASan/UBSan. Detailed maintained suites
retain parser/upload/calibration/runtime fault sweeps using actual method bodies.
Synthetic AMP/controller fixtures execute production service discovery/binding
with IDs absent from the central descriptor table and generic endpoint storage.
Generic resource retry tests cover a waiting member with a ready peer, no factory
reallocation, retry failure and retained cleanup evidence.

The formatter configuration is pinned in `.clang-format`; modified/new source
files use it. Full host audit (27 maintained suites plus optional Windows audit), Debug/Release,
plist/metadata, vtable, x86_64 and binary/dSYM UUID gates passed. No new bundle was
installed or loaded.

This completes software framework ownership. It does not establish native
DesignWare/MP2, CS35L56, SoundWire or a new physical board. Those need resource/PM/
arbitration/stream contracts, firmware provenance/licenses and target validation.
The custom loaded provider's binary ABI now has concrete UUID/hash/disassembly
evidence in [provider ABI evidence](provider_abi_evidence.md); fork source revision
and distribution provenance remain unresolved. Existing embedded firmware bytes
and repository licensing are unchanged. Physical cold boot, playback, headphone
switching, sleep/wake and speaker identity remain separate validation tasks.


### Completed native communication ownership boundary

Native SPI also uses a replaceable `NativeSPIControllerOwner` contract rather than
concrete Intel/AMD pointers in `NativeSPIHost`. The owner supplies its transaction
port, suspend/resume and failure evidence. Platform data selects an optional
`spiControllerFactory`; the shipping composition selects the existing Intel/AMD
modules. A selected failure cannot fall back to another controller. The host
retains ACPI targets, resolves resources, shares one owner per target and releases
maps/buses before owners/targets. AMD D0 checks and Intel identity validation remain
module policy with the same underlying engines and register behavior.

An independent synthetic SPI owner executes production service setup and host
resource/PM paths under normal and sanitizer runs. It has no Intel/AMD base class,
uses three resources on two controller targets, shares owners correctly, handles
resume rollback and fails selected factory creation without built-in fallback.
This makes both AMP and communication ownership extensible through module
implementations and verified platform data. It adds no physical controller IDs.


Final bundle verification: Debug UUID `2E32A0FB-B690-3F39-A580-5B06CE074F49`.
Both configurations pass their matching binary/dSYM UUID and x86_64 gates. The
SDK 15.5 kmod startup objects still emit the existing deployment-floor 11.0 linker
warnings; this does not establish physical compatibility across macOS releases.

## Board-owned driver configuration and identity

`AmplifierBinding` may carry an immutable configuration pointer, size and schema name. Only the registered AMP factory interprets that view; it rejects unknown schemas and invalid electrical values before initialization. The CS35L41 module now implements its internal and legacy external power paths through this mechanism. A new silicon driver owns its own schema without adding boost fields or model branches to the service/runtime.

A `PlatformProfile` declares its identity source. Existing profiles use audio PCI identity; the exact HP SPI profile selects amplifier ACPI `_SUB`. `BoardConfiguration::useBoardFirmwareIdentity` explicitly opts into that verified board identity for firmware selection. Other profiles retain existing codec-identity behavior. No global subsystem translation is inferred from the CPU, codec model or transport.

Controller modules still own provider resources. Intel LPSS borrows the generic `SPIChipSelect` contract for each external selection, while the Intel module resolves ACPI GPIO resources and retains/maps the documented GPIO family. Unsupported GPIO providers fail binding rather than falling back to a native CS. See [HP SPI bring-up](hp_spi_bringup.md) for the current target validation boundary.

## Firmware-property discovery

`BoardDiscovery` is an optional AMP-registration contract. The host provides evaluated firmware properties and discovered endpoints; the module owns interpretation and the returned configuration. Its lifetime extends through driver teardown. CS35L41 uses this path for internal-boost HDA properties, independent of OEM name and ACPI controller path. Other modules can supply different parsers and configuration schemas. Exact property corrections are silicon-owned data. Filenames are asset provenance, not endpoint identities. See [evidence, implementation and limits](platform_evidence_audit.md).
