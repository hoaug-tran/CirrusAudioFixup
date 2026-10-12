# Kernel driver naming and ownership

The project names hardware roles and ownership explicitly. The public IOKit
service remains `CirrusAudioFixup`, with its existing bundle identity, personality,
provider contracts and global IOKit class names. Ordinary C++ helpers do not gain
an `IO` prefix: they are not new IOService classes or Apple framework objects.

## References and design choice

Apple's [IOAudioFamily source](https://github.com/apple-oss-distributions/IOAudioFamily)
uses role names such as IOAudioDevice, IOAudioEngine, IOAudioControl and
IOAudioStream. [AppleALC](https://github.com/acidanthera/AppleALC/blob/master/AppleALC/kern_alc.hpp)
uses a different project convention (`kern_alc.hpp`, `AlcEnabler`). Linux's
[CS35L41 driver](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/cs35l41.h)
identifies the actual silicon and audio-driver role. These are reference designs,
not evidence of one mandatory cross-project filename standard.

This kext uses PascalCase C++ role names and matching headers, with explicit
subsystems and chip names at module boundaries. Its existing architecture is a
registered driver factory plus lifecycle composition; RegisterIO/RegisterBus
separate silicon operations from transport, and controller owners enforce resource
and PM lifetime. Renaming documents that architecture without creating another
abstraction layer or claiming another supported codec.

## Directory responsibilities

| Directory | Responsibility |
| --- | --- |
| `Core` | Bus/register interfaces, device identity, lifecycle state machine and amplifier group coordination |
| `Devices/Amplifiers/<Chip>` | Independent amplifier driver, silicon state, register/OTP/errata definitions, firmware/tuning and registration |
| `Devices/AmplifierDrivers.hpp` | Statically linked amplifier-driver composition |
| `Transport` | Controller contracts, I2C/SPI register buses, ACPI resources and communication-provider adapters |
| `Transport/Intel`, `Transport/AMD` | Controller IP engines, controller binding and PM resource owners |
| `Platform` | Board identity/authorization, driver context/registry, instance management and borrowed host facilities |
| `Platform/HDA` | Host-audio observations and IOAudioFamily event integration |
| `Firmware` | Shared firmware bundle schema and WMFW parser/upload format support |
| `Diagnostics` | Structured failures, attachment evidence and transfer history |
| `Support` | Build identity, logging, CPU metadata and small shared utilities |
| `Tests`, `Tools`, `docs` | Host verification, build/evidence utilities and documentation |

Future codec implementations belong under `Devices/Codecs/<Chip>` when they
exist. No empty codec module or matching personality is introduced to imply
support. Future communication standards add their own controller/transport
contracts; they do not live inside amplifier directories.

## Role vocabulary

| Name | Meaning |
| --- | --- |
| `CS35L41Driver` | Complete independent chip lifecycle and owned live state |
| `CS35L41Device` | Lower-level silicon identification/reset/configuration helper |
| `CS35L41DriverFactory` | Validated creation, destruction and read-only inspection |
| `CS35L41DriverRegistration` | Immutable matching, formats, access policy and factory registration |
| `AmplifierDriverRegistration`, `AmplifierDriverRegistry` | Generic registration schema and bounded static registry |
| `AmplifierDriverContext` | Borrowed register/host/board/firmware contract for an instance |
| `AmplifierManager` | Owns selected driver instances and coordinates group lifecycle |
| `AmplifierHostAdapter` | Adapts the service's serialized facilities to the borrowed host interface |
| `SPIAmplifierManager` | SPI host ownership combined with amplifier-instance management |
| `IntelSPIControllerOwner`, `AMDSPIControllerOwner` | Own controller binding and PM lifetime around the existing IP engines |
| `SPIControllerDrivers`, `createNativeSPIController` | Shipping controller selection and validated owner creation |
| `SiliconState`, `DeviceState` | Chip state versus chip endpoint/register views, distinct from service endpoint metadata |

A factory constructs an object; a registration describes how to match/create it;
a registry stores registrations; a manager owns instances. A provider is an IOKit
or addressed-transfer contract, a controller owns communication operations, and
a register bus implements framing. Keep these terms distinct in future modules.
Generic hooks retain their existing `AmplifierLifecycleHooks` name because they
are the operations invoked by the common lifecycle state machine.

The naming gate rejects the obsolete implementation-role term in production
filenames and symbols. Source paths, namespaces, test fixtures, Xcode navigation
and compilation membership follow the same directory structure. Internal failure
reason labels use DRIVER terminology; stable `Cirrus_*` property keys, provider
ABI, boot arguments, hardware identities and firmware selection remain unchanged.
Vendored dependencies and user-supplied collection scripts retain their original
names and ownership.

Service operations that serve both I2C and SPI are named `setupCommunication`,
`teardownCommunication`, `communicationReady`, `suspendCommunication` and
`resumeCommunication`. `bindAmplifierDrivers` and `releaseAmplifierDrivers`
describe instance ownership operations. Native SPI types remain explicitly SPI:
shared service operations must not imply the wrong transport.

All 74 production C++ files also use the pinned formatter. The rename comparison
normalizes whitespace, include sorting and macro line-splicing; it verifies that
formatting does not alter operation tokens or embedded payload values.
